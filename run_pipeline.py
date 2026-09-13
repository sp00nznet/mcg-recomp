#!/usr/bin/env python3
"""
MechCommander Gold static recompilation — Phase 1 codegen.

Drives the shared pcrecomp lifter over the function table recovered from
MCX.EXE's own CodeView stream (see tools/cv_extract.py). Two things differ from
the usual project driver, both because this target shipped its symbols:

  * function bounds come from the table, and 2,038 of them are exact to the byte
    (FPO / proc records) rather than "up to the next entry";
  * generated functions keep the original names, so the output is greppable —
    `Mech__handleCollision_00641A30` rather than `sub_00641A30`.

Usage: py -3.11 run_pipeline.py [MCX.EXE] [out_dir] [functions_per_file]
"""
import json
import os
import re
import sys
import time

_here = os.path.dirname(os.path.abspath(__file__))
_pc = os.path.join('G:', os.sep, 'recomp', 'pc', 'tools', 'tools')
sys.path.insert(0, os.path.join(_pc, 'pe'))
sys.path.insert(0, os.path.join(_pc, 'lift'))

from pe_analyze import analyze_pe, build_iat_map            # noqa: E402
from lift32 import Lifter, FUNCTION_LOCALS                   # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_32             # noqa: E402
from capstone.x86 import X86_OP_IMM                          # noqa: E402

COND_JUMPS = {'je', 'jne', 'jz', 'jnz', 'ja', 'jae', 'jb', 'jbe', 'jg', 'jge',
              'jl', 'jle', 'js', 'jns', 'jo', 'jno', 'jp', 'jnp', 'jcxz',
              'jecxz'}

# The lifter emits FPU-compare branches as 1-arg CMP_xx(_fpu_cmp), but CMP_xx
# are 2-arg integer-flag macros. _fpu_cmp is -1/0/1, so rewrite the 1-arg forms
# into a direct comparison against 0.
FPU_CMP = {'EQ': '==', 'NE': '!=', 'B': '<', 'BE': '<=', 'A': '>', 'AE': '>=',
           'L': '<', 'LE': '<=', 'G': '>', 'GE': '>='}


# --------------------------------------------------------------------------
def c_name(entry):
    """A readable, guaranteed-unique C identifier for a lifted function.

    MSVC decorated names are `?method@Class@@<type stuff>`, so the leading
    @-separated fields give the readable half without demangling. The address
    suffix makes collisions impossible, which is why there is no dedupe pass.
    """
    sym = entry['name']
    addr = entry['address_int']
    if sym.startswith('?'):
        parts = sym[1:].split('@')
        method = parts[0]
        cls = parts[1] if len(parts) > 1 and parts[1] else ''
        # `??0Class@@...` is a constructor, `??1` a destructor, `??_G`/`??_E`
        # deleting destructors -- their first field is empty or a digit.
        if not method or method[0].isdigit() or method.startswith('_'):
            special = {'0': 'ctor', '1': 'dtor', '_G': 'vdtor', '_E': 'vdtor'}
            method = special.get(method, 'op')
            cls = parts[1] if len(parts) > 1 else ''
        stem = f'{cls}__{method}' if cls else method
    else:
        stem = sym.lstrip('_').split('@')[0]
    stem = re.sub(r'[^A-Za-z0-9_]', '_', stem) or 'sub'
    if stem[0].isdigit():          # __87except -> _87except
        stem = '_' + stem
    return f'{stem}_{addr:08X}'


class LinearInstruction:
    __slots__ = ['address', 'size', 'mnemonic', 'op_str', 'bytes', 'operands',
                 'is_call', 'is_ret', 'is_cond_jump', 'is_uncond_jump', 'is_jump']

    def __init__(self, insn):
        self.address = insn.address
        self.size = insn.size
        self.mnemonic = insn.mnemonic
        self.op_str = insn.op_str
        self.bytes = bytes(insn.bytes)
        self.operands = list(insn.operands) if insn.operands else []
        self.is_call = insn.mnemonic == 'call'
        self.is_ret = insn.mnemonic in ('ret', 'retn', 'retf')
        self.is_cond_jump = insn.mnemonic in COND_JUMPS
        self.is_uncond_jump = insn.mnemonic == 'jmp'
        self.is_jump = self.is_cond_jump or self.is_uncond_jump

    @property
    def end_address(self):
        return self.address + self.size

    def get_branch_target(self):
        if self.operands and self.operands[0].type == X86_OP_IMM:
            return self.operands[0].imm & 0xFFFFFFFF
        return None


def linear_disassemble(md, code_data, code_start, lo, hi):
    offset = lo - code_start
    size = hi - lo
    if offset < 0 or offset + size > len(code_data):
        return [], set()
    insns, leaders = [], {lo}
    for insn in md.disasm(code_data[offset:offset + size], lo):
        li = LinearInstruction(insn)
        insns.append(li)
        if li.is_jump:
            t = li.get_branch_target()
            if t and lo <= t < hi:
                leaders.add(t)
            leaders.add(li.end_address)
        if li.mnemonic == 'int3':
            break
    return insns, leaders


def lift_function(lifter, name, insns, leaders):
    # The per-function scratch list belongs to the lifter, not to us: `ebp` and
    # the x87 stack (_st/_fp_top/_fpu_cw) are deliberately GLOBAL in
    # recomp_types.h, because an optimising compiler splits one frame across
    # blocks that all address it through the same ebp. Declaring them locally
    # here -- which is what an older driver did -- silently zeroes the frame
    # pointer at the top of every such block.
    lines = [f'void {name}(void) {{']
    lines += [f'    {d}' for d in FUNCTION_LOCALS]
    lines.append('')
    lifter._flag_state = None
    for insn in insns:
        if insn.address in leaders:
            lines.append(f'L_{insn.address:08X}:')
        for line in lifter.lift_instruction(insn):
            lines.append(f'    {line}')
    if insns and not insns[-1].is_ret:
        lines.append('    return; /* falls through */')

    # Cross-function jumps reference labels outside this function's range; emit
    # them as indirect tail-calls so the C compiles and dispatch resolves them
    # at runtime.
    body = '\n'.join(lines)
    defined = set(re.findall(r'(?m)^\s*(L_[0-9A-Fa-f]{8})\s*:', body))
    refed = set(re.findall(r'goto\s+(L_[0-9A-Fa-f]{8})', body))
    for lbl in sorted(refed - defined):
        lines.append(f'    {lbl}: RECOMP_ITAIL(0x{int(lbl[2:], 16):08X}u); return;')
    lines.append('}')
    out = '\n'.join(lines)
    return re.sub(r'CMP_(\w+)\(_fpu_cmp\)',
                  lambda m: f'((_fpu_cmp) {FPU_CMP.get(m.group(1), "==")} 0)', out)


def write_chunk(out, idx, chunk):
    path = os.path.join(out, f'recomp_{idx:04d}.c')
    with open(path, 'w', encoding='utf-8') as f:
        f.write('/* MechCommander Gold recompilation - generated - DO NOT EDIT */\n')
        f.write(f'/* File {idx}: {len(chunk)} functions */\n\n')
        f.write('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n')
        f.write('#include "recomp_funcs.h"\n#include <math.h>\n#include <string.h>\n\n')
        for code, entry, name in chunk:
            sig = entry.get('demangled') or entry['name']
            mod = (entry.get('module') or '?').replace('.\\relnodbg\\', '')
            f.write(f'/* {entry["address"]}  {mod}\n   {sig} */\n')
            f.write(code + '\n\n')


# --------------------------------------------------------------------------
def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_here, 'MCX.EXE')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(_here, 'src', 'recomp', 'gen')
    split = int(sys.argv[3]) if len(sys.argv) > 3 else 500
    os.makedirs(out, exist_ok=True)

    print('=== MechCommander Gold static recompilation (Phase 1) ===', flush=True)
    info = analyze_pe(exe)
    iat = build_iat_map(info)
    print(f'[*] base=0x{info.image_base:08X} '
          f'code=0x{info.code_start:08X}-0x{info.code_end:08X} IAT={len(iat)}')

    with open(exe, 'rb') as fh:
        pe_data = fh.read()
    text = [s for s in info.sections if s.name == '.text'][0]
    code_data = pe_data[text.raw_offset:
                        text.raw_offset + min(text.virtual_size, text.raw_size)]
    cs, ce = info.code_start, info.code_end

    with open(os.path.join(_here, 'config', 'functions.json'), encoding='utf-8') as fh:
        table = json.load(fh)
    entries = sorted((e for e in table if cs <= e['address_int'] < ce),
                     key=lambda e: e['address_int'])
    exact = sum(1 for e in entries if e['exact_size'])
    print(f'[*] {len(entries)} functions from the CodeView table '
          f'({exact} with an exact size)')

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    lifter = Lifter(iat_map=iat)

    all_entries, stats, chunk = [], [], []
    errors = idx = 0
    t0 = time.time()
    for e in entries:
        addr = e['address_int']
        end = min(addr + e['size'], ce)
        # A one-byte function is a bare `ret` -- an empty virtual override, of
        # which this binary has 104. They sit in vtables and must be lifted, or
        # dispatch has no target when one is called.
        if end - addr < 1:
            continue
        name = c_name(e)
        try:
            insns, leaders = linear_disassemble(md, code_data, cs, addr, end)
            if not insns:
                continue
            # Trim padding after a ret that nothing jumps back into.
            trimmed, seen_ret = [], False
            for ins in insns:
                if ins.mnemonic == 'int3':
                    break
                if seen_ret:
                    if ins.address not in leaders:
                        continue
                    seen_ret = False
                trimmed.append(ins)
                if ins.is_ret:
                    seen_ret = True
            if not trimmed:
                continue
            code = lift_function(lifter, name, trimmed, leaders)
            stats.append({'address': e['address'], 'name': name,
                          'symbol': e['name'], 'module': e.get('module'),
                          'num_instructions': len(trimmed)})
        except Exception as exc:                            # noqa: BLE001
            code = f'/* LIFT ERROR {name}: {exc} */\nvoid {name}(void) {{}}\n'
            errors += 1
        chunk.append((code, e, name))
        all_entries.append((addr, name))
        if len(chunk) >= split:
            write_chunk(out, idx, chunk)
            idx += 1
            chunk = []
            print(f'[*]   {len(all_entries)}/{len(entries)} ({errors} err)', flush=True)
    if chunk:
        write_chunk(out, idx, chunk)
        idx += 1

    with open(os.path.join(out, 'recomp_funcs.h'), 'w', encoding='utf-8') as f:
        f.write('/* MechCommander Gold recompilation - generated */\n'
                '#pragma once\n#include <stdint.h>\n\n')
        for a, n in all_entries:
            f.write(f'void {n}(void);  /* 0x{a:08X} */\n')

    with open(os.path.join(out, 'recomp_dispatch.c'), 'w', encoding='utf-8') as f:
        f.write('/* MechCommander Gold recompilation - generated */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n')
        f.write('const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(all_entries):
            f.write(f'    {{ 0x{a:08X}u, {n} }},\n')
        f.write('};\n\n'
                f'const uint32_t recomp_dispatch_count = {len(all_entries)};\n')

    # The lifter and recomp_types.h are two halves of one contract -- copy it
    # from the toolbox on every run rather than keeping a checked-in copy that
    # can go stale against the macros the lifter just emitted.
    header = os.path.join(os.path.dirname(_pc), 'runtime', 'recomp32', 'recomp_types.h')
    with open(header, 'rb') as src, \
            open(os.path.join(out, 'recomp_types.h'), 'wb') as dst:
        dst.write(src.read())

    # Count only what we generated. Build output lands here too, and a .obj
    # counted as "lines of C" doubled the reported size once already.
    total_lines = total_bytes = 0
    for fn in os.listdir(out):
        fp = os.path.join(out, fn)
        if os.path.isfile(fp) and fn.endswith(('.c', '.h')):
            total_bytes += os.path.getsize(fp)
            with open(fp, encoding='utf-8', errors='replace') as fh:
                total_lines += sum(1 for _ in fh)

    with open(os.path.join(_here, 'analysis', 'phase1_codegen.json'), 'w',
              encoding='utf-8') as f:
        json.dump({'functions': len(all_entries), 'errors': errors,
                   'files': idx, 'lines': total_lines, 'bytes': total_bytes,
                   'entries': stats}, f, indent=1)

    print('=' * 60)
    print(f'  functions: {len(all_entries):,}   lift errors: {errors}   files: {idx}')
    print(f'  lines of C: {total_lines:,}   size: {total_bytes / 1048576:.1f} MB'
          f'   time: {time.time() - t0:.1f}s')
    print('=' * 60)


if __name__ == '__main__':
    main()
