#!/usr/bin/env python3
"""
Generate MechCommander Gold's import-bridge layer (Phase 3).

The lifted code reaches each Win32 import through its IAT slot VA. This emits a
C shim per import plus a {iat_va -> shim} bridge table the runtime installs.

Every shim must be stdcall-correct: it pops the dummy return address plus
exactly the right number of argument slots. That count is not cosmetic. Get it
wrong and the simulated stack silently desynchronises, so every value read after
that call is garbage -- and the symptom shows up nowhere near the cause.

So nothing here is typed by hand. There are three independent sources of the
truth, and this script uses all of them and makes them agree:

  1. The Windows SDK's 32-bit import libraries, where stdcall exports are
     decorated `_Name@N` (N = argument bytes). This is pcrecomp's
     stdcall_argc.py, and it answers 192 of MCX.EXE's 204 imports.
  2. MCX.EXE's OWN CodeView symbol table, which names each import thunk with
     the same decoration -- `_DirectPlayLobbyCreateA@20` is 5 arguments, stated
     by the linker that built the game. 410 symbols carry it.
  3. The import table's own names, for a third-party DLL the SDK never heard of:
     smackw32 is imported as `_SmackOpen@12`, decoration included.

Where (1) and (2) both have an answer they are checked against each other, and a
disagreement is a hard error rather than a silent pick.

A thunk symbol present WITHOUT decoration is positive evidence of cdecl, not an
absence of evidence: `_wsprintfA` is varargs, the caller cleans up, and the shim
must pop nothing. That is the only case where zero is correct, and it is still
derived rather than assumed.

Output: src/runtime/imports_gen.c
"""
import collections
import json
import os
import re
import sys

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.environ.get('PCRECOMP') or os.path.join(_here, '..', 'tools'),
                                'tools', 'pe'))
from pe_analyze import analyze_pe, build_iat_map      # noqa: E402
from stdcall_argc import ArgcResolver                 # noqa: E402

DECORATED = re.compile(r'^_([A-Za-z_][A-Za-z0-9_]*)@(\d+)$')
UNDECORATED = re.compile(r'^_([A-Za-z_][A-Za-z0-9_]*)$')

# Imports with real bodies in src/runtime/shims_impl.c. These get an extern
# declaration instead of a stub; the bridge table still points at them, so
# swapping a stub for a real body is a one-line change here.
#
# Names are checked against the import table below -- a typo would otherwise
# leave the stub in place and look exactly like the shim not being called.
HAND_WRITTEN = {
    # The MSVC 6 CRT's heap. It calls HeapCreate then HeapAlloc for every
    # malloc, so these have to return VAs inside the simulated address space,
    # not host pointers.
    'HeapCreate', 'HeapDestroy', 'HeapAlloc', 'HeapFree', 'HeapReAlloc',
    'HeapSize', 'GetProcessHeap',
    'VirtualAlloc', 'VirtualFree', 'VirtualQuery', 'GlobalMemoryStatus',
    # Program identity and environment: what the CRT builds argv/environ from.
    'GetCommandLineA', 'GetModuleHandleA', 'GetModuleFileNameA',
    'GetStartupInfoA', 'GetEnvironmentStrings', 'GetEnvironmentStringsW',
    'FreeEnvironmentStringsA', 'FreeEnvironmentStringsW',
    'GetVersion', 'GetCurrentProcess',
    # Standard handles and the file-type probe the CRT runs on each of them.
    'GetStdHandle', 'SetStdHandle', 'SetHandleCount', 'GetFileType',
    # Critical sections: single-threaded here, so these are bookkeeping only.
    'InitializeCriticalSection', 'DeleteCriticalSection',
    'EnterCriticalSection', 'LeaveCriticalSection',
    'InterlockedExchange',
    # Errors and the pointer-validity probes the CRT uses before touching argv.
    'GetLastError', 'IsBadReadPtr', 'IsBadWritePtr', 'IsBadCodePtr',
    # Locale init -- the CRT does this before main and checks the results.
    'GetACP', 'GetOEMCP', 'GetCPInfo', 'GetStringTypeA', 'GetStringTypeW',
    'LCMapStringA', 'LCMapStringW', 'MultiByteToWideChar',
    'WideCharToMultiByte', 'CompareStringA', 'CompareStringW',
    # File I/O is forwarded to the host's own Win32 -- the host IS Windows.
    'CreateFileA', 'ReadFile', 'WriteFile', 'CloseHandle', 'SetFilePointer',
    'GetFileSize', 'FlushFileBuffers', 'SetEndOfFile',
    'GetFileAttributesA', 'FindFirstFileA', 'FindNextFileA', 'FindClose',
    'GetCurrentDirectoryA', 'GetFullPathNameA',
    # Time.
    'GetTickCount', 'QueryPerformanceCounter', 'QueryPerformanceFrequency',
    'GetLocalTime', 'GetSystemTime', 'GetSystemTimeAsFileTime', 'Sleep',
    'timeGetTime', 'timeBeginPeriod', 'timeEndPeriod',
    # The engine drives its heartbeat from a multimedia timer; a stub that
    # reports failure leaves its state machine never ticking.
    'timeSetEvent', 'timeKillEvent', 'timeGetDevCaps', 'timeGetSystemTime',
    # Diagnostics, and exits that really exit.
    'OutputDebugStringA', 'ExitProcess', 'TerminateProcess',
    # Devices with no implementation yet report ABSENT, not success. A stub
    # returning 0 would claim a device exists and leave its out-struct
    # uninitialised, which is worse than saying no.
    'waveOutGetNumDevs', 'DirectDrawCreate', 'DirectSoundCreate',
    'DirectPlayLobbyCreateA',
    # The engine reaches DirectPlay through COM, not the DPLAYX entry point.
    'CoInitialize', 'CoUninitialize', 'CoCreateInstance',
    # Registry: forwarded to the host's, because the settings really do
    # live there. Stubs were actively harmful here -- ERROR_SUCCESS is 0,
    # so each one claimed to open a key and left the out HKEY stale.
    'RegOpenKeyA', 'RegOpenKeyExA', 'RegCloseKey', 'RegQueryValueExA',
    'RegSetValueExA', 'RegFlushKey',
    # Modules and dialogs.
    'LoadLibraryA', 'FreeLibrary', 'GetProcAddress', 'MessageBoxA',
    # APIs whose whole job is to fill a caller-provided struct. A stub
    # returning 0 is a valid-looking answer that leaves the struct stale,
    # and the fault then lands wherever the caller first trusts it.
    'GetTimeZoneInformation', 'GetSystemInfo', 'GetUserNameA',
    'GetWindowsDirectoryA', 'GetSystemDirectoryA', 'GetTempPathA',
    'GetDriveTypeA',
    # Every message the engine shows is loaded through this. Answered from
    # MCX.EXE's own .rsrc via the host's resource loader.
    'LoadStringA',
    # The window and its message pump, forwarded to the host's own USER32. The
    # window procedure is the interesting one: it calls back INTO lifted code.
    'RegisterClassA', 'CreateWindowExA', 'DefWindowProcA', 'DestroyWindow',
    'ShowWindow', 'SetFocus', 'SetForegroundWindow', 'EnableWindow',
    'SetWindowTextA', 'PostQuitMessage', 'MessageBeep', 'ShowCursor',
    'SetCursorPos', 'SetCapture', 'ReleaseCapture', 'GetSystemMetrics',
    'GetAsyncKeyState', 'GetKeyState', 'GetCaretBlinkTime', 'GetMessagePos',
    'RegisterWindowMessageA', 'FindWindowA', 'PostMessageA', 'WaitMessage',
    'SetRect', 'PtInRect', 'IntersectRect', 'AdjustWindowRect',
    'ClientToScreen', 'GetCursorPos', 'ClipCursor', 'MapWindowPoints',
    'InvalidateRect', 'SetWindowPos', 'SetWindowLongA',
    'PeekMessageA', 'TranslateMessage', 'DispatchMessageA',
    'GetDC', 'ReleaseDC', 'LoadIconA',
    # GDI: these imports are exactly an 8-bit DIB blitter, which is the
    # engine's windowed presentation path.
    'CreateCompatibleDC', 'DeleteDC', 'DeleteObject', 'SelectObject',
    'GetStockObject', 'SetStretchBltMode', 'BitBlt', 'CreateDIBSection',
    'SetDIBColorTable', 'CreatePalette', 'SelectPalette', 'RealizePalette',
    'AnimatePalette', 'GetSystemPaletteEntries',
    # Smacker. A stub returning NULL is not safe here: BriefingScreen::
    # setUpOperation reads the returned Smack's width and height with no null
    # check at all, so a stub faults the moment a briefing starts.
    'SmackOpen',
}


# Shims that never return, so they have no epilogue to get right. Adding a dead
# STDRET just to satisfy the check below would be cargo-cult: control does not
# come back, and the simulated stack is never read again. They are still checked
# -- for actually terminating, which is the property that matters here.
NORETURN = {'ExitProcess', 'TerminateProcess'}


# DLLs the game resolves at runtime with LoadLibrary/GetProcAddress instead of
# importing statically. They never appear in the import table, so nothing above
# sees them -- but they still need stdcall-correct shims, and the argument
# counts are derivable from the SDK exactly as the static ones are.
#
# MechCommander loads imagehlp.dll for its own crash reporter and treats a
# failed GetProcAddress as a fatal exception, quitting before it reaches its
# main loop. The value beside each name is what its shim returns.
# A shim is generated for EVERY export the SDK knows about, not for a list --
# the game resolves a batch of symbols in a loop and treats the first missing
# one as fatal, so naming them individually is whack-a-mole. Only the return
# values that need to be non-zero are listed.
DYNAMIC = {
    'imagehlp.dll': {
        'SymInitialize': 1,        # TRUE, or the game calls the load fatal
        'SymCleanup': 1,
        'SymLoadModule': 1,
        'SymUnloadModule': 1,
        'SymRegisterCallback': 1,
    },
}


HEADER = """/* MechCommander Gold recompilation - import bridge layer - GENERATED */
/* Regenerate: py -3.11 gen_imports.py   -- do not edit by hand */
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include "imports.h"

/* %d imports across %d DLLs.
 *
 * Argument counts are derived, never typed: from the Windows SDK's 32-bit
 * import libraries, from MCX.EXE's own CodeView thunk symbols, and from the
 * decoration carried in the import table itself. Where two sources both have an
 * answer they are checked against each other. Every shim below therefore pops
 * exactly what the real API pops.
 *
 * Bodies are stubs: each logs once and returns a benign default. Real bodies
 * land in shims_impl.c during bring-up, declared extern here.
 */

"""


def binary_argc(sym_path):
    """argc for every decorated symbol in the game's own CodeView table, plus
    the set of names it declares undecorated (i.e. cdecl)."""
    if not os.path.exists(sym_path):
        return {}, set()
    with open(sym_path, encoding='utf-8') as fh:
        syms = json.load(fh)
    dec, und = {}, set()
    for s in syms:
        m = DECORATED.match(s['name'])
        if m:
            dec[m.group(1)] = int(m.group(2)) // 4
            continue
        m = UNDECORATED.match(s['name'])
        if m:
            und.add(m.group(1))
    return dec, und


def resolve(iat, dec, und):
    """(va, dll, name, real, argc, source) per import, plus what failed."""
    sdk_rows, sdk_unres = ArgcResolver().resolve_iat(iat)
    sdk = {(va, real): argc for va, _d, _n, real, argc in sdk_rows}
    known = {va: (dll, name, real) for va, dll, name, real, _a in sdk_rows}
    for va, dll, name, real in sdk_unres:
        known[va] = (dll, name, real)

    rows, unresolved, conflicts = [], [], []
    for va in sorted(known):
        dll, name, real = known[va]
        from_sdk = sdk.get((va, real))
        # The import name may carry its own decoration (smackw32!_SmackOpen@12).
        m = DECORATED.match(name)
        from_name = int(m.group(2)) // 4 if m else None
        if m:
            real = m.group(1)
        from_bin = dec.get(real)

        answers = {k: v for k, v in
                   (('sdk', from_sdk), ('binary', from_bin), ('import', from_name))
                   if v is not None}
        if len(set(answers.values())) > 1:
            conflicts.append((va, dll, real, answers))
            continue
        if answers:
            src, argc = sorted(answers.items())[0]
            rows.append((va, dll, name, real, argc, '+'.join(sorted(answers))))
        elif real in und:
            # Declared, but with no @N. That is what cdecl looks like, and for a
            # varargs export like wsprintfA it is the only correct answer: the
            # caller cleans up, so the shim pops nothing but the return address.
            rows.append((va, dll, name, real, 0, 'cdecl'))
        else:
            unresolved.append((va, dll, name, real))
    return rows, unresolved, conflicts


def emit(rows, out_path):
    dlls = sorted({d for _, d, _, _, _, _ in rows})
    lines = [HEADER % (len(rows), len(dlls))]
    seen = set()
    for _va, dll, _name, real, argc, src in rows:
        if real in seen:
            continue                  # same export reached through two IAT slots
        seen.add(real)
        if real in HAND_WRITTEN:
            lines.append(f'extern void imp_{real}(void);  /* {dll}!{real} '
                         f'({argc} args, {src}) - real body in shims_impl.c */')
            continue
        lines.append(f'/* {dll}!{real}  ({argc} args, via {src}) */')
        lines.append(f'static void imp_{real}(void) {{ IMPORT_STUB("{real}"); '
                     f'RET(0); STDRET({argc}); }}')

    lines.append('\n/* IAT slot VA -> shim. The runtime installs these before '
                 'entry. */')
    lines.append('const recomp_dispatch_entry_t mcg_import_bridges[] = {')
    for va, dll, _name, real, _argc, _src in rows:
        lines.append(f'    {{ 0x{va:08X}u, imp_{real} }},  /* {dll}!{real} */')
    lines.append('};')
    lines.append(f'const uint32_t mcg_import_bridge_count = {len(rows)};\n')

    lines.append('/* The IAT slots themselves, so the runtime can point each '
                 'one at its bridge. */')
    lines.append('const uint32_t mcg_iat_slots[] = {')
    for va, _d, _n, _r, _a, _s in rows:
        lines.append(f'    0x{va:08X}u,')
    lines.append('};')
    lines.append(f'const uint32_t mcg_iat_slot_count = {len(rows)};')

    # Runtime-resolved exports (LoadLibrary/GetProcAddress). Same rule as the
    # static side: the count is derived, never typed.
    resolver = ArgcResolver()
    dyn = []
    for dll, retvals in sorted(DYNAMIC.items()):
        # Every export in the SDK's import library for this DLL, with its
        # decoration-derived argument count. `_lib` is the parsed table the
        # resolver already builds for lookup(); enumerating it is what lets us
        # cover the whole DLL instead of the names we happened to hit.
        table = resolver._lib(dll)
        if not table:
            raise SystemExit(f'{dll}: no import library found, so no argument '
                             f'counts. Install the matching 32-bit SDK.')
        for proc in sorted(table):
            dyn.append((dll, proc, table[proc], retvals.get(proc, 0)))
        missing = sorted(set(retvals) - set(table))
        if missing:
            raise SystemExit(f'{dll}: named in DYNAMIC but not exported: '
                             + ', '.join(missing))

    lines.append('\n/* Exports the game resolves at runtime with '
                 'LoadLibrary/GetProcAddress. */')
    for dll, proc, argc, retval in dyn:
        lines.append(f'/* {dll}!{proc}  ({argc} args) */')
        lines.append(f'static void imp_dyn_{proc}(void) {{ '
                     f'IMPORT_STUB("{dll}!{proc}"); '
                     f'RET({retval}); STDRET({argc}); }}')
    lines.append('const mcg_dyn_export_t mcg_dyn_exports[] = {')
    for dll, proc, _argc, _r in dyn:
        lines.append(f'    {{ "{dll}", "{proc}", imp_dyn_{proc} }},')
    lines.append('};')
    lines.append(f'const uint32_t mcg_dyn_export_count = {len(dyn)};')

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('\n'.join(lines) + '\n')


def check_hand_written(rows, shims_path):
    """Verify each hand-written shim pops what the derived count says.

    Deriving 204 argument counts and then hand-typing STDRET in the bodies would
    put the mistake straight back where it was taken from -- and a wrong STDRET
    fails silently. So the two are checked against each other on every run.
    """
    if not os.path.exists(shims_path):
        return
    derived = {real: argc for _v, _d, _n, real, argc, _s in rows}
    with open(shims_path, encoding='utf-8') as fh:
        src = fh.read()
    # Bound each body by the start of the next definition, so one-line shims on
    # consecutive lines don't run together.
    starts = [(m.start(), m.group(1))
              for m in re.finditer(r'void imp_(\w+)\(void\)\s*\{', src)]
    problems = []
    for i, (pos, name) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else len(src)
        found = {int(x) for x in re.findall(r'STDRET\((\d+)\)', src[pos:end])}
        want = derived.get(name)
        body = src[pos:end]
        if want is None:
            problems.append(f'{name}: defined but not imported by this binary')
        elif name in NORETURN:
            # No epilogue by design -- but it had better really not return.
            if 'ExitProcess(' not in body:
                problems.append(f'{name}: listed NORETURN but does not exit')
            if found:
                problems.append(f'{name}: NORETURN, so STDRET is unreachable')
        elif not found:
            problems.append(f'{name}: no STDRET (should pop {want})')
        elif found != {want}:
            problems.append(f'{name}: pops {sorted(found)}, should pop {want}')
    defined = {n for _p, n in starts}
    for name in sorted(HAND_WRITTEN - defined):
        problems.append(f'{name}: in HAND_WRITTEN but no body in '
                        f'{os.path.basename(shims_path)}')
    if problems:
        print('[!] hand-written shims disagree with the derived counts:',
              file=sys.stderr)
        for p in problems:
            print('      ' + p, file=sys.stderr)
        raise SystemExit(1)
    print(f'  {len(starts)} hand-written shims verified against derived counts')


def main(argv):
    exe = argv[0] if argv else os.path.join(_here, 'game', 'MCX.EXE')
    out = (argv[1] if len(argv) > 1
           else os.path.join(_here, 'src', 'runtime', 'imports_gen.c'))

    info = analyze_pe(exe)
    iat = build_iat_map(info)
    dec, und = binary_argc(os.path.join(_here, 'work', 'cv_symbols.json'))
    rows, unresolved, conflicts = resolve(iat, dec, und)

    if conflicts:
        print('[!] argument-count sources disagree -- one of them is wrong, and '
              'picking silently is how a stack desync gets shipped:',
              file=sys.stderr)
        for va, dll, real, answers in conflicts:
            detail = ', '.join(f'{k}={v}' for k, v in sorted(answers.items()))
            print(f'      0x{va:08X}  {dll}!{real}: {detail}', file=sys.stderr)
        return 1

    if unresolved:
        print(f'[!] could not derive an argument count for {len(unresolved)} '
              f'import(s):', file=sys.stderr)
        for va, dll, name, real in unresolved:
            extra = '' if real == name else f' (-> {real})'
            print(f'      0x{va:08X}  {dll}!{name}{extra}', file=sys.stderr)
        print('    A guessed count silently desynchronises the simulated stack.',
              file=sys.stderr)
        return 1

    # A HAND_WRITTEN name that is not actually imported means a typo, and a typo
    # here fails silently: the stub stays and looks just like the shim not being
    # reached.
    imported = {real for _v, _d, _n, real, _a, _s in rows}
    stray = sorted((HAND_WRITTEN | NORETURN) - imported)
    if NORETURN - HAND_WRITTEN:
        print('[!] NORETURN names missing from HAND_WRITTEN: '
              + ', '.join(sorted(NORETURN - HAND_WRITTEN)), file=sys.stderr)
        return 1
    if stray:
        print("[!] HAND_WRITTEN names not in this binary's import table: "
              + ', '.join(stray), file=sys.stderr)
        return 1

    emit(rows, out)
    check_hand_written(rows, os.path.join(os.path.dirname(out), 'shims_impl.c'))

    by_dll = collections.Counter(d for _v, d, _n, _r, _a, _s in rows)
    by_src = collections.Counter(s for _v, _d, _n, _r, _a, s in rows)
    print(f'{len(rows)} import bridges across {len(by_dll)} DLLs '
          f'({len(HAND_WRITTEN)} hand-written, '
          f'{len(imported) - len(HAND_WRITTEN)} stubs) -> {out}')
    for dll, n in sorted(by_dll.items()):
        print(f'  {dll:<16} {n}')
    print('  argc derived from: '
          + ', '.join(f'{k} x{v}' for k, v in sorted(by_src.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
