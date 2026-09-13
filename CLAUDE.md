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

## Phase 0 (done)
`tools/cv_extract.py` → `config/functions.json` (5,760), `analysis/cv_symbols.json`
(12,614), `analysis/cv_modules.json` (612), `analysis/fpo.json` (1,973).
98.7% of `.text` covered. `--selftest` asserts: table sorted, no overlapping
bounds, every entry inside `.text`, PE entry point present, demangling works,
source paths recovered. Re-run it after any change to the extractor.
