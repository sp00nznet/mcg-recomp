# MechCommander Gold — Static Recompilation

A static recompilation of **MechCommander Gold** / *Desperate Measures* (1998–99,
FASA Interactive / MicroProse), lifting the retail `MCX.EXE` to C for native
Windows 11.

This is a game-preservation project. It ships no game code and no game assets —
bring your own legally-obtained copy.

> *"Command the finest BattleMech pilots in the Inner Sphere."*

## Why this one is different

Nearly every project in this toolbox starts by *guessing* where the functions
are. This one doesn't have to.

`MCX.EXE` was linked from a build directory called `RelNoDbg` — release, no
debug — but the linker's **CodeView (NB11) debug stream was left appended to the
image anyway**, 1.79 MB of it in the PE overlay. It contains:

- **612 object modules**, each with its segment extents
- **12,614 public symbols** carrying **MSVC C++ decorated names** — so class,
  method, parameter types and calling convention, not just an address
- **179 source-line sections** naming the original files (`L:\mcx\Terrain\Terrain.cpp`)
- an **FPO table** of 1,973 frame records giving exact prologue and frame layout

The result is a function table that is *read*, not inferred: **5,760 functions
covering 98.7% of `.text`**, 5,113 of them with a full C++ signature. There is no
discovery heuristic in this repo because none is needed.

## Project status

| Phase | Status | Description |
|-------|--------|-------------|
| **Phase 0** | **Complete** | Binary triage, CodeView symbol recovery — **5,760 named functions, 98.7% of `.text`** |
| **Phase 1** | **Complete** | x86 → C code generation — **all 5,760 functions, 0 lift errors**, 829K lines |
| **Phase 2** | **Complete** | Compilation — **13 objects, 0 errors, 0 warnings** (MSVC, `/W1`) |
| **Phase 3** | **Complete** | Links and runs — **CRT startup completes, game code executes**, 204 import bridges |
| Phase 4 | Next | Win32 + DirectX 6 HAL (DDraw / DSound / DPlay / DInput) |
| Phase 5 | Pending | Engine abstraction — VFX rasteriser, terrain, sprite |
| Phase 6 | Pending | Asset loading (FST / PAK / SHP / FIT) |
| Phase 7+ | Pending | ABL script VM, logistics, mission layer, modern backend |

## Binary

| Property | Value |
|----------|-------|
| **Target** | `MCX.EXE` (3,607,856 bytes), built 1999-07-26 |
| **Original name** | `MechCmdr.exe`, from `.\RelNoDbg\` |
| **Compiler** | Visual C++ 6.0 (linker 6.00), C++ with decorated names |
| **Architecture** | x86-32, PE32, image base **`0x00600000`** (not the usual `0x00400000`) |
| **Code (.text)** | `0x00601000` – `0x007794A8` (1,541,288 bytes) |
| **Entry point** | `0x00762C24` |
| **DRM** | **none** — no packer, no protection, analyses directly |
| **Graphics/audio** | DirectDraw (`DirectDrawCreate`), DirectSound, DirectPlay (`DPLAYX` ordinal 4) |
| **Imports** | 204 functions from 12 DLLs |

The image base of `0x00600000` matters: the lifted image has to be mapped there,
and the simulated stack cannot sit below it the way it can for a `0x00400000`
target.

### The other binaries on the disc

| Binary | What it is | Use to us |
|--------|-----------|-----------|
| `MCX.EXE` | the game | **the target** |
| `MCXTRACT.EXE` | FASA's own asset extractor, MFC42, 11 KB of code | the FST archive format, written by the people who wrote the archiver |
| `HSBRMP.EXE` | Hasbro multiplayer launcher, linker 5.0, 3 MB of it resources | not part of the game |
| `AUTORUN.EXE`, `SETUP/`, `DIRECTX/` | installer and a DirectX 6 redist | ignore |

## Phase 0: CodeView symbol recovery

`tools/cv_extract.py` parses the appended debug stream and writes the function
table. It has no dependencies — the PE and CodeView parsing is direct, and
demangling goes through Windows' own `dbghelp!UnDecorateSymbolName`, which gets
MSVC decoration exactly right for free.

```
py -3.11 tools/cv_extract.py            # -> config/ + analysis/
py -3.11 tools/cv_extract.py --selftest # bounds, coverage and demangling invariants
```

| Artifact | Contents |
|----------|----------|
| `config/functions.json` | **5,760** functions — address, size, decorated name, demangled signature, COMDAT aliases |
| `analysis/cv_symbols.json` | **12,614** public symbols across all four sections |
| `analysis/cv_modules.json` | **612** object modules + the 179 recovered source paths |
| `analysis/fpo.json` | **1,973** FPO frame records |

Function bounds come from an FPO or proc record where one exists (2,038 of them,
exact to the byte) and from the next symbol's address otherwise. Bounds only ever
shrink, so they can never overlap — the self-test asserts exactly that, plus that
every entry lands inside `.text` and that the PE entry point is one of them.

MSVC 6 folds identical COMDATs, so several public symbols legitimately share one
address. Those are kept as one function with an `aliases` list rather than being
counted twice (191 of them).

### The engine, as the original build saw it

The source paths recovered from the debug stream map the whole engine before a
single instruction is lifted:

| Subsystem | Modules | Notes |
|-----------|--------:|-------|
| `object/` | 47 | `Mech`, `Gvehicl`, `Warrior`, `Move`, `MOVER`, `ELEMNTL` — the unit sim |
| `lib/` | 15 | support library |
| `abl/` | 14 | **ABL**, the mission scripting VM (`Ablxstd`, `Ablstd`) |
| `logistics/` | 13 | `logistics.obj` is the single biggest module at 102 KB |
| `engine/` | 12 | core |
| `sprite/` | 12 | sprite/shape rendering |
| `gui/` | 10 | widget layer |
| `vfx/` | 8 | the VFX software rasteriser |
| `linkup/` | 7 | DirectPlay multiplayer (`SessionManager`, `DPPlayer`) |
| `terrain/` | 4 | `Terrain`, `terrmap`, `TerrTxm`, `Vertex` |
| `iface/`, `mission/`, `ai/`, `camera/`, `appear/`, `Game OS/Sound Renderer/` | 2–4 each | |

## Phase 1: x86 → C code generation

`run_pipeline.py` drives the shared `pcrecomp` lifter over the CodeView table.
Because the bounds come from the binary's own debug data rather than a heuristic,
every entry lifts:

| Result | Value |
|--------|-------|
| Functions lifted | **5,760 / 5,760** |
| Lift errors | **0** |
| Output | 829,128 lines of C, 41.1 MB, 12 files |
| Time | ~9 s |

```
py -3.11 run_pipeline.py            # -> src/recomp/gen/
```

Generated functions keep the original names, so the output is greppable —
`Terrain__getTerrainWindow_0073B320`, not `sub_0073B320` — and each carries its
address, its object module and its demangled C++ signature as a comment.

Two things the symbols made visible that a heuristic run would have hidden:

- **104 functions are a single byte.** They are empty virtual overrides that the
  compiler reduced to a bare `ret`, and they sit in vtables, so dispatch needs
  them. The inherited driver skipped anything under two bytes — a sane guard when
  bounds come from IDA, wrong when they come from the linker.
- **`ebp` and the x87 stack are global in the runtime, not per-function.** An
  optimising compiler splits one frame across blocks that all address it through
  the same `ebp`; declaring it locally gives each block a frame pointer of zero.
  The lifter publishes the correct scratch list as `lift32.FUNCTION_LOCALS`, and
  that is what this driver emits.

## Phase 2: compilation

```
cl /nologo /c /W1 /MP /I . recomp_*.c      # 13 objects, 39.8 MB
```

**0 errors, 0 warnings.** Getting to zero warnings turned up a real semantics bug
in the shared lifter rather than anything specific to this game: x86 masks a
shift count to its low five bits before doing anything, so `shl dword [ecx],0x6e`
shifts by **14**, and `shl dword [edx],0xa0` shifts by **zero** and touches no
flags at all. The lifter emitted the raw immediate, which in C is a shift wider
than the type — undefined behaviour, and the compiler is free to hold the
operand, zero it, or use the low bits. This binary has 172 such sites.

Fixed in `pcrecomp`'s `lift32.py` (`shl/sal`, `shr`, `sar`, `shld`, `shrd` — the
rotate emitter already masked correctly), with four new cases in the toolbox's
differential harness, which runs the lifted C against Unicorn and compares every
register and flag: **74/78 match hardware, 0 failures**, the 4 remaining being
pre-existing documented divergences.

## Phase 3: it links, and it runs

```
py -3.11 gen_imports.py                      # -> src/runtime/imports_gen.c
cmake -S . -B build -G Ninja && cmake --build build
./build/mcg_recomp MCX.EXE
```

The CRT startup now runs to completion and the game's own code executes:

```
[runtime] mapped MCX.EXE at 0x00600000, 2.1 MB
[runtime] stack 0x00820000-0x00C20000
[shims]   heap arena 0x00C20000-0x10C20000
[runtime] 5878 lifted functions, 204 import bridges
[runtime] entering at 0x00762C24
          ... CRT init, heap, locale, argv/environ ...
[shims]   LoadLibraryA(".\IMAGEHLP.DLL") -> exports shimmed
```

### Argument counts are derived from three sources, never typed

Every shim has to pop exactly what the real API pops; a wrong count silently
desynchronises the simulated stack and the symptom appears nowhere near the
cause. `gen_imports.py` resolves all 204 imports without a single hand-written
number, from three independent places — and where two of them both have an
answer, it **checks them against each other** and fails on a disagreement:

| Source | Covers | |
|--------|-------:|---|
| Windows SDK 32-bit import libraries (`_Name@N`) | 192 | via pcrecomp's `stdcall_argc.py` |
| MCX.EXE's **own** CodeView thunk symbols | 193 | `_DirectPlayLobbyCreateA@20` → 5 args, stated by the linker that built the game |
| The import table's own names | 10 | `smackw32!_SmackOpen@12` — a DLL the SDK never heard of |
| Undecorated thunk symbol ⇒ cdecl | 1 | `_wsprintfA` is varargs: the caller cleans up, so the shim pops nothing |

192 were confirmed by two sources at once with **zero disagreements**. The same
machinery covers the 307 `imagehlp.dll` exports the game resolves at runtime
through `GetProcAddress`, which never appear in the import table at all.

### What bring-up found

- **`VirtualAlloc`'s alignment is part of its contract.** With `lpAddress` NULL
  the real API only ever returns 64 KB-granular addresses, and the MSVC 6
  small-block heap recovers a region from a block pointer by masking low bits.
  Handing it a 16-byte-aligned bump pointer made those masks yield a base that
  was itself an absolute address, and the first write through it landed 12 MB
  outside anything allocated.
- **A stub returning 0 is not neutral** — the house rule, earned again twice
  here. `RegOpenKeyExA` returning 0 is `ERROR_SUCCESS`, so six registry stubs
  each claimed to open a key and left the caller's `HKEY` stale. The registry is
  now forwarded to the host's, because that is where the settings really live.
- **The game requires `imagehlp.dll`** for its own crash reporter and calls a
  failed load a fatal exception, quitting before its main loop. It resolves
  symbols in a batch and gives up on the first one missing, so shims are
  generated for every export rather than for the names we happened to hit.
- **Every crash report names its instruction.** Generated code records the VA it
  is about to execute (free without `-DRECOMP_TRACE`), which turned "somewhere
  in this 777-byte function" into an exact address, twice.

### Two lifter bugs the engine found, and the test that named them

The engine read `system.cfg` correctly — the right bytes, verified in the
buffer — and then reported that every key in it was missing. When the I/O is
provably fine, the suspect is what the parser runs *on top of*: the CRT's
hand-written string routines, which lean on exactly the flag semantics a lifter
is most likely to get subtly wrong.

`mcg_recomp --selftest` calls each lifted routine with known inputs and compares
against the host's answer for the same inputs, so a failure names a function
instead of leaving a parser that "just doesn't work". It found two:

- **`cmp` and `sub` never published CF.** The lazy flag model served the
  `jcc`/`setcc` that ask by condition, but `sbb r, r` reads the running `_cf`
  directly — and that held whatever an unrelated earlier instruction left there.
  `cmp` + `sbb eax, eax` is exactly how the MSVC 6 CRT turns a comparison into
  −1/0/+1, so **`strcmp` returned the wrong sign**.
- **`repe cmpsd` was unimplemented** — only the byte forms of `cmps`/`scas`
  existed, so it lifted to an empty statement. That is the aligned fast path in
  `memcmp`, which then returned whatever was in `eax`: **`memcmp` called two
  identical buffers different.** Hence a parser that could not find a key in a
  file it had just read.

Both are fixed in [pcrecomp]'s `lift32.py`, each with new cases in the
differential harness. That suite went from **70/74 with 4 known divergences to
82/85 with 3 and 0 failures** — the `sbb` idiom divergence it had carried as a
documented ceiling is now simply correct.

With the config parsed, startup proceeds: the game creates its directories,
reads file times, registers its window class and enters `systemInit`.

### Where it stops

`systemInit` (`HonorB.obj`) makes a C++ virtual call through an object whose
vtable pointer is garbage — so an object is being used before its constructor
ran. That points at the one open item below.

Open, and honestly open rather than fixed:

- **A stack over-pop inside `_initterm`'s constructor loop.** `pop esi`
  retrieves the dummy return address instead of the saved `esi`, so `esp` is 4
  too *high*. The canary caught it (`RECOMP_RETADDR=0xDEAD0000` turned up in
  `esi`, which is how the single-instance check came to believe another copy was
  running), but the call-site balance check cannot see it: that check looks for
  `esp` too *low*, the signature of a dropped `ret`. The build defaults to
  `RECOMP_RETADDR=0` so a leaked zero is inert and bring-up continues; that is a
  mitigation, not a fix.
- **Lifted `__sbh_alloc_block`** faults building a block footer from a local
  that holds a pointer where a size belongs. The runtime sets `___sbh_threshold`
  to 0 — the CRT's own switch, what `_set_sbh_threshold()` writes — so
  allocation goes to `HeapAlloc` instead. A supported MSVC 6 configuration, but
  a sidestep.
- **x87 80-bit values.** `$I10_OUTPUT`/`__fltout` read an extended value's raw
  bytes, and the lifter models the x87 stack as a 64-bit C `double`, so `%f`
  cannot round-trip. The engine's two error reporters are overridden in
  `recomp_lookup_manual()` to print their message and skip the float, because
  the message is the point.

Ruled out, so as not to be retried: the environment block (the host's real one
is passed through now) and `GetTimeZoneInformation` leaving its struct stale
(implemented now). Neither was the cause of anything.

## Layout

```
config/     functions.json                    the recomp's function table (committed)
analysis/   cv_symbols, cv_modules, fpo       derived binary metadata (committed)
tools/      cv_extract.py                     project-specific scripts
src/        recompiled / HAL / engine sources (generated, not committed)
docs/       analysis notes & handoffs
```

The shared toolbox lives at `G:/recomp/pc/tools` ([pcrecomp]) and is referenced,
not vendored — a stale copy of the lifter's `recomp_types.h` is a known way to
lose an afternoon.

> **Game data is not in this repo.** The retail install sits in this same folder
> and the `.gitignore` is an allowlist, so no game file can be committed by
> accident. Supply your own legally-obtained copy.

## Prior work

[`mcgoldng`](https://github.com/sp00nznet/mcgoldng) is the *clean-room*
reimplementation of the same game — an SDL2/OpenGL engine that reads the retail
assets. Its asset work stands on its own and is not superseded: FST and PAK
archive readers, LZ decompression, the FIT config parser, VFX shape tables and
palettes are all decoded there. This repo is the other approach to the same
problem — the original code, lifted — and the two meet at the asset layer.

[pcrecomp]: https://github.com/sp00nznet/pcrecomp
