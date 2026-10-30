# Settings: mcg.ini

`mcg.ini` lives beside `mcg_recomp.exe` (`build\mcg.ini`). The first run writes
it with every key at its default and a comment saying what it does; delete it
to get the defaults back. A command-line flag beats the matching setting.

The game's own preferences -- difficulty, music, sound-effect and radio volume,
brightness -- are not here. They stay in `game\PREFS.CFG`, which the game's
options screen reads and writes as it always did. `mcg.ini` holds only what the
1999 game had no setting for.

```ini
[display]
fullscreen=0      ; 1 = borderless fullscreen      (flag: --fullscreen / --window)
scale=            ; window scale                    (flag: --scale)
res=              ; WxH                             (flag: --res WxH)

[sound]
master_volume=100 ; 0-100, on top of the game's own sliders
mute=0

[game]
unlock_missions=0 ; 1 = jump to any campaign mission from the briefing screen

[keys]
Q=A               ; physical key = what the game sees
```

`[display]` and `[sound]` are read by the presenter and the sound code
(`mcg_setting("display", "scale", NULL)`); see their own docs for what each
value does. Unset keys mean "the code's default".

## For code: reading a setting

```c
#include "settings.h"
const char* res = flag_res ? flag_res : mcg_setting("display", "res", NULL);
int vol = mcg_setting_int("sound", "master_volume", 100);
```

`settings_init()` runs first thing in `main`, so any code after the command
line is parsed can call these. Strings are copies; keep one past startup with
your own `strdup`.

## [game] unlock_missions

The campaign is a straight line of segments in `mechcmdr1.fit`; there is no
mission-select screen to unlock. What there is, left in the retail binary by
the developers, is a **mission jump**: `Logistics::processCheatCode`
(`0x006FFA50`) gets every key released on the briefing screen, and one of its
branches reads two typed digits as a scenario number, saves your current
roster as `start<N>`, and reloads it as the campaign at mission N. The
briefing for that mission comes up, with your 'Mechs, pilots and C-Bills.

Its enable flag (`0x00808654`) is only ever cleared in the retail build -- the
code that set it did not ship -- and it sits behind `cheatsOn`, the
`ixtlriimceourl` switch. `unlock_missions=1` sets both, and re-arms the flag
after each jump. Nothing else is changed: the jump, the save and the reload
are the game's own code.

On the briefing screen, type the two keys in the table (the digit row, not
the keypad). They are the scenario's index in `mechcmdr1.fit`, tens then
units -- except that the second key counts `0` as ten, so the round numbers
are typed one ten lower. That is the developers' arithmetic, kept as is.
Index 0 (the first mission) cannot be typed; New Campaign starts there.

| Op | Mission 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| 1 | New Campaign | `01` | `02` | `03` | `04` | `05` |
| 2 | `06` | `07` | `08` | `09` | `00` | `11` |
| 3 | `12` | `13` | `14` | `15` | `16` | `17` |
| 4 | `18` | `19` | `10` | `21` | `22` | `23` |
| 5 | `24` | `25` | `26` | `27` | `28` | `29` |

(Operation 3 lists mis0302 before mis0301, and operation 5 runs mis0501,
0502, 0504, 0505, 0503, 0507: the table follows the campaign file's order.)

A wrong second key cancels; start again. While the jump is armed the briefing
screen's own logistics cheats (which read the same keys) are off.

## [keys]

`physical=game`: pressing the left-hand key gives the game the right-hand one.
Names are a letter or digit, `F1`-`F12`, `SPACE TAB ENTER ESC BACKSPACE SHIFT
CTRL ALT UP DOWN LEFT RIGHT HOME END PGUP PGDN INS DEL`, or `0xNN` (a Windows
virtual-key code). A line that cannot be read is reported on stderr and
ignored.

The remap is applied host-side, before the engine sees anything:
`PeekMessageA` rewrites `WM_KEYDOWN`/`WM_KEYUP` (key and scan code, so the
`WM_CHAR` that `TranslateMessage` makes from it is the new key's), and
`GetAsyncKeyState`/`GetKeyState` poll the physical key bound to whatever the
engine asks about. A key moved away from its default reads as up. Scripted
typing (`--click ":text@t"`) goes through the same table.

### The game's default keys

The game has no keyboard config file; these are its own bindings, from the
shipped `Readme.txt` and the engine code.

| Key | Does |
|---|---|
| `A` + click | attack with energy weapons only (ammo conservation) |
| `BACKSPACE` | clear all orders, ammo conservation included |
| `1` + click | small artillery strike |
| `2` + click | large artillery strike |
| `3` + click | sensor probe |
| `4` + click | camera drone |
| held key + click | RUN, JUMP and GUARD (the original MechCommander's hotkeys; the MFD highlights the button) |
| `ESC` | skip a movie / leave a screen |
| `CTRL`+`ALT` chords | developer keys (polled through `GetAsyncKeyState`; most need `cheatsOn`) |

## Not built (cheap later)

- Remember the window position: one `[display] window=x,y` key, written on
  exit -- belongs with the display flags.
- Skip the intro movies: nothing to skip yet, the Smacker movies are not
  decoded (`SmackOpen` returns a movie that has already ended).
