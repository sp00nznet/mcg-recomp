# Architecture

Three Python steps turn your copy of `MCX.EXE` into C, and one C host runs it.
Nothing derived from the binary is committed: it all lands in gitignored
`work/`, `src/recomp/gen/` and `src/runtime/imports_gen.c`.

```
game/MCX.EXE
   |
   |  tools/cv_extract.py      CodeView NB11 + FPO from the PE overlay
   v
work/functions.json, fpo.json, cv_symbols.json, cv_modules.json
   |
   |  run_pipeline.py          pcrecomp lift32 over the table, plus four
   |                           discovery sources (docs/bringup.md)
   v
src/recomp/gen/recomp_00NN.c, recomp_dispatch.c, recomp_funcs.h, recomp_types.h
   |
   |  gen_imports.py           one shim or stub per import, argument counts
   |                           derived and cross-checked, never typed
   v
src/runtime/imports_gen.c
   |
   |  build.cmd (CMake + Ninja, MSVC x64)
   v
build/mcg_recomp.exe --run     runs in game/, reads the game's own data
```

## The parts

| Part | Owns |
|---|---|
| `tools/cv_extract.py` | Reading the debug stream: 5,760 functions with exact or bounded sizes, demangled through `dbghelp`. `--selftest` asserts the table is sorted, non-overlapping, inside `.text`, and contains the entry point |
| `run_pipeline.py` | Function discovery beyond the publics, switch tables, and driving `lift32` into 13 chunks of C. Every generated function keeps the original name (`Terrain__getTerrainWindow_0073B320`) and its demangled signature |
| `gen_imports.py` | The import bridge table. Hand-written bodies are listed in `HAND_WRITTEN`; their `STDRET`s are checked against the derived counts on every run |
| `src/runtime/recomp_runtime.c` | `main`: the command line, mapping the image at `0x00600000`, the stack, entry. Dispatch, the watchdog, the shadow call stack, fault reports, `--selftest`, and the native overrides in `recomp_lookup_manual()` |
| `src/runtime/shims_impl.c` | Win32: the heap arena, files (forwarded to the host with tagged handles), registry, windows and messages, the timer, scripted input, `--headless` |
| `src/runtime/ddraw.c` | DirectDraw, DirectSound and DirectPlay as object graphs in simulated memory; `present()` and `--record` |
| pcrecomp `runtime/recomp32` | `image_loader.c`, `crash_report.c`, `recomp_types.h` (the register file, flags and memory macros the lifted code is written against) |

## Why the host is x64 when the game is x86

The lifted code models a 32-bit machine in portable C: registers are globals,
memory is addressed by simulated VA. The image, stack and arena are mapped at
their real addresses inside the host, low enough (under 0x11000000) that an
x64 process has them free, so a simulated VA *is* a host address and no
translation happens on any load or store. A 32-bit host would collide with its
own image at those addresses.

## Why registers are global

`ebp`, `esp`, the x87 stack and the flags live in `recomp_types.h` as globals,
not per-function locals. MSVC 6 splits one frame across blocks that all address
it through the same `ebp`, and discovery deliberately splits functions at
outside branch targets; a local `ebp` gives each piece a frame pointer of zero.
The same globals are why the multimedia timer is serviced on the main thread:
a second thread entering lifted code would trample them.

## Why the window is the engine's, not a presenter's

MechCommander is a software renderer: it locks a DirectDraw surface, writes
8- or 16-bit pixels and flips. The mock's surfaces are arena memory the engine
draws into directly, and `present()` converts the visible region to a 32-bit
DIB and blits it to the game's own window (framed rather than the fullscreen
popup it asks for). `--record` reads that DIB, not the screen, which is why it
works headless and over RDP.
