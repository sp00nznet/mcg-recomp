# MechCommander Gold Static Recompilation — Project Memory

## Standing Directive
Keep the README's status table and numbers current with every phase. Commit and
push to `main` on GitHub regularly. Repo is **private**.

## Project Overview
Static recompilation of _MechCommander Gold / Desperate Measures_ (1998–99, FASA
Interactive) — lifting the retail `MCX.EXE` to C for native Windows 11.

## Key Paths
- **Repo root**: `G:/recomp/pc/mcg` — the retail install is in this **same
  folder** (`DATA/`, `*.FST`, `MCX.EXE`, `SETUP/`, …). The `.gitignore` is an
  allowlist (`/*` then `!`) so no game file can be committed by accident.
- **pcrecomp toolbox**: `G:/recomp/pc/tools` (shared checkout; scripts live in
  `tools/tools/`). Referenced, never vendored.
- **Clean-room sibling**: `D:/mcgoldng` (repo `sp00nznet/mcgoldng`) — SDL2/OpenGL
  reimplementation of the same game. Its asset decoding is real work and is not
  superseded; see "Assets" below.
- **MC2 shared source**: `C:/MechCommander2-Source` — Microsoft's 2006 release of
  the *sequel*. Useful as a naming/structure reference (many class names carry
  over). It is a different codebase under its own licence: read it, never paste
  from it.

## Hard-Won Facts (do not re-derive)
- **`MCX.EXE` ships full CodeView (NB11) debug info in its PE overlay** — 1.79 MB
  at file offset `0x1BBC60`, debug directory type 2. Also type 3 (FPO, 1,973
  records) and type 4 (MISC, the string `.\RelNoDbg/MechCmdr.exe`). This is the
  whole ballgame: **no function-discovery heuristic is needed on this target.**
- **12,614 `S_PUB32` symbols with MSVC C++ decorated names.** `S_PUB32` is
  `0x1009` in CV5's numbering and its layout is `typind(u32) off(u32) seg(u16)
  name(pstr)` — note `typind` comes *first*, which is not what every reference
  says. 612 `sstModule`, 179 `sstSrcModule` (source paths under `L:\mcx\`).
- **Only 65 `S_GPROC32` records exist.** A `RelNoDbg` build keeps publics and
  line info but drops most per-module proc/local symbols, so exact byte lengths
  come from FPO for the most part (2,038 entries have an exact size, the other
  3,722 are bounded by the next symbol).
- **Image base is `0x00600000`, not `0x00400000`.** The lifted image must map
  there. The usual "put the simulated stack below the image" trick has more room
  here than on a `0x00400000` target, but confirm before relying on it.
- **No DRM, no packer.** `analyze_sections.py` reports no indicators; entropy of
  `.text` is 6.28. It analyses directly — no runtime dump needed, unlike Crimson
  Skies or XWA.
- **MSVC 6 folded identical COMDATs** (191 addresses carry more than one public
  symbol). They are one function with aliases, not several functions. Anything
  that keys a function table by address must dedupe or it will emit duplicate C
  symbols.
- **Demangling is `dbghelp!UnDecorateSymbolName` via ctypes.** The toolbox's
  `tools/cpp/msvc_mangler.py` is a *mangler* built for Black & White, not a
  demangler — do not reach for it. Windows already has the right answer.
- **`MCXTRACT.EXE` is FASA's own asset extractor** — 11 KB of code, MFC42. When
  the FST format needs settling, lift that, not the game.
- **`HSBRMP.EXE` is the Hasbro multiplayer launcher, not the game.** Linker 5.0,
  3 MB of its 3.5 MB is `.rsrc`. Ignore it.

## Engine map (from the recovered source paths)
`object/` (47 modules — `Mech`, `Gvehicl`, `Warrior`, `Move`, `MOVER`,
`ELEMNTL`), `lib/` (15), `abl/` (14 — the ABL mission-script VM), `logistics/`
(13; `logistics.obj` is the biggest single module at 102 KB), `engine/` (12),
`sprite/` (12), `gui/` (10), `vfx/` (8 — the software rasteriser), `linkup/`
(7 — DirectPlay), `terrain/` (4), plus `iface/`, `mission/`, `ai/`, `camera/`,
`appear/`, `Game OS/Sound Renderer/`.

## Assets
The clean-room sibling already decoded these — **do not re-derive**, read
`D:/mcgoldng/src/assets/` and `D:/mcgoldng/docs/FILE_FORMATS.md`:
FST archives, PAK archives, LZ decompression, FIT config files, VFX shape tables
(`.SHP`), 256-colour palettes. Open there: the mech sprite row encoding (a long
investigation that did not land) and ABL.

## Phase 3 (done) — links, runs, CRT completes, game code executes
- Layout: image `0x00600000` (+2.1 MB), stack `0x00820000-0x00C20000` (4 MB),
  heap arena `0x00C20000-0x10C20000` (256 MB reserved, committed on demand).
  Synthetic VAs for `GetProcAddress` results live at `0x11000000+`, deliberately
  above everything mapped so a stray one is unmistakable.
- **`gen_imports.py` derives every argument count from three sources and makes
  them agree**: the SDK import libs (192), MCX.EXE's own decorated CodeView
  thunk symbols (193), and the import table's own names for smackw32 (10).
  192 are confirmed by two sources with zero disagreements. An undecorated thunk
  symbol is positive evidence of cdecl — that is `_wsprintfA`, varargs, pops 0.
  It also verifies every hand-written shim's STDRET on each run. **Never
  hand-tune a STDRET.**
- **`VirtualAlloc` must return 64 KB-aligned addresses.** The real one always
  does, and the MSVC 6 small-block heap masks low bits off a block pointer to
  find its region. A 16-byte-aligned bump pointer made that arithmetic produce
  base+base and the write landed 12 MB out.
- **`___sbh_threshold` (`0x007A714C`, ships as `0x3F8`) is set to 0 at startup**
  so every allocation goes to HeapAlloc instead of the CRT small-block heap.
  This is the CRT's own switch (`_set_sbh_threshold`), a supported config — but
  it SIDESTEPS an unexplained fault in lifted `__sbh_alloc_block`, where a local
  holds a pointer where a size belongs. The lift of the faulting instruction and
  the flag model around it are both faithful. Open.
- **The game needs `imagehlp.dll`**: `LoadLibraryA(".\IMAGEHLP.DLL")` then a
  batch of `GetProcAddress`; a failure of either is a fatal exception and it
  quits before its main loop. Shims are generated for ALL 307 SDK-known exports
  (argc derived), not a hand-list — it gives up on the first missing name.
- **A stub returning 0 is not neutral**, hit twice: `ERROR_SUCCESS` is 0, so the
  Reg* stubs "succeeded" and left the out `HKEY` stale; and struct-filling APIs
  (`GetTimeZoneInformation`, `GetSystemInfo`, …) left the caller's struct
  uninitialised. Registry and those structs now forward to the host.
- **Generated code records its PC** (`RECOMP_PC`, free without `-DRECOMP_TRACE`)
  so a crash report names the faulting instruction, not just the function. Build
  `build-trace/` with `-DCMAKE_C_FLAGS=-DRECOMP_TRACE` for that plus a 1024-deep
  function-entry backtrace. It found both faults above in one run each.

### Lifted-CRT self-test (reach for this first)
`build-trace/mcg_recomp.exe MCX.EXE --selftest` calls the lifted CRT string
routines with known inputs and compares against the host's answers. When the
engine reads a file correctly and still cannot find what is plainly in it, run
this BEFORE reading any more disassembly -- it named two real lifter bugs in one
run. Add a case whenever a primitive is suspected: it is cheap, and it converts
"the parser does not work" into a function name.

### Two lifter bugs found that way (both fixed upstream in pcrecomp)
- **`cmp`/`sub` never published `_cf`.** The lazy flag triple served jcc/setcc,
  but `sbb r, r` reads the running `_cf` directly. `cmp` + `sbb eax,eax` is how
  the MSVC 6 CRT makes -1/0/+1, so **strcmp returned the wrong sign**. Fixed by
  emitting `_cf = CMP_B(_flag_a, _flag_b)` in both. This retired the
  long-standing `sbb.idiom` KNOWN divergence in difftest.
- **`repe cmpsd` was unimplemented** -- only byte-width `cmps`/`scas` existed,
  so it lifted to an empty statement. That is memcmp's aligned fast path, so
  **memcmp called two identical buffers different**, and the config parser found
  no keys in a file it had read correctly. Replaced the three narrow handlers
  with one covering every width, guarded by opcode (A6/A7/AE/AF) because
  `cmpsd` is also an SSE mnemonic.
- difftest went 70/74 (4 divergences) -> **82/85 (3 divergences), 0 failures**.
- **`precise_sbb=True` is NOT what fixed this** and is not sufficient alone: at a
  jump target the lifter cannot know which `cmp` set the flags, so it falls back
  to `_cf`. It stays enabled, harmlessly.

### Bring-up instrumentation (trace builds)
- `RECOMP_PC` records each instruction's VA, so a crash report names the
  faulting instruction and not just the function.
- `RECOMP_BALANCE` / `RECOMP_BALANCE2` wrap every direct/indirect call and
  report (a) the poison value turning up in a callee-saved register, naming the
  call that lost it, and (b) esp left *low* by <= 16 bytes. A large drop is NOT
  a leak -- `__chkstk` hands hundreds of bytes to its caller by design.
- `recomp_lookup_manual()` overrides a VA without regenerating. NOTE: a direct
  `RECOMP_CALL` never consults it, so an overridden VA must ALSO be withheld
  from the lifter's `func_names`/`lifted` sets -- that is what `OVERRIDES` in
  run_pipeline.py is for.

### Current bring-up stop
`systemInit` (`HonorB.obj`, 0x007590D0) makes a C++ virtual call at 0x0075982B
through an object whose vtable pointer is garbage — an object used before its
constructor ran. Most likely traced to the open `_initterm` over-pop below.

**Open: a stack OVER-pop inside `_initterm`'s constructor loop.** `pop esi`
retrieves the dummy return address, so esp ends 4 too HIGH. The balance check
cannot see it — that looks for esp too LOW, the signature of a dropped `ret`.
The canary found it instead: with `RECOMP_RETADDR=0xDEAD0000`, esi held the
poison at the CRT's WinMain call, so hPrevInstance was non-zero and the game
announced "An instance of this application is already running." The build
defaults to `RECOMP_RETADDR=0` as a MITIGATION, not a fix.

**Also open:** x87 80-bit values cannot round-trip through the lifter's 64-bit
`double`, so `%f` formatting (`$I10_OUTPUT`, `__fltout`, `__fptostr`) derails.
The engine's two error reporters are overridden in `recomp_lookup_manual()` to
print their message and skip the float.

**Disproven, do not retry:** (1) the environment block being empty — the host's
real one is passed through now; (2) `GetTimeZoneInformation` leaving its struct
stale — implemented now. Neither was the cause of anything.

## Phase 1/2 (done)
- `run_pipeline.py` drives the shared lifter: **5,760/5,760 functions, 0 lift
  errors**, 829,128 lines across 12 files. Compiles to **13 objects, 0 errors,
  0 warnings** (`cl /nologo /c /W1 /MP`).
- **Emit `lift32.FUNCTION_LOCALS` as the per-function preamble, never a
  hand-written list.** `ebp` and the x87 stack (`_st`/`_fp_top`/`_fpu_cw`) are
  deliberately GLOBAL in `recomp_types.h`, because an optimising compiler splits
  one frame across blocks that all address it through the same `ebp`. The mw3
  driver this was copied from declares them locally and is stale; copying it
  cost a compile cycle of `_flag_a`/`_flag_b`/`_flag_k` undeclared errors.
- **A one-byte function is a bare `ret`** -- an empty virtual override, 104 of
  them, all live in vtables. The inherited `if end - addr < 2: continue` guard
  silently dropped every one. Right for IDA-derived bounds, wrong for
  linker-derived ones.
- **Lifter bug found and fixed upstream** (`G:/recomp/pc/tools`, `lift32.py`):
  x86 masks shift counts to 5 bits for every operand width, so `shl [ecx],0x6e`
  shifts by 14 and `shl [edx],0xa0` shifts by 0 and writes no flags. The lifter
  emitted the raw immediate -> a shift wider than the C type -> UB. 172 sites
  here. Fixed in `shl/sal`, `shr`, `sar`, `shld`, `shrd` via `_shift_count()` /
  `_shift_out_cf()`; the rotate emitter already masked. Four cases added to
  `difftest.py`; suite is 74/78 with 4 pre-existing known divergences, 0
  failures. **That change is uncommitted in the toolbox** -- it shares a working
  tree with unrelated WIP in `lift32.py`'s `_flag_condition`.
- Build artifacts go to `build/obj/`, never into `src/recomp/gen/`: the stats
  loop counts files in the output directory and once reported 80 MB of "C"
  because it was counting `.obj` files.

## Phase 0 (done)
`tools/cv_extract.py` → `config/functions.json` (5,760), `analysis/cv_symbols.json`
(12,614), `analysis/cv_modules.json` (612), `analysis/fpo.json` (1,973).
98.7% of `.text` covered. `--selftest` asserts: table sorted, no overlapping
bounds, every entry inside `.text`, PE entry point present, demangling works,
source paths recovered. Re-run it after any change to the extractor.
