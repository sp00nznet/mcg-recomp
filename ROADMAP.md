# Roadmap

The menus, the campaign shell and the first mission run, recompiled
throughout. What follows turns that into a game you can play start to finish.
Each milestone lands with a conformance milestone and a note in `docs/`.

## 1. Play a mission

- **A move order the engine accepts.** Selection works and an order onto
  impassable ground is refused with the red-X cursor, so input reaches the
  simulation. Find a destination the engine itself calls passable (ask its map,
  don't guess pixels), issue it, and check the 'Mech's position changes.
- **Combat**: attack an enemy, take damage, a unit destroyed.
- **Mission end**: objectives complete, the debriefing and salvage screens.
- **The third drag**: the scripted deployment lands two 'Mechs of three.

## 2. Sound and movies

- **DirectSound to the host**: the mock's buffers are real memory the engine
  already fills; play them through WASAPI or XAudio2.
- **Smacker movies**: the intro and the mission videos. `SmackOpen` currently
  hands back a movie that has already finished. Decoding must be our own (no
  copyleft decoder; see REPO_RULES section 3).

## 3. Correctness debts

- `__sbh_alloc_block` faults; the small-block heap is switched off with the
  CRT's own `_set_sbh_threshold` switch rather than fixed.
- x87 80-bit values: `__fassign` and `__cfltcvt` are native replacements.
- 14 `UNIMPLEMENTED` instructions in the lift: 12 are bytes decoded as code
  (`sti`, `insb`, `hlt`, `daa`, `fnstenv`/`fldenv` in the CRT's FP exception
  path), and two are `lahf` in the Cyrix CPU check, where the unimplemented
  result happens to give the right answer ("not a Cyrix"). Implementing `lahf`
  belongs in pcrecomp.

## 4. Housekeeping

- **netlab**: the recipe builds on the farm once its clang-cl builder handles a
  64-bit host with 1.5M lines of generated C; until then the local
  `build.cmd` is the build.
- Screenshots of a mission in motion (a GIF from `--record`).

## Out of scope

- Multiplayer over DirectPlay. The mock reports no providers, which is true; a
  modern transport would be its own project.
- The mission editor (`MCEDSETP`) and `HSBRMP.EXE` (the Hasbro launcher).
