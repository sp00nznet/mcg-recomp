#!/usr/bin/env python3
"""
Extract the CodeView (NB11) debug information MCX.EXE ships in its overlay, and
turn it into the recomp's function table.

MCX.EXE was linked from a `RelNoDbg` build that nonetheless kept the linker's
CodeView output appended to the image: 612 object modules, 12k+ public symbols
carrying MSVC C++ decorated names, per-module source file paths, and an FPO
table. That is the whole function table -- there is no discovery heuristic here
and none is needed.

Outputs (paths relative to the repo root):
    config/functions.json     lifter-compatible entry list  (committed)
    analysis/cv_symbols.json  every public symbol, all segments
    analysis/cv_modules.json  the 612 object modules + source paths
    analysis/fpo.json         frame/prologue records, used to tighten bounds

Usage:  py -3.11 tools/cv_extract.py [MCX.EXE] [repo_root]
        py -3.11 tools/cv_extract.py --selftest
"""
import ctypes
import json
import os
import struct
import sys

# --- CodeView subsection types (cvinfo.h, 32-bit "sst" values) ---
SST_MODULE = 0x120
SST_ALIGNSYM = 0x125
SST_SRCMODULE = 0x127
SST_GLOBALPUB = 0x12A
SST_SEGMAP = 0x12D

S_PUB32 = 0x1009
S_LPROC32 = 0x100A
S_GPROC32 = 0x100B

IMAGE_DEBUG_TYPE_CODEVIEW = 2
IMAGE_DEBUG_TYPE_FPO = 3


# --------------------------------------------------------------------------
# PE bits. Deliberately hand-rolled: this reads the raw file at byte offsets,
# and pefile's RVA helpers assume a loaded image.
# --------------------------------------------------------------------------
class PE:
    def __init__(self, data):
        self.data = data
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        self.n_sections = struct.unpack_from('<H', data, pe + 6)[0]
        opt_size = struct.unpack_from('<H', data, pe + 20)[0]
        opt = pe + 24
        self.image_base = struct.unpack_from('<I', data, opt + 28)[0]
        self.entry_rva = struct.unpack_from('<I', data, opt + 16)[0]
        self.sections = []
        for i in range(self.n_sections):
            o = opt + opt_size + i * 40
            name = data[o:o + 8].rstrip(b'\0').decode('latin1')
            vsize, vaddr, rsize, raw = struct.unpack_from('<IIII', data, o + 8)
            self.sections.append({'name': name, 'vaddr': vaddr, 'vsize': vsize,
                                  'raw': raw, 'rsize': rsize})
        self.debug_dirs = self._debug_dirs(opt + 96 + 6 * 8)

    def rva_to_file(self, rva):
        for s in self.sections:
            if s['vaddr'] <= rva < s['vaddr'] + max(s['vsize'], s['rsize']):
                return s['raw'] + (rva - s['vaddr'])
        return None

    def _debug_dirs(self, dd_off):
        rva, size = struct.unpack_from('<II', self.data, dd_off)
        if not size:
            return []
        off = self.rva_to_file(rva)
        out = []
        for i in range(size // 28):
            _, _, _, _, typ, sz, _, ptr = struct.unpack_from(
                '<IIHHIIII', self.data, off + i * 28)
            out.append({'type': typ, 'size': sz, 'ptr': ptr})
        return out


def pstr(data, off):
    """CodeView length-prefixed string."""
    n = data[off]
    return data[off + 1:off + 1 + n].decode('latin1'), off + 1 + n


# --------------------------------------------------------------------------
# Demangling. MSVC's own UnDecorateSymbolName gets this exactly right and
# ships with Windows, so there is nothing to write and nothing to install.
# --------------------------------------------------------------------------
_dbghelp = None


def demangle(name):
    """MSVC decorated name -> readable signature. None if not decorated."""
    global _dbghelp
    if not name.startswith('?'):
        return None
    if _dbghelp is None:
        try:
            _dbghelp = ctypes.WinDLL('dbghelp.dll')
            _dbghelp.UnDecorateSymbolName.argtypes = [
                ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint]
        except (OSError, AttributeError):
            _dbghelp = False
    if _dbghelp is False:
        return None
    buf = ctypes.create_string_buffer(4096)
    # UNDNAME_COMPLETE(0): keep the full signature -- class, params, convention.
    if _dbghelp.UnDecorateSymbolName(name.encode('latin1'), buf, 4096, 0):
        return buf.value.decode('latin1')
    return None


# --------------------------------------------------------------------------
class CodeView:
    def __init__(self, pe):
        self.pe = pe
        self.data = pe.data
        cv = next((d for d in pe.debug_dirs
                   if d['type'] == IMAGE_DEBUG_TYPE_CODEVIEW), None)
        if cv is None:
            raise SystemExit('no IMAGE_DEBUG_TYPE_CODEVIEW directory here')
        self.base = cv['ptr']
        sig = self.data[self.base:self.base + 4]
        if sig != b'NB11':
            raise SystemExit(f'unsupported CodeView signature {sig!r}, want NB11')
        lfo_dir = struct.unpack_from('<I', self.data, self.base + 4)[0]
        o = self.base + lfo_dir
        hdr_size, ent_size = struct.unpack_from('<HH', self.data, o)
        count = struct.unpack_from('<I', self.data, o + 4)[0]
        self.entries = [
            struct.unpack_from('<HHII', self.data, o + hdr_size + i * ent_size)
            for i in range(count)]

    def _of_type(self, sst):
        return [e for e in self.entries if e[0] == sst]

    def seg_to_rva(self, seg):
        """CodeView segment index is 1-based over the PE section table."""
        if 1 <= seg <= len(self.pe.sections):
            return self.pe.sections[seg - 1]['vaddr']
        return None

    def modules(self):
        """The object files, with their per-segment extents."""
        out = []
        for _sst, imod, lfo, _cb in self._of_type(SST_MODULE):
            p = self.base + lfo
            _, _, c_segs = struct.unpack_from('<HHH', self.data, p)
            q = p + 8
            extents = []
            for _ in range(c_segs):
                seg, _pad, off, size = struct.unpack_from('<HHII', self.data, q)
                q += 12
                extents.append({'seg': seg, 'offset': off, 'size': size})
            name, _ = pstr(self.data, q)
            out.append({'index': imod, 'obj': name, 'segments': extents})
        out.sort(key=lambda m: m['index'])
        return out

    def source_files(self):
        """imod -> original source path, e.g. the L:\\mcx\\ tree."""
        out = {}
        for _sst, imod, lfo, _cb in self._of_type(SST_SRCMODULE):
            p = self.base + lfo
            c_file = struct.unpack_from('<H', self.data, p)[0]
            if not c_file:
                continue
            base = struct.unpack_from('<I', self.data, p + 4)[0]
            fp = p + base
            c_fileseg = struct.unpack_from('<H', self.data, fp)[0]
            name_off = fp + 4 + c_fileseg * 4 + c_fileseg * 8
            name, _ = pstr(self.data, name_off)
            out[imod] = name
        return out

    def publics(self):
        """Every S_PUB32 in the global public table."""
        secs = self._of_type(SST_GLOBALPUB)
        if not secs:
            return []
        _sst, _imod, lfo, _cb = secs[0]
        p = self.base + lfo
        cb_symbol = struct.unpack_from('<I', self.data, p + 4)[0]
        q = p + 16
        end = q + cb_symbol
        out = []
        while q < end:
            length, rectyp = struct.unpack_from('<HH', self.data, q)
            if length == 0:
                break
            if rectyp == S_PUB32:
                # S_PUB32: typind(u32) off(u32) seg(u16) name(pstr)
                _ti, off, seg = struct.unpack_from('<IIH', self.data, q + 4)
                name, _ = pstr(self.data, q + 14)
                out.append({'seg': seg, 'offset': off, 'name': name})
            q += length + 2
        return out

    def procs(self):
        """S_GPROC32/S_LPROC32 carry an exact byte length. Only some modules
        have them, but where present they beat next-symbol-start bounds."""
        out = []
        for _sst, imod, lfo, cb in self._of_type(SST_ALIGNSYM):
            p = self.base + lfo
            q = p + 4
            end = p + cb
            while q < end:
                length, rectyp = struct.unpack_from('<HH', self.data, q)
                if length == 0:
                    break
                if rectyp in (S_GPROC32, S_LPROC32):
                    proc_len = struct.unpack_from('<I', self.data, q + 16)[0]
                    off = struct.unpack_from('<I', self.data, q + 32)[0]
                    seg = struct.unpack_from('<H', self.data, q + 36)[0]
                    name, _ = pstr(self.data, q + 39)
                    out.append({'imod': imod, 'seg': seg, 'offset': off,
                                'size': proc_len, 'name': name})
                q += length + 2
        return out


def parse_fpo(pe):
    """IMAGE_FPO_DATA, 16 bytes each: an exact start+size per record."""
    d = next((x for x in pe.debug_dirs if x['type'] == IMAGE_DEBUG_TYPE_FPO), None)
    if d is None:
        return []
    out = []
    for i in range(d['size'] // 16):
        o = d['ptr'] + i * 16
        start, size, n_locals = struct.unpack_from('<III', pe.data, o)
        n_params, bits = struct.unpack_from('<HH', pe.data, o + 12)
        out.append({
            'rva': start, 'size': size, 'locals': n_locals, 'params': n_params,
            'prolog': bits & 0xFF, 'saved_regs': (bits >> 8) & 7,
            'has_seh': bool((bits >> 11) & 1), 'uses_bp': bool((bits >> 12) & 1),
            'frame_type': (bits >> 14) & 3,
        })
    return out


# --------------------------------------------------------------------------
def build(exe_path, root):
    with open(exe_path, 'rb') as fh:
        data = fh.read()
    pe = PE(data)
    cv = CodeView(pe)

    modules = cv.modules()
    srcfiles = cv.source_files()
    for m in modules:
        m['source'] = srcfiles.get(m['index'])

    text = pe.sections[0]
    text_lo = pe.image_base + text['vaddr']
    text_hi = text_lo + text['vsize']

    # A symbol's segment says code vs data outright, so nothing is guessed.
    syms = []
    for s in cv.publics():
        rva = cv.seg_to_rva(s['seg'])
        if rva is None:
            continue
        syms.append({'va': pe.image_base + rva + s['offset'],
                     'seg': s['seg'], 'name': s['name'],
                     'section': pe.sections[s['seg'] - 1]['name']})
    syms.sort(key=lambda s: s['va'])

    # Exact sizes where a proc or FPO record covers the entry, else the next
    # entry's start. Bounds only ever shrink, so they can never overlap.
    exact = {}
    for p in cv.procs():
        rva = cv.seg_to_rva(p['seg'])
        if rva is not None:
            exact[pe.image_base + rva + p['offset']] = p['size']
    fpo = parse_fpo(pe)
    for f in fpo:
        exact.setdefault(pe.image_base + f['rva'], f['size'])

    # MSVC6 folds identical COMDATs (/OPT:ICF), so several public symbols can
    # share one address. That is one function with several names, not several
    # functions -- keep the decorated name as primary and carry the rest as
    # aliases, so a call through any of them still resolves.
    by_va = {}
    for s in syms:
        if text_lo <= s['va'] < text_hi:
            by_va.setdefault(s['va'], []).append(s['name'])

    code = sorted(by_va)
    functions = []
    for i, va in enumerate(code):
        names = sorted(by_va[va], key=lambda n: (not n.startswith('?'), len(n)))
        nxt = code[i + 1] if i + 1 < len(code) else text_hi
        size = min(exact.get(va, nxt - va), nxt - va)
        functions.append({
            'address': f'0x{va:08X}',
            'address_int': va,
            'name': names[0],
            'size': size,
            'demangled': demangle(names[0]),
            'exact_size': va in exact,
            'aliases': names[1:],
        })

    os.makedirs(os.path.join(root, 'config'), exist_ok=True)
    os.makedirs(os.path.join(root, 'analysis'), exist_ok=True)

    def dump(rel, obj):
        path = os.path.join(root, rel)
        with open(path, 'w', encoding='utf-8') as fh:
            json.dump(obj, fh, indent=1)
        print(f'  {rel:<28} {len(obj):>7,} records  '
              f'{os.path.getsize(path) / 1e6:.1f} MB')

    print(f'{os.path.basename(exe_path)}: base 0x{pe.image_base:08X}, '
          f'.text 0x{text_lo:08X}-0x{text_hi:08X}, '
          f'entry 0x{pe.image_base + pe.entry_rva:08X}')
    print(f'  CodeView NB11 at file offset 0x{cv.base:X}, '
          f'{len(cv.entries)} subsections')
    dump('config/functions.json', functions)
    dump('analysis/cv_symbols.json', syms)
    dump('analysis/cv_modules.json', modules)
    dump('analysis/fpo.json', fpo)

    named = sum(1 for f in functions if f['demangled'])
    covered = sum(f['size'] for f in functions)
    print(f'  {len(functions):,} functions, {named:,} with C++ signatures, '
          f'{len(exact):,} with an exact size')
    print(f'  {covered:,} of {text["vsize"]:,} .text bytes covered '
          f'({100 * covered / text["vsize"]:.1f}%)')
    return functions, syms, modules


def selftest():
    """The invariants that matter: bounds never overlap, every entry lands in
    .text, and the PE entry point is one of the functions we found."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = os.path.join(root, 'MCX.EXE')
    with open(exe, 'rb') as fh:
        pe = PE(fh.read())
    funcs, _syms, mods = build(exe, root)

    text = pe.sections[0]
    lo = pe.image_base + text['vaddr']
    hi = lo + text['vsize']
    for a, b in zip(funcs, funcs[1:]):
        assert a['address_int'] < b['address_int'], 'function table not sorted'
        assert a['address_int'] + a['size'] <= b['address_int'], \
            f'overlap at {a["address"]} ({a["name"]})'
    for f in funcs:
        assert lo <= f['address_int'] < hi, f'{f["address"]} outside .text'
        assert f['size'] > 0, f'{f["address"]} has zero size'

    entry = pe.image_base + pe.entry_rva
    assert any(f['address_int'] == entry for f in funcs), \
        f'entry point 0x{entry:08X} is not a known function'

    sample = next(f for f in funcs if f['demangled'])
    assert '(' in sample['demangled'], f'demangle gave {sample["demangled"]!r}'

    assert len(mods) > 500, f'only {len(mods)} modules'
    assert any(m['source'] for m in mods), 'no source paths recovered'
    print('selftest: ok')


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        selftest()
    else:
        here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, 'MCX.EXE')
        build(exe, sys.argv[2] if len(sys.argv) > 2 else here)
