# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
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
