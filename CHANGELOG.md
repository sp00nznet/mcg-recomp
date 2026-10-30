# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- **Smacker movies play** (`src/runtime/smacker.c`): a decoder written from the
  format description (palette, 4x4 MONO/FULL/SKIP/FILL video, DPCM audio) and
  real bodies for all ten smackw32 imports. The FASA logo, the opening
  cinematic, briefing and pilot videos decode; audio is decoded to s16 and
  pushed to the host stream API in `audio.h`
  and mixed with the game sound. Frames are paced to the movie's own clock. Build smacker.c with
  `-DSMK_TEST` for a standalone check that dumps frames and a WAV.
  `MCG_NOMOVIES=1` makes every movie one frame long; `conformance.py` sets it.
- **Sound**: DirectSound buffers play. `audio.c` mixes them on waveOut at
  44.1 kHz with each buffer's rate, volume and pan, and moves the play cursor
  in real time so the engine's streaming works; `Lock` wraps around the ring
  and a duplicated buffer has its own cursor. `MCG_VOLUME`, `MCG_MUTE`, and
  `MCG_WAVDUMP=file.wav` to hear a headless run (headless never opens a
  device, but the clock still runs). A stream API for movie sound.
- **The 'Mech sprite format is settled** ([docs/sprites.md](docs/sprites.md)):
  a standard VFX shape table behind a 6-byte prefix, one LZ-packed packet per
  gesture and direction in a PAK nested per 'Mech in `LEGS/TORSOS/RARMS/LARMS
  [90].PAK`. Read out of the engine's own `getMechShapeData` / `Shape::init` /
  `AG_shape_draw`.
- `tools/sprites.py`: a standalone decoder that writes a 'Mech's frames as
  PNGs (to the gitignored `out/`), with `--selftest` against the engine.
- `MCG_SPRITEDUMP=dir` (off by default): the runtime writes every 'Mech packet
  the engine loads and what its lifted `AG_shape_draw` makes of each frame.
  Hooked through the new `HOOK_CALLS` table in `run_pipeline.py`.
- `mcg.ini` beside the exe (`src/runtime/settings.c`): written with defaults on
  first run, read with the Win32 profile API; `mcg_setting(section, key, dflt)`
  for the display and sound flags to fall back on. docs/settings.md.
- `[keys]` remapping, host-side: `PeekMessageA` rewrites key messages (key and
  scan code), `GetAsyncKeyState`/`GetKeyState` poll the bound physical key.
- `[game] unlock_missions=1`: arms the developers' mission jump left in
  `Logistics::processCheatCode` (enable flag `0x00808654`, never set in the
  retail build). Two digits on the briefing screen go to any campaign mission
  with your roster.
- Scripted typing (`:text@t`) also posts the `WM_KEYUP` with its scan code, as a
  real key does, and goes through `[keys]`. The briefing screen reads keys
  from `WM_KEYUP`.
- `LOG_CALLS`: `Logistics::processCheatCode` and `Logistics::loadCampaign`.
- `--selftest` checks the `[keys]` parser.
- `--fullscreen` (borderless, over the monitor, no mode switch) and
  `--window`; `--scale sharp|smooth|integer`. The picture keeps its aspect in
  any window, with black bars, and the mouse maps through the same rectangle.
- `--res WxH` (up to 3840x2160): the game runs at that resolution and a
  mission shows that much more battlefield. Done by replacing PREFS.CFG's
  resolution after `_systemInit` and scaling the mission's
  `VisibleVerticesPerSide` as it is read; no engine code patched
  ([docs/hires.md](docs/hires.md)).
- `--fps`: engine and display frame rates in the corner and on stderr.

### Changed
- Presenting runs on its own thread, paced by `DwmFlush` (one blit per
  compositor frame, newest frame wins), so the engine never waits on the
  display and game timing is unchanged.
- The shell is no longer stretched to the window's 5:4 shape: aspect is kept.
- `--record` records the window as shown (its client size, bars included).

### Fixed
- **A fresh install stalled before the shell.** The shipped `PREFS.CFG` has no
  `Resolution` line (the 1999 installer's MCCONFIG.DLL wrote it), so a copy of
  the CD left the engine with display mode 0 at 640x480 and it never set a
  display mode. The runtime now supplies mode 3, 1280x1024, when it is
  missing. Earlier test runs passed only because the development copy's
  `PREFS.CFG` already had the line.
- Stale README screenshots of the main menu, deployment and mission.

## [0.1.0] - 2026-10-30

First release: boots, plays the menus, and plays the first campaign mission
through to its debriefing. Silent, and the movies are skipped.

### Added
- A Windows installer (recomp-launcher, `launcher/launcher.json`,
  `.github/workflows/installer.yml`): it finds your copy of the game, builds
  the recomp from it and leaves a Play button. It is attached to each release.
- `CONTRIBUTING.md`.
- **Mission 1 plays through to "MISSION SUCCESSFUL"**: Camp Alpha's HQ and gas
  tower, the three Garrison buildings, Camp Beta's guard and HQ, then the
  debriefing with its statistics, objectives and salvage. Attack orders,
  objectives and the end of a mission all ran with nothing to fix.
- `tools/conformance.py --playthrough`: plays it unattended (~18 minutes) and
  scores the three objectives and the debriefing.
- `--click` types text with `:text@t` (posted as `WM_CHAR`, which is how the
  engine reads typed characters, its cheat codes included), and takes 24
  steps.
- `MCG_PEEKF=0xADDR[,N]`: N simulated floats on each watchdog line.
- `LOG_CALLS` entries can print arguments: an order's code and world goal,
  objective status changes, the part ID of whatever is destroyed.
- **Combat works**: in the first campaign mission a 'Mech sent down to the
  road meets an armored car, fires, and destroys it, entirely in the engine's
  own code; nothing needed fixing. The conformance run now does exactly that
  (15/15): new milestones for a 'Mech firing and an enemy vehicle destroyed.
- `LOG_CALLS` covers weapon fire, weapon hits, unit destruction, attack
  orders and `Mission::EndScenario`.
- **Move orders work**: select a 'Mech, click open ground, and it walks there.
  The conformance run does that too.
- `LOG_CALLS` in `run_pipeline.py`: a lifted function can print a line on
  each of its first 20 calls (`Parser::SendTacOrder` does), which is how the
  harness sees a player order happen. Direct calls never reach
  `recomp_lookup_manual()`, so this lives in the generated body.
- `--click` takes up to 16 steps (was 8).
- `Setup.cmd`: one double-click from a plain download to a playable build. It
  checks Python, pefile/capstone, pcrecomp and Visual Studio (asking before
  installing any), copies your copy of the game from a drive, folder, `.iso`
  or `.zip` into `game\`, lifts, builds, and leaves a shortcut.
- `build.cmd`: the MSVC x64 + Ninja build without opening a developer prompt.
- The house command line: `--run`, `--game DIR`, `--headless`,
  `--record out.mp4`, `--frames N`, `--watchdog S`, `--click SPEC`. Without
  `--run` the host maps and binds the image and stops.
- `--headless`: a DWM-cloaked window that never reaches a screen or takes
  focus; the real keyboard and mouse are ignored, only scripted input counts.
- `--record`: the presented frames piped to ffmpeg at 30 fps, paced by the
  wall clock, from the frame buffer rather than the screen.
- `[mission] state A -> B` in the log whenever `Mission::run` changes state.
- `tools/conformance.py` and `conformance.json`: milestones of a headless run
  that starts a mission, the self-test count and lift health, against a
  baseline that fails on regression.
- CI: the Python compiles, `cv_extract`'s and the lifter's self-tests where they
  need no game, and the conformance harness's no-corpus path.
- A recomp-netlab recipe (`mcg`): clang-cl builds the tree on the farm in
  about three minutes, and that build passes the harness 12/12 as well.
- `docs/architecture.md`, `docs/bringup.md`, `docs/toolkit.md`, `ROADMAP.md`,
  `NOTICE`.

### Changed
- The game lives in `game\` instead of the repository root, and the host runs
  there (`--game` to point elsewhere).
- Everything derived from `MCX.EXE` (the CodeView function table, symbol and
  module maps, FPO records, lift statistics) is written to gitignored `work\`
  instead of `config\` and `analysis\`.
- The toolkit is found through `PCRECOMP`, else `..\tools`, instead of a fixed
  path. Built against pcrecomp `main` (`8f4aa77`): no unmerged toolkit change is
  needed any more.
- `Setup.cmd` takes `-GameDir` (`-Game` still works), never waits for Enter
  under the installer, and looks for the toolkit in `..\pcrecomp` first.
- `gen_imports.py` is now in the repository; the build needed it and it was
  never committed.

### Removed
- `config/functions.json` and `analysis/*.json` from the repository: they are
  symbol maps reconstructed from the binary (REPO_RULES section 3). Run
  `tools/cv_extract.py` to produce them locally.

### Fixed
- Every mission click landed about 48 px left and 70 px above where it was
  aimed. `GetCursorPos` already answers in the engine's own screen space, and
  `MapWindowPoints`/`ClientToScreen` were forwarded to Windows, which
  subtracted the framed window's desktop position a second time. Both are the
  identity now: to the engine, its window IS the screen.
- `ddraw.c` redefined the SDK's `E_NOINTERFACE` (a `/WX` error in CI).
- Link error against current pcrecomp: the runtime now defines
  `g_icall_from`, which `recomp_types.h` declares for trace builds.

[Unreleased]: https://github.com/sp00nznet/mcg-recomp/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/sp00nznet/mcg-recomp/releases/tag/v0.1.0
