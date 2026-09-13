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
| Phase 1 | Next | x86 → C code generation via the shared `pcrecomp` lifter |
| Phase 2 | Pending | Compilation — MSVC x86 static lib |
| Phase 3 | Pending | Executable link: register/memory model, CRT init, import bridges |
| Phase 4 | Pending | Win32 + DirectX 6 HAL (DDraw / DSound / DPlay / DInput) |
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
