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
import struct

_here = os.path.dirname(os.path.abspath(__file__))
_pc = os.path.join('G:', os.sep, 'recomp', 'pc', 'tools', 'tools')
sys.path.insert(0, os.path.join(_pc, 'pe'))
sys.path.insert(0, os.path.join(_pc, 'lift'))

from pe_analyze import analyze_pe, build_iat_map            # noqa: E402
from lift32 import Lifter, FUNCTION_LOCALS                   # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_32             # noqa: E402
from capstone.x86 import X86_OP_IMM, X86_OP_MEM      # noqa: E402

COND_JUMPS = {'je', 'jne', 'jz', 'jnz', 'ja', 'jae', 'jb', 'jbe', 'jg', 'jge',
              'jl', 'jle', 'js', 'jns', 'jo', 'jno', 'jp', 'jnp', 'jcxz',
              'jecxz'}

# The lifter emits FPU-compare branches as 1-arg CMP_xx(_fpu_cmp), but CMP_xx
# are 2-arg integer-flag macros. _fpu_cmp is -1/0/1, so rewrite the 1-arg forms
# into a direct comparison against 0.
FPU_CMP = {'EQ': '==', 'NE': '!=', 'B': '<', 'BE': '<=', 'A': '>', 'AE': '>=',
           'L': '<', 'LE': '<=', 'G': '>', 'GE': '>='}

BALANCE_CALL = re.compile(r'^RECOMP_(I?)CALL\((.+?)\);')


def _balance(va):
    """Wrap a direct call so that a stack leak names its own call site.

    RECOMP_CALL pushes the dummy return address and the callee's `ret` pops it,
    plus its arguments for a stdcall. So esp after a call is never LOWER than
    before it -- unless the callee failed to pop at all, which leaks 4 bytes and
    only shows up much later, as a poisoned value in a callee-saved register
    somewhere unrelated. Checking at the call site turns that into a name.

    Compiles away without -DRECOMP_TRACE.
    """
    def sub(m):
        indirect, target = m.group(1), m.group(2)
        # ebp is callee-saved, so a call must give it back unchanged. It is
        # checked alongside esp because the two fail differently: esp drifts
        # when a `ret` is dropped, ebp when a body's push/pop pair does not
        # survive its own lift -- and a zeroed ebp turns every frame-relative
        # access into a small absolute address.
        if indirect:
            # Evaluate the target once into a temporary: it names the callee in
            # the report, which is what turns "something inside _initterm" into
            # the one static constructor that misbehaved.
            return (f'{{ uint32_t _sp0 = g_esp, _bp0 = ebp, '
                    f'_tg = (uint32_t)({target}); RECOMP_PUSHSITE(0x{va:08X}u); '
                    f'RECOMP_ICALL(_tg); '
                    f'RECOMP_BALANCE2(_sp0, _bp0, 0x{va:08X}u, _tg); }}')
        return (f'{{ uint32_t _sp0 = g_esp, _bp0 = ebp; '
                f'RECOMP_PUSHSITE(0x{va:08X}u); RECOMP_CALL({target}); '
                f'RECOMP_BALANCE(_sp0, _bp0, 0x{va:08X}u); }}')
    return sub


# Functions the runtime replaces, in recomp_lookup_manual(). The bodies are
# still generated -- they cost nothing unused, and keep the dispatch table
# complete -- but callers reach them indirectly so the override takes effect.
# Keep this list short and say why each one is here.
OVERRIDES = {
    0x006440B0: 'GeneralMsg(char*) -- print the message, skip the %f path',
    0x006440D0: 'FatalMsg(char*)   -- ditto; x87 80-bit formatting derails',
    0x00644180: 'Fatal(uint,char*,char*) -- report and stop',
    0x00644250: 'Assert -- surface the engine diagnostic; the lift clobbers ebp',
    0x00760D27: '_initterm -- probe: every ctor must leave esp exactly as found',
    0x00763EDA: '__fassign -- strtod natively; the long double path zeroed ebp',
    0x006DD410: 'BriefingScreen::setUpDeploy -- report what the Ready Area got',
    0x006DD7C0: 'MechBriefBlock::init -- report where each Ready Area entry lands',
    0x006BB4D0: 'ObjectMap::addObject -- report the map cell index it derives',
}


# --------------------------------------------------------------------------
def c_name(entry):
    """A readable, guaranteed-unique C identifier for a lifted function.

    MSVC decorated names are `?method@Class@@<type stuff>`, so the leading
    @-separated fields give the readable half without demangling. The address
    suffix makes collisions impossible, which is why there is no dedupe pass.
    """
    sym = entry['name']
    addr = entry['address_int']
    if not sym:                      # discovered by call-graph, no symbol
        return f'sub_{addr:08X}'
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


def _switch_tables(md, code_data, code_start, lo, hi):
    """Find jump tables embedded in this function's own bytes.

    MSVC compiles a dense switch into `jmp dword ptr [reg*4 + table]` with the
    table sitting in .text, usually right after the code that uses it. Decoded
    linearly those dwords become nonsense instructions, and -- far worse -- the
    decode goes out of phase, so every real instruction after the table lands at
    the wrong address. `MasterComponent::initEXCEL` fails that way: its computed
    jump targets an address that no longer has a label, because the label ended
    up mid-instruction.

    Returns {table_start: end} plus the case targets, which are real branch
    destinations and need labels.
    """
    tables, targets = {}, set()
    # A dense MSVC switch emits TWO tables: a byte index table read with
    # `movzx r, byte ptr [r + disp]`, then the dword target table the jmp uses.
    # Only the second is reachable from the jmp, so the first used to be
    # decoded as code -- and in translateMessage those bytes disassemble to
    # `popal`, which pops every register including ebp. Collect the byte-table
    # displacements here and close the gap below.
    byte_tables = set()
    for insn in md.disasm(code_data[lo - code_start:hi - code_start], lo):
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            src = insn.operands[1]
            if (src.type == X86_OP_MEM and src.size == 1
                    and lo <= (src.mem.disp & 0xFFFFFFFF) < hi):
                byte_tables.add(src.mem.disp & 0xFFFFFFFF)
        if insn.mnemonic != 'jmp' or not insn.operands:
            continue
        op = insn.operands[0]
        if op.type != X86_OP_MEM or op.mem.scale != 4 or op.mem.index == 0:
            continue
        start = op.mem.disp & 0xFFFFFFFF
        if not (lo <= start < hi):
            continue
        # A byte table immediately preceding this one runs up to it.
        for b in byte_tables:
            if b < start:
                tables[b] = start
        # Entries run until one stops being an address inside this function.
        end = start
        while end + 4 <= hi:
            word = struct.unpack_from('<I', code_data, end - code_start)[0]
            if not (lo <= word < hi):
                break
            targets.add(word)
            end += 4
        if end > start:
            tables[start] = end
    return tables, targets


def linear_disassemble(md, code_data, code_start, lo, hi):
    offset = lo - code_start
    size = hi - lo
    if offset < 0 or offset + size > len(code_data):
        return [], set()

    tables, targets = _switch_tables(md, code_data, code_start, lo, hi)
    insns, leaders = [], {lo}
    leaders |= targets

    pos = lo
    while pos < hi:
        if pos in tables:                 # skip the table's bytes, not decode them
            pos = tables[pos]
            continue
        # Decode up to the next table (or the end), so a table never gets
        # swallowed as the tail of an instruction.
        stop = min([t for t in tables if t > pos] + [hi])
        advanced = False
        for insn in md.disasm(code_data[pos - code_start:stop - code_start], pos):
            li = LinearInstruction(insn)
            insns.append(li)
            advanced = True
            pos = li.end_address
            if li.is_jump:
                t = li.get_branch_target()
                if t and lo <= t < hi:
                    leaders.add(t)
                leaders.add(li.end_address)
            # NOT a stopping point. MSVC pads between basic blocks with int3,
            # so halting here truncates the function at its first gap --
            # handleEvent lost everything past 0x611564, including BOTH of its
            # `ret`s, so the lifted body fell out through the end instead of
            # running its epilogue and never restored the caller's ebp. Trailing
            # int3 padding after the last ret is unreachable, so decoding it
            # costs nothing.
        if not advanced:                  # undecodable byte: give up on the rest
            break
    return insns, leaders


def lift_function(lifter, name, insns, leaders):
    # The per-function scratch list belongs to the lifter, not to us: `ebp` and
    # the x87 stack (_st/_fp_top/_fpu_cw) are deliberately GLOBAL in
    # recomp_types.h, because an optimising compiler splits one frame across
    # blocks that all address it through the same ebp. Declaring them locally
    # here -- which is what an older driver did -- silently zeroes the frame
    # pointer at the top of every such block.
    lines = [f'void recomp_{name}(void) {{']
    lines += [f'    {d}' for d in FUNCTION_LOCALS]
    # Records which lifted function is running, so a crash report names it
    # instead of saying sub_00000000. Free unless -DRECOMP_TRACE, which also
    # keeps the last 1024 entries as a backtrace.
    lines.append(f'    RECOMP_ENTER(0x{insns[0].address:08X}u);' if insns else '')
    lines.append('')
    lifter._flag_state = None
    # A function containing an indirect jump needs a label on EVERY instruction,
    # because a jump table can land anywhere and the local dispatch below has to
    # be able to reach it.
    has_indirect = any(i.is_uncond_jump and i.get_branch_target() is None
                       for i in insns)
    for insn in insns:
        if has_indirect or insn.address in leaders:
            lines.append(f'L_{insn.address:08X}:')
        lines.append(f'    RECOMP_PC(0x{insn.address:08X}u);')
        for line in lifter.lift_instruction(insn):
            lines.append(f'    {BALANCE_CALL.sub(_balance(insn.address), line)}')

    # Falling off the end is NOT the same as returning. If the last instruction
    # is neither a `ret` nor an unconditional `jmp`, control flows into whatever
    # follows -- the linker routinely leaves one routine's `ret` owned by the
    # next catalogued function.
    #
    # A bare `return;` there silently drops that `ret`, so the dummy return
    # address RECOMP_CALL pushed is never popped and the simulated stack leaks
    # 4 bytes per call. That drift is invisible until something reads an
    # argument off the shifted stack: here it cost ~584 bytes across one
    # sprintf, whose destination pointer then pointed below the image.
    #
    # Tail-call the fall-through address instead, so the next function's `ret`
    # does the pop.
    if insns and not insns[-1].is_ret:
        last = insns[-1]
        if last.is_uncond_jump:
            lines.append('    return; /* tail call already transferred */')
        else:
            lines.append(f'    RECOMP_ITAIL(0x{last.end_address:08X}u);'
                         f' /* falls through */')
            lines.append('    return;')

    # Intra-function indirect jumps (jump tables, switch statements) lift to
    # RECOMP_ITAIL(<runtime expr>), but the dispatch table only knows function
    # ENTRIES -- it cannot resolve a label inside this function, so the switch
    # would break. Route indirect tail-jumps through a local dispatch first,
    # falling back to the global one for genuine cross-function tail calls.
    indirect = [i for i, l in enumerate(lines)
                if 'RECOMP_ITAIL(' in l and 'RECOMP_ITAIL(0x' not in l]
    body = '\n'.join(lines)
    defined = sorted(set(re.findall(r'(?m)^\s*(L_[0-9A-Fa-f]{8})\s*:', body)))
    if indirect and defined:
        for i in indirect:
            m = re.search(r'RECOMP_ITAIL\((.+?)\);\s*return;', lines[i])
            if m:
                lines[i] = (f'    {{ _itail_tgt = (uint32_t)({m.group(1)}); '
                            f'goto _ljump; }}')
        lines.append('  _ljump:')
        lines.append('    switch (_itail_tgt) {')
        for lbl in defined:
            lines.append(f'      case 0x{int(lbl[2:], 16):08X}u: goto {lbl};')
        lines.append('      default: RECOMP_ITAIL(_itail_tgt); return;')
        lines.append('    }')

    # Cross-function jumps reference labels outside this function's range; emit
    # them as indirect tail-calls so the C compiles and dispatch resolves them
    # at runtime.
    refed = set(re.findall(r'goto\s+(L_[0-9A-Fa-f]{8})', '\n'.join(lines)))
    for lbl in sorted(refed - set(defined)):
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
        f.write('#include "recomp_funcs.h"\n#include <math.h>\n#include <string.h>\n')
        # Under RECOMP_TRACE every instruction records its own VA, so a crash
        # report names the faulting instruction and not just the function.
        # Compiles to nothing otherwise.
        f.write('#ifdef RECOMP_TRACE\n'
                'extern uint32_t g_cur_pc;\n'
                'void recomp_check_balance(uint32_t sp, uint32_t bp, uint32_t site);\n'
                'void recomp_check_balance2(uint32_t sp, uint32_t bp,\n'
                '                           uint32_t site, uint32_t target);\n'
                'void recomp_push_site(uint32_t site);\n'
                '#define RECOMP_PC(va) (g_cur_pc = (va))\n'
                '#define RECOMP_PUSHSITE(site) recomp_push_site(site)\n'
                '#define RECOMP_BALANCE(sp, bp, site) '
                'recomp_check_balance((sp), (bp), (site))\n'
                '#define RECOMP_BALANCE2(sp, bp, site, tgt) '
                'recomp_check_balance2((sp), (bp), (site), (tgt))\n'
                '#else\n'
                '#define RECOMP_PC(va) ((void)0)\n'
                '#define RECOMP_PUSHSITE(site) ((void)0)\n'
                '#define RECOMP_BALANCE(sp, bp, site) ((void)0)\n'
                '#define RECOMP_BALANCE2(sp, bp, site, tgt) ((void)0)\n'
                '#endif\n\n')
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
    # Clear previous output first. The file count moves with the function count,
    # and a leftover chunk from a longer run still gets globbed by CMake -- which
    # shows up as every function in it being defined twice at link time.
    for fn in os.listdir(out):
        if re.fullmatch(r'recomp_\d+\.c', fn):
            os.remove(os.path.join(out, fn))

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
    with open(os.path.join(_here, 'analysis', 'fpo.json'), encoding='utf-8') as fh:
        fpo = json.load(fh)

    # Start from the CodeView publics, then add what a public symbol table
    # cannot see. `static` functions have no public symbol at all, and this
    # binary has thousands of them -- they showed up as direct call targets with
    # no definition. Two sources close the gap: FPO records, which describe a
    # frame regardless of linkage, and the call graph itself.
    def synth(va):
        return {'address': f'0x{va:08X}', 'address_int': va, 'name': '',
                'demangled': None, 'module': None, 'source': None}

    known = {e['address_int']: e for e in table if cs <= e['address_int'] < ce}
    exact_size = {e['address_int']: e['size'] for e in table if e['exact_size']}
    from_syms = len(known)
    for f in fpo:
        va = info.image_base + f['rva']
        if cs <= va < ce:
            exact_size.setdefault(va, f['size'])
            known.setdefault(va, synth(va))
    from_fpo = len(known) - from_syms

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    def bounds(order):
        """Each entry runs to the next one, or to its exact size if it has a
        smaller one. Bounds only ever shrink, so they cannot overlap -- and a
        newly discovered entry correctly shortens whatever preceded it."""
        out = {}
        for i, va in enumerate(order):
            nxt = order[i + 1] if i + 1 < len(order) else ce
            out[va] = min(exact_size.get(va, nxt - va), nxt - va)
        return out

    # Pointer tables in data. A static constructor is never *called* from code:
    # its address sits in a table that _initterm walks, so neither the public
    # symbols nor a call-graph sweep can see it. MechCommander has one at
    # 0x0074D4A0 that consequently never ran, leaving its object unconstructed
    # and systemInit calling a virtual method through a garbage vtable pointer.
    #
    # These tables are runs of aligned dwords that are each either null or a
    # .text address -- constructor tables, vtables and callback tables all have
    # that shape, and a run of several in a row is not something data hits by
    # accident. A lone plausible-looking dword is NOT enough: a false entry
    # would split a real function.
    data_ptrs = set()
    for sec in info.sections:
        if sec.name not in ('.rdata', '.data'):
            continue
        blob = pe_data[sec.raw_offset:
                       sec.raw_offset + min(sec.virtual_size, sec.raw_size)]
        base = info.image_base + sec.virtual_address
        run = []
        for off in range(0, len(blob) - 3, 4):
            word = struct.unpack_from('<I', blob, off)[0]
            if word == 0 or cs <= word < ce:
                run.append(word)
                continue
            if len({w for w in run if w}) >= 2:
                data_ptrs.update(w for w in run if w)
            run = []
        if len({w for w in run if w}) >= 2:
            data_ptrs.update(w for w in run if w)
    # A candidate must be a real instruction boundary. A run of dwords is
    # strong evidence of a pointer table but not proof, and one false entry is
    # expensive: it truncates whatever function contains it, so the real body
    # ends mid-flow and a garbage "function" starts inside an instruction.
    # 0x00646F6D got in this way -- four bytes into a five-byte `push` -- and
    # chopped HeapManager::destroy down to its first branch.
    #
    # Decoding forward from the entry that already covers the candidate settles
    # it: either an instruction starts exactly there, or nothing does.
    def is_boundary(va):
        prev = [k for k in known if k <= va]
        if not prev:
            return False
        start = max(prev)
        if start == va:
            return True
        limit = min(va + 16, ce)
        for insn in md.disasm(code_data[start - cs:limit - cs], start):
            if insn.address == va:
                return True
            if insn.address > va:
                return False
        return False

    new_from_data = {va for va in data_ptrs - set(known) if is_boundary(va)}
    rejected = len(data_ptrs - set(known)) - len(new_from_data)
    for va in new_from_data:
        known[va] = synth(va)
    from_data = len(new_from_data)
    if rejected:
        print(f'[*]   data sweep: {from_data} entries, {rejected} rejected as '
              f'not instruction boundaries', flush=True)

    # Only `call` targets seed discovery. A jump target is not reliably a
    # function entry -- an intra-function branch and a tail call look identical
    # here -- and a false entry would split a real function in half.
    for rnd in range(1, 9):
        order = sorted(known)
        size_of = bounds(order)
        found = set()
        for va in order:
            hi = va + size_of[va]
            insns, _ = linear_disassemble(md, code_data, cs, va, hi)
            for ins in insns:
                # A code address loaded as an IMMEDIATE is an entry too.
                #
                # AG_ellipse_fill picks one of two span writers with
                # `mov ebx, 0x6b6c07` / `mov ebx, 0x6b6b5a`, stores it in a
                # global and then `call dword ptr [0x8023d4]` -- into the middle
                # of itself. Both targets are internal labels ending in their
                # own `ret`, and no other source can see them: they are never
                # the operand of a call or jump, and they are in code rather
                # than in a data table. Unresolved, the indirect call did
                # nothing, the routine's span globals were never advanced, and
                # the span length came out as 0xFFFFFFFF -- which then ran the
                # byte-write loop off the end of the surface. That was the first
                # frame of the tactical map.
                # Skipped for call/jump, whose immediate IS the branch target
                # and is handled below -- a `continue` here would swallow that.
                if not (ins.is_call or ins.is_jump):
                    for op in ins.operands:
                        if op.type != X86_OP_IMM:
                            continue
                        v = op.imm & 0xFFFFFFFF
                        if cs <= v < ce and v not in known and is_boundary(v):
                            found.add(v)
                t = ins.get_branch_target()
                if not t or not (cs <= t < ce) or t in known:
                    continue
                if ins.is_call:
                    found.add(t)
                elif ins.is_jump and not (va <= t < hi):
                    # A branch that LEAVES this function has to land on a known
                    # entry: it lifts to RECOMP_ITAIL, and dispatch resolves
                    # entries only. An unresolved one returns without popping,
                    # so the dummy return address and anything the function
                    # pushed are left behind.
                    #
                    # _strcpy is a five-byte stub -- `push edi` then a jump to
                    # its real body, which sits AFTER _strcat -- so its target
                    # is in the middle of another function. That leaked exactly
                    # 8 bytes a call, shifting every later stack local, which is
                    # how systemInit came to read an uninitialised vtable
                    # pointer out of a stack object.
                    #
                    # Splitting the containing function here is safe: the part
                    # before the new entry falls through to it, and registers
                    # (ebp included) are global in this model precisely so a
                    # frame can span separately-lifted blocks.
                    found.add(t)
        if not found:
            break
        for va in found:
            known[va] = synth(va)
        print(f'[*]   discovery round {rnd}: +{len(found)} static functions',
              flush=True)

    order = sorted(known)
    size_of = bounds(order)
    print(f'[*] {len(order)} functions: {from_syms} CodeView publics, '
          f'+{from_fpo} FPO-only, +{from_data} from data pointer tables, '
          f'+{len(order) - from_syms - from_fpo - from_data} via the call graph')

    entries = [dict(known[va], size=size_of[va]) for va in order]
    # Telling the lifter the names means a direct call emits the real symbol;
    # telling it which VAs exist means a call to anything else degrades to a
    # logged indirect call instead of an unresolved external at link time.
    #
    # Except for OVERRIDES. A direct RECOMP_CALL is a plain C call and never
    # consults recomp_lookup_manual, so a runtime override of one of these would
    # simply never fire. Withholding the VA makes every call to it indirect,
    # which does go through the lookup -- and the manual override wins there.
    names = {va: c_name(known[va]) for va in order if va not in OVERRIDES}
    # precise_sbb: take `sbb r,r`'s carry from the comparison that actually set
    # it, rather than from the lifter's running `_cf`.
    #
    # It is off by default in the toolbox because it regressed Fury3, but this
    # target cannot do without it. `cmp` + `sbb eax,eax` is how the MSVC 6 CRT
    # turns a comparison into -1/0/+1, and both memcmp and strcmp end that way.
    # With the stale carry, memcmp reported two identical buffers as different,
    # so the config parser found none of its keys in a file it had read
    # correctly. src/runtime's --selftest is the arbiter: it exercises these
    # routines against the host's answers, and it goes from 2 failures to 0.
    lifter = Lifter(iat_map=iat, func_names=names,
                    lifted=set(order) - set(OVERRIDES), precise_sbb=True)

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
            #
            # int3 is SKIPPED, not a stopping point. MSVC pads between basic
            # blocks with it, so breaking here truncated the instruction list
            # mid-function: handleEvent lost everything past 0x611564,
            # including both of its `ret`s, and the lifted body fell out
            # through the end instead of running its epilogue -- which is why
            # it never restored the caller's ebp and the corruption cascaded
            # all the way up to aCallback::exec.
            trimmed, seen_ret = [], False
            for ins in insns:
                if ins.mnemonic == 'int3':
                    continue
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
            code = f'/* LIFT ERROR {name}: {exc} */\nvoid recomp_{name}(void) {{}}\n'
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
            f.write(f'void recomp_{n}(void);  /* 0x{a:08X} */\n')

    with open(os.path.join(out, 'recomp_dispatch.c'), 'w', encoding='utf-8') as f:
        f.write('/* MechCommander Gold recompilation - generated */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n')
        f.write('const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(all_entries):
            f.write(f'    {{ 0x{a:08X}u, recomp_{n} }},\n')
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
