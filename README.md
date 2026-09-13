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
| **Phase 1** | **Complete** | x86 → C code generation — **5,981 functions, 0 lift errors**, 1.49M lines |
| **Phase 2** | **Complete** | Compilation — **13 objects, 0 errors, 0 warnings** (MSVC, `/W1`) |
| **Phase 3** | **Complete** | Links and runs — **CRT startup completes, game code executes**, 204 import bridges |
| **Phase 4** | **In progress** | Win32 + DirectX 6 HAL — **renders and navigates: title, menus, campaign loads** |
| **Phase 5** | **In progress** | Engine abstraction — **a mission runs; the tactical view faults in `AG_ellipse_fill`** |
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

### Function discovery needs four sources, and each was forced by a bug

The CodeView publics cover 98.7% of `.text`, and the missing fraction is not
inert — it is where the bugs were:

| Source | Adds | Why nothing else finds it |
|--------|-----:|---------------------------|
| CodeView publics | 5,760 | — |
| FPO records | +94 | describe a frame regardless of linkage |
| **Data pointer tables** | +63 | a static constructor is never *called*; its address only sits in a table `_initterm` walks |
| **Call targets + branches that leave a function** | +157 | see below |

Two of those were found by running, not by reading:

- **A static constructor at `0x0074D4A0` never ran.** `_initterm` reported it
  unresolved, because nothing referenced it except a table in `.data`.
- **`_strcpy` leaked 8 bytes per call.** It is a five-byte stub — `push edi`,
  then `jmp` to its real body, which sits *after* `_strcat` — so its jump target
  is in the middle of another function. A branch leaving a function lifts to
  `RECOMP_ITAIL`, and dispatch resolves function *entries* only; unresolved, the
  generated `return` left the pushed `edi` **and** the dummy return address on
  the stack. That shifted every later stack local, which is how `systemInit`
  came to read an uninitialised vtable pointer out of a stack object.

Splitting a function to create such an entry is safe here: the part before it
falls through to it, and registers (`ebp` included) are deliberately global so
one frame can span separately-lifted blocks.

### The COM layer: DirectDraw, DirectSound, DirectPlay

The engine imports exactly one symbol from each of DDRAW, DSOUND and DPLAYX
(`DirectDrawCreate`, and the other two by ordinal). Everything after that is COM
vtable dispatch, so there is no import to shim — what is needed is an object
graph in **simulated** memory whose vtable slots are addresses the dispatcher
can resolve back to C.

That reuses the mechanism the `imagehlp` exports already needed: each method
gets a synthetic VA, the vtable is an array of those VAs built in the arena, and
`recomp_lookup_import` maps them back. A lifted `call dword ptr [eax + 0x54]`
then lands in `dd_SetDisplayMode`. The whole DirectDraw startup now runs:

```
[ddraw] DirectDrawCreate -> object 0x018BF4A0
[ddraw] QueryInterface {B3A6F3E0...} -> IDirectDraw2     <- IID_IDirectDraw2
[ddraw] SetCooperativeLevel(flags=00000013)
[ddraw] SetDisplayMode2 1280x1024 8bpp
[ddraw] CreateSurface 1280x1024 caps=00004218 back=1
[ddraw] present target 1280x1024                          <- a frame composited
[dsound] DirectSoundCreate -> object (silent)
```

MechCommander is a software renderer: it locks a surface, writes 8-bit pixels,
and flips. So the back buffer is arena memory the engine draws into directly,
and presenting is a palette lookup into a 32-bit DIB that the host BitBlts to
the window. `IDirectDraw2` needs its own vtable rather than a cast — its
`SetDisplayMode` takes two more arguments and `GetAvailableVidMem` is appended.

Unimplemented vtable slots are left **null** on purpose. An unimplemented COM
method has no argument count to guess, and a wrong `STDRET` desynchronises the
stack silently, whereas a null slot is reported by address.

### How far it gets

It plays the menus. The title screen, the main menu and the logistics shell all
render, the mouse tracks, and clicking **New Campaign** loads the campaign and
reaches **Briefing/Deployment** -- which draws in full: the navigation column,
the three force groups, the tactical map with its numbered objectives, drop
weight, and a 'Mech loadout with armour, internals and payload.

```
[runtime] mapped MCX.EXE at 0x00600000, 2.1 MB
[initterm] 5 constructors, 0 unbalanced
[initterm] 26 constructors, 0 unbalanced      <- all of them, none unresolved
[win] popup -> framed window, client 1280x1024
[ddraw] SetDisplayMode2 1280x1024 8bpp
[ddraw] CreateSurface 1280x1024 caps=00004218
[watchdog] func=0x0061E2E0 pc=0x0061EC0C peek=8003 dispatch=17 wndproc=117
```

The call stack under the frame loop reads
`WinMain -> RealWinMain -> aSystem::run -> aCallback::exec -> RunMission ->
Mission::run`, so the engine is inside its own top-level state machine, not
parked in startup. It runs for minutes without faulting.

**The media check.** `checkForCDInDrive()` -- the binary names it itself --
walks `A:` to `Z:` for a drive reporting `DRIVE_CDROM`, then looks for
`\data\tiles\gtiles90.pak` and `\hidden.txt` **from the drive root**, because in
1999 the content was on a disc whose root *was* the data directory. Installed
into a subfolder, those paths cannot resolve on any machine. The runtime answers
the question it is really asking -- where does the content live -- by reporting
the install drive as the media and retrying root-anchored paths under the
install directory, and only when the result exists. Both files it wants are the
game's own, already present. (`MCX.EXE` carries no DRM: Phase 0 found no packer
and no protection indicators.)

### Three bugs that stood between "it runs" and "it draws"

**`int3` is padding, not a wall.** MSVC pads between basic blocks with `int3`,
and the disassembler stopped at the first one. `handleEvent` lost everything
past `0x611564` -- including both of its `ret`s -- so the lifted body fell out
through the bottom without running its epilogue, never restored the caller's
`ebp`, and the corruption cascaded all the way up to `aCallback::exec`. Fixing
it eliminated every remaining ebp-clobber at once. The same pass also learned
that a dense MSVC switch emits *two* tables: a byte index table read with
`movzx r, byte [r+disp]`, then the dword target table the `jmp` uses. Only the
second is reachable from the `jmp`, so the first was decoded as code -- and in
`translateMessage` those bytes happen to disassemble to `popal`.

**`WM_PAINT` must be validated.** Without a `ValidateRect`, the message is
regenerated immediately and the engine spends every frame repainting instead of
reaching `UpdateDisplay`. It looked like a hang; it was a paint storm.

**Windowed cursor coordinates.** The engine believes it owns a fullscreen
display and reads the pointer straight out of `GetCursorPos` in those
coordinates. Windowed, a raw desktop point lands outside a screen that as far as
the engine knows has no edges, so `DrawMouse`'s save-under width
(`clipRight - x + 1`) went to -46, `shr ecx, 2` turned that into `0x3FFFFFF4`,
and the `rep movsd` that saves the pixels under the cursor walked out of the
arena. `GetCursorPos` now maps the point back through the client rect and the
same scale `present()` uses; `ClipCursor` and `SetCursorPos` speak those
fullscreen coordinates too, so both became no-ops rather than confining or
warping the host pointer.

### The bug that was hiding behind everything

`test a, b` leaves CF=0 and OF=0 and sets ZF/SF from `a & b`, so a condition
after it compares that RESULT against zero -- not the two operands against each
other. The lifter knew that and had the mapping, but the lookup was keyed on the
raw mnemonic while the table is keyed on jcc spellings. So `jle` after a test
correctly got `TEST_LE`, and `setle` after the identical test fell through to
`CMP_LE(a, b)`.

For the `test r, r` idiom that reads `r <= r`, true for every value a register
can hold. `PacketFile::seekPacket` builds its return code out of exactly that
sequence -- `xor/test/setle/dec/and/add`, where the success path relies on the
add wrapping to zero -- so it returned its failure code on every call and
`Logistics::loadCampaign` gave up with "could not find initial campaign file".
Every `setcc` and `cmovcc` in the binary carrying an ordering condition off a
test was wrong the same way, which is also why the menu widgets had spent days
rendering as 2-pixel bars.

Fixed upstream in pcrecomp, with ten difftest cases against hardware: each of
`setle`/`setg`/`setl`/`setge`/`cmovle` after a test, run with the register
positive, zero and negative, plus the whole `seekPacket` tail end to end.

### Where it stops

Open, and honestly open rather than fixed:

- **A mission runs; the battlefield view faults.** Deploying three 'Mechs
  satisfies `Logistics::requiredAssigned()`, the launch button fires,
  `Scenario::init` populates the ObjectMap correctly (cells 37–66, valid bucket
  pointers — the world-to-map-cell float maths is sound), and control reaches
  the in-mission frame loop. It then reads a null pointer while drawing units:

  ```
  RunMission -> Mission::run -> UpdateDisplay+0x1A0
    -> TacticalMap::display+0x8A4
      -> TacticalMap::drawObjects+0xAD9
        -> AG_ellipse_fill      ecx=FFFFFFFF, bad read of 3
  ```

  `ecx` is a count gone negative — the same shape as the VFX blitter bugs, a
  hand-written asm primitive whose loop bound the lifter mis-derived. The fix
  route is a `--selftest` case, which is how each of those was settled.
- **Each Ready Area entry draws only ~160 of its 2392 pixels** -- nine distinct
  palette indices, 2233 of them the wipe colour. This is NOT what blocks
  deployment: the entries are live objects with working hit-testing whether or
  not they draw, and the drag lands on them regardless. The blit chain is
  intact and correctly aimed at every stage; half the 25x30 parts
  `Logistics::drawMechBodyLoc` composes leave the destination untouched, which
  for a transparent blit means an all-`0xFF` source -- possibly the 'Mech's own
  armour data arriving empty rather than a drawing fault.
- **The nav column does not respond** to scripted clicks on the briefing screen
  (MAIN MENU / PURCHASING / 'MECH BAY), though the main menu's own items do.
- **The shell renders 640x480 into a 1280x1024 mode**, which the window then
  stretches. Present #8700 reads exactly 307,200 source bytes, so this is the
  engine's own choice rather than a presentation bug.
- **Lifted `__sbh_alloc_block`** faults building a block footer from a local
  that holds a pointer where a size belongs. The runtime sets `___sbh_threshold`
  to 0 -- the CRT's own switch, what `_set_sbh_threshold()` writes -- so
  allocation goes to `HeapAlloc` instead. A supported MSVC 6 configuration, but
  a sidestep.
- **x87 80-bit values.** MSVC 6's float paths keep values as 12-byte long
  doubles and the lifter models the x87 stack as a 64-bit C `double`, so they
  cannot round-trip. Two heads of that chain are replaced with native code in
  `recomp_lookup_manual()`: `__fassign` (native `strtod`) and `__cfltcvt`
  (native `snprintf`). The second was a crash, not a rounding difference --
  `__output` reaches it through `[0x7A7150]` for every `%e/%f/%g`, the lifted
  body came back with the caller's `ebp` gone, and `__output` then read its own
  locals through a null frame pointer. The first menu that formatted a number
  died there.

Ruled out, so as not to be retried: the environment block (the host's real one
is passed through now), `GetTimeZoneInformation` leaving its struct stale
(implemented now), `_initterm` over-popping (30 constructors, 0 unbalanced), and
the three blitters above. None was the cause of anything.

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
