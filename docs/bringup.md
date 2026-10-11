# Bring-up notes

What it took to get `MCX.EXE` from "lifts" to "a mission runs", and what was
ruled out on the way. Each entry was paid for with at least one wasted run;
read the relevant one before chasing a symptom that looks familiar.

Code: `src/runtime/recomp_runtime.c` (memory layout, dispatch, watchdog,
self-test), `src/runtime/shims_impl.c` (Win32), `src/runtime/ddraw.c` (the COM
mocks and the presenter). How the parts fit: [architecture.md](architecture.md).

## The binary

- **`MCX.EXE` ships full CodeView (NB11) debug info in its PE overlay**:
  1.79 MB at file offset `0x1BBC60`, debug directory type 2, plus type 3 (FPO,
  1,973 records) and type 4 (MISC, the string `.\RelNoDbg/MechCmdr.exe`). So no
  function-discovery heuristic is needed for the bulk of the program.
- **12,614 `S_PUB32` symbols with MSVC C++ decorated names.** `S_PUB32` is
  `0x1009` in CV5's numbering and its layout is
  `typind(u32) off(u32) seg(u16) name(pstr)`: `typind` comes *first*, which is
  not what every reference says. 612 `sstModule`, 179 `sstSrcModule` (source
  paths under `L:\mcx\`).
- **Only 65 `S_GPROC32` records.** A `RelNoDbg` build keeps publics and line
  info but drops most per-module procs, so exact byte lengths come from FPO
  (2,038 exact; the other 3,722 are bounded by the next symbol).
- **Image base `0x00600000`**, not `0x00400000`.
- **No DRM, no packer.** `.text` entropy 6.28; it analyses directly.
- **MSVC 6 folded identical COMDATs**: 191 addresses carry more than one public
  symbol. One function with aliases, not several; anything keyed by address
  must dedupe or it emits duplicate C symbols.
- **Demangling is `dbghelp!UnDecorateSymbolName` via ctypes.** pcrecomp's
  `tools/cpp/msvc_mangler.py` is a *mangler*, not a demangler.
- **`MCXTRACT.EXE`** is FASA's own asset extractor (11 KB of code, MFC42): lift
  that, not the game, if the FST format ever needs settling.
  **`HSBRMP.EXE`** is the Hasbro multiplayer launcher, not the game.

## Memory layout

Image `0x00600000` (+2.1 MB), stack `0x00820000-0x00C20000` (4 MB), heap arena
`0x00C20000-0x10C20000` (256 MB reserved, committed on demand). Synthetic VAs
for `GetProcAddress` results live at `0x11000000+` and COM vtable slots at
`0x12000000+`, deliberately above everything mapped so a stray one is
unmistakable. The host is x64; the whole simulated range sits far below
anything the host occupies, so simulated VA = host address.

## Function discovery: five sources, each forced by a bug

The CodeView publics cover 98.7% of `.text`, and the missing 1.3% is where the
bugs were. `run_pipeline.py` adds four more sources at lift time:

| Source | Adds | Why nothing else finds it |
|---|--:|---|
| CodeView publics | 5,760 | |
| FPO records | +94 | describe a frame regardless of linkage |
| Data pointer tables | +54 | a static constructor is never *called*; its address only sits in a table `_initterm` walks. The one at `0x0074D4A0` never ran |
| Call targets, and branches that *leave* a function | +158 (with the next row) | `_strcpy` is `push edi` + `jmp` into the middle of `_strcat`'s neighbour. Unresolved, the generated `return` left the pushed `edi` and the dummy return address on the stack: **8 bytes leaked per strcpy**, which shifted every later stack local |
| Code addresses loaded as immediates | | `mov esi, 0x00721C10`, then called through the register: in no table, no branch, no symbol. `AG_ellipse_fill` came in this way; without it the mission drew no terrain |

Scan immediates on **non-branch** instructions only: a branch immediate is the
branch target and source 3 has it already, and scanning them splits a function
at every `jcc`. **The function total is the tell: a discovery change must make
it go up.** An early version that skipped branches with `continue` swallowed
source 3 and the count fell 5,981 -> 5,964.

Splitting a function to make such an entry is safe: the part before falls
through to it, and registers (`ebp` included) are global, so one frame can span
separately lifted blocks.

### Decode hazards the driver handles

- **Embedded switch tables.** MSVC puts a dense switch table in `.text` right
  after its code. Decoded linearly it is nonsense and the decode goes out of
  phase. `_switch_tables()` finds `jmp [reg*4 + disp]` with `disp` inside the
  function, walks entries while they stay in range, skips those bytes and adds
  the targets as labels. A dense switch emits *two* tables (a byte index table
  read by `movzx r, byte [r+disp]`, then the dword table): only the second is
  reachable from the `jmp`, so the first used to be decoded as code.
- **Data-sweep entries must be instruction boundaries.** `0x00646F6D` got in
  as a "pointer" four bytes into a `push` and truncated `HeapManager::destroy`.
  `is_boundary()` decodes forward from the covering entry; 9 are rejected.
- **`int3` is padding, not the end of a function.** `handleEvent` lost both its
  `ret`s, fell out the bottom without its epilogue and never restored the
  caller's `ebp`.
- **One-byte functions are real**: 104 empty virtual overrides reduced to a
  bare `ret`, all live in vtables.
- **Stale generated chunks**: `run_pipeline.py` clears its output first, or a
  leftover `recomp_00NN.c` defines everything twice at link time.

## Lifter bugs this game found (all fixed in pcrecomp)

- **`rep movs`/`stos` ignored the direction flag.** With `std` a real CPU walks
  down; `VFX_pane_copy` takes that path for an overlapping blit, so the lifted
  copy ran past its destination over a live C++ object. That was every
  mysterious heap corruption. Fixed with `memmove`; after it the engine stopped
  crashing.
- **`cmp`/`sub` never published `_cf`**, which `sbb r, r` reads directly: the
  MSVC 6 CRT's `strcmp` returned the wrong sign.
- **`repe cmpsd` was unimplemented**, so `memcmp`'s aligned fast path called
  identical buffers different and the config parser found no keys.
- **`setcc` after `test` read the wrong macro**: `setle` after `test r, r`
  became `r <= r`. `PacketFile::seekPacket` failed every call and the campaign
  would not load.
- **Shift counts are masked to 5 bits** for every width (`shl [ecx], 0x6e`
  shifts by 14). 172 sites here emitted a C shift wider than the type.

## Win32 shims

- **`gen_imports.py` derives every argument count from three sources and makes
  them agree**: the SDK import libraries, MCX.EXE's own decorated CodeView thunk
  symbols, and the import table's own names (smackw32). An undecorated thunk
  symbol is positive evidence of cdecl (`_wsprintfA`, pops 0). It also checks
  every hand-written shim's `STDRET`. **Never hand-tune a STDRET.**
- **`VirtualAlloc` must return 64 KB-aligned addresses.** The MSVC 6 small-block
  heap masks low bits off a block pointer to find its region.
- **`___sbh_threshold` (`0x007A714C`) is set to 0 at startup**, the CRT's own
  `_set_sbh_threshold` switch, so every allocation goes to `HeapAlloc`. A
  supported configuration, and also a sidestep: lifted `__sbh_alloc_block`
  faults with a pointer where a size belongs. Open.
- **The game needs `imagehlp.dll`** (`LoadLibraryA(".\IMAGEHLP.DLL")` then a
  batch of `GetProcAddress`) or it quits. Shims exist for all 307 SDK exports,
  because it gives up on the first missing name.
- **A stub returning 0 is not neutral.** `ERROR_SUCCESS`, `DD_OK`, `S_OK` are
  all 0: the Reg* stubs "succeeded" and left the out `HKEY` stale, and
  struct-filling APIs left structs uninitialised. Registry and those structs
  forward to the host.
- **Test your own handle tag first.** `map_hkey` checked `v >= 0x80000000` for
  predefined roots before its own tag, and the tag (`0xF4E70000`) is above that.
- **`waveOutGetNumDevs` reports 1**: zero wave devices is fatal ("No Digital
  Sound Hardware Installed").
- **SessionManager reaches DirectPlay through `CoCreateInstance(CLSID_DirectPlay,
  IID_IDirectPlay3A)`**, and `IDirectPlayLobby::GetConnectionSettings` must
  return `DPERR_NOTLOBBIED (0x8877042E)`.
- **`LoadStringA` must be real**: stubbed, the game showed an empty dialog
  43,000 times. It is answered from MCX.EXE's `.rsrc` loaded as a datafile.
- **The multimedia timer must work.** `timeSetEvent` drives the 50 ms heartbeat
  (proc `0x0061EE80`). It is serviced from `PeekMessage` on the main thread on
  purpose: registers and stack are global, so a real timer thread would corrupt
  whatever the main thread was doing.
- **`WM_PAINT` must be validated**, or it regenerates forever and the engine
  repaints instead of reaching `UpdateDisplay`.
- **The media check.** `checkForCDInDrive()` (`0x00758DA0`) looks for
  `\data\tiles\gtiles90.pak` and `\hidden.txt` from a drive *root*. The runtime
  reports the install drive as the media and retries root-anchored paths under
  the install folder, only when the result exists (`redirect()`).
- **x87 80-bit values** cannot round-trip through the lifter's 64-bit `double`,
  so `__fassign` and `__cfltcvt` are replaced with native `strtod`/`snprintf`
  in `recomp_lookup_manual()`. A direct `RECOMP_CALL` never consults that table,
  so an overridden VA is also withheld from the lifter (`OVERRIDES` in
  `run_pipeline.py`).

## The COM mocks (`ddraw.c`)

DDRAW, DSOUND and DPLAYX each contribute one import; everything after is vtable
dispatch, so each mock is an object graph in simulated memory whose vtable
slots are synthetic VAs resolved by `ddraw_lookup()`.

- `IDirectDraw2` needs its own vtable: `SetDisplayMode` takes two more args.
- Unimplemented vtable slots stay **null** on purpose: a null slot is reported
  by address, a guessed `STDRET` desyncs the stack silently.
- DirectSound buffers are real arena memory, never played. DirectPlay reports
  no providers.
- **The mission is 16bpp, the shell 8bpp, both at 1280x1024.** The mock used to
  refuse 16bpp, so the terrain renderer never got a surface and the
  battlefield was black under a working HUD. Surfaces carry their own bpp and
  `present()` unpacks 565.
- **The shell is a 640x480 layout inside that mode.** `present()` picks its
  source rectangle by probing a sparse grid outside the 640x480 box; colour
  depth is not a reliable tell. `ddraw_map_cursor` maps through the same
  rectangle or clicks land off target. `MCG_NOSCALE=1` shows the raw mode.

## Diagnostics (trace build)

Build with `set BUILD_DIR=build-trace & set CMAKE_ARGS=-DCMAKE_C_FLAGS=-DRECOMP_TRACE`.

- `--selftest`: lifted CRT string routines, `FitIniFile::textToLong`, the
  printf float path, the three VFX blitters and `AG_ellipse_fill`, each against
  the host's answer. **Run it first** when the engine reads a file correctly
  and still cannot find what is in it: it named two lifter bugs in one run.
- `--watchdog S` (or `MCG_WATCHDOG=1`): every 3 s, the running function, the
  PC, the shadow call depth and the message counters. "PC unchanged" with a
  flat peek count is a hang; with peek climbing it is a hot function.
- `MCG_WATCH=0xADDR`: polls one simulated dword at every call boundary and
  names the call that changed it. That is how `VFX_pane_copy` was caught.
  **About a hundred times too slow to drive the UI**: never leave it armed
  while scripting. Hooking a hot function through `OVERRIDES` is just as slow.
- A fault report prints the faulting instruction VA, the window-proc depth, a
  hex dump around `ecx`/`esi`/`edi`, the shadow call stack (the path the crash
  is *on*) and a ring of the last 32 call sites.
- `RECOMP_BALANCE` checks esp/ebp around every call. A large esp drop is not a
  leak: `__chkstk` hands hundreds of bytes to its caller by design.
- `-DRECOMP_RETADDR=0xDEAD0000u` puts the canary return address back, so a
  stack imbalance shows up as a poisoned register instead of an inert zero.

## Scripted input

`--click` (or `MCG_CLICK`) takes `x,y@t` clicks and `x,y>x2,y2@t` drags, in
the engine's own 640x480 shell coordinates, with times in seconds **from the
moment `Mission::run` reaches state 3** (the logistics shell): the intro movies
make startup vary by tens of seconds. This one starts a mission:

```
477,165@8;477,165@18;477,165@28;37,376>248,61@45;37,376>304,61@55;37,376>246,172@65;570,339@78;570,339@90
```

New Campaign, three drags from the Ready Area into force groups, then launch.
`MCG_CURSOR=x,y` pins the pointer when nothing is scripted (headless does that
by default), because a real pointer resting near a screen edge scrolls the
battlefield.

## Ruled out, do not retry

1. `FitIniFile::textToLong` returning bad counts: all self-test cases pass.
2. "No Digital Sound Hardware Installed": `waveOutGetNumDevs` reports 1.
3. Window-procedure re-entrancy: crash reports print `window-proc depth: 0`.
4. `_initterm` over-popping: 30 constructors, 0 unbalanced.
5. The three VFX blitters: self-tested at unaligned widths and off-origin panes.
6. Missing art: every `data\art\*.tga` miss in the log is the engine probing for
   a loose file before falling back to the packfile.
7. The environment block, and `GetTimeZoneInformation` leaving its struct stale.

## Open

- **No move order has been accepted yet.** Selecting a 'Mech works and an order
  onto impassable ground is refused with the engine's own red-X cursor, so
  input reaches the simulation. What is missing is a valid destination: ask the
  engine which terrain it considers passable rather than guessing pixels.
- The intro Smacker movies are not decoded (`SmackOpen` returns a one-frame
  movie that has already finished).
- The `__sbh_alloc_block` fault above.
- Sound is silent.
