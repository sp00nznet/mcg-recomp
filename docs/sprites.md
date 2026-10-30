# 'Mech sprites

How MechCommander Gold stores and draws its 'Mechs, read out of the engine's
own code (`MCX.EXE`'s CodeView symbols name every function below) and checked
against what the lifted engine decodes at run time. `tools/sprites.py` is a
standalone decoder that follows this page; its `--selftest` compares it pixel
for pixel with the engine.

**The short version:** a 'Mech sprite is a standard VFX shape table, the same
format as every `.SHP` in `SHAPES.FST`, behind a 6-byte prefix, LZ-packed one
gesture-and-direction per packet inside a PAK nested in a PAK. Nothing about
the row encoding is special to 'Mechs.

## Where they live

`data/sprites/` holds eight files, one per body part and resolution:

| file | part index | 'Mechs | packets each |
|---|---|---|---|
| `LEGS.PAK`, `LEGS90.PAK` | 0 | 19 | 189 = 21 groups x 9 directions |
| `TORSOS.PAK`, `TORSOS90.PAK` | 1 | 24 | 391 = 23 groups x 17 directions |
| `RARMS.PAK`, `RARMS90.PAK` | 2 | 24 | 391 |
| `LARMS.PAK`, `LARMS90.PAK` | 3 | 24 | 391 |

`SpriteManager::initMechPacketFiles` (0x00642B00) opens all eight from
`spritePath` (falling back to `CDspritePath`). The `90` files are the same
frames drawn at twice the scale for the close zoom (`use90PixelSprite`,
`Use90Pixel` in `PREFS.CFG`). Legs, torsos and arms are separate sprites: the
legs walk and the upper body turns on its own, so each part has its own set
of 'Mech indices (19 leg sets, 24 torso sets), chosen by the appearance type.

### PAK (PacketFile)

```
u32  0xFEEDFACE
u32  offset of packet 0           -> packet count = this / 4 - 2
u32  entry[count]                 low 29 bits: offset; high 3 bits: storage
     ... packet data ...          a packet ends where the next begins (last: EOF)
```

Storage 0 is raw, 2 is LZD (`u32` unpacked size, then FASA's LZW: 9-to-12-bit
LSB-first codes, 256 = clear, 257 = end), 7 is an empty packet. The format was
already decoded in mcgoldng; the one trap is the second dword, which is a
header field and not the first table entry (reading it as one shifts every
packet by one and makes packet 0 look empty).

Each part file's **outer** packets are stored raw and each is itself a PAK:
packet *m* is 'Mech set *m*. Its **inner** packets are LZD-packed, one per
gesture group and direction. `SpriteManager::getMechShapeData` (0x00643A00)
opens the inner PAK over the outer packet on first use, seeks packet
`group * dirs + direction`, reads it into the sprite heap and hands it to
`Shape::init` (0x006B4F50).

## A packet

```
u16  part index (0 legs, 1 torso, 2 right arm, 3 left arm)
u16  frame count (the same as the table's own count)
u16  0
     VFX shape table
```

`Shape::init` skips the 6-byte prefix unless the packet already begins with the
`"1.10"` version tag (it keeps a pointer to the prefix, at `Shape+4`), then
requires `VFX_shape_count() > 0` and that the first frame's offset is exactly
`8 + 8 * count`, i.e. the table is dense.

### VFX shape table

```
char[4] "1.10"
u32     frame count n
{u32 offset, u32 palette offset (0)}[n]      offsets from the table start
frame[n]
```

### A frame

```
u32  bounds   (w << 16) | h of the nominal cell: 75x75, or 150x150 in the 90 set
u32  origin   (x << 16) | y of the hotspot in that cell: (37,52), or (75,104)
i32  xmin, ymin, xmax, ymax      the drawn rectangle, relative to the hotspot
     rows: ymax - ymin + 1 of them, each a run of tokens ending in 0
```

The pixel at (xmin, ymin) is drawn at screen (x + xmin, y + ymin), where (x, y)
is the hotspot (the 'Mech's position); `xmax - xmin + 1` is the width. Frames
of one packet keep the same bounds and origin but their rectangles differ.

### Row encoding

Read a byte `t` (this is `_DrawShapeUnclipped`, 0x0076DD3C, and its C twin
`AG_shape_draw`, 0x006B7F00):

| `t` | meaning |
|---|---|
| `0` | end of row |
| `1` | skip: the next byte is a count of transparent pixels |
| even, > 1 | run: `t >> 1` copies of the next byte |
| odd, > 1 | literal: the next `t >> 1` bytes are pixels |

A row ends where its last opaque pixel does; whatever is right of it is
transparent. There is no transparent colour index: transparency is only the
skip token and the tail of a row, so every index 0-255 is a real colour,
0 and 255 included.

`AG_shape_fill` (0x006B7930), which the engine uses for mirrored and scaled
frames, differs in one way: it writes the skip pixels as index 0 rather than
leaving them, so its scratch buffer has 0 for transparent (see below).

## Directions, mirroring, animation

A group holds one packet per direction over **half** a turn, from facing 0 to
180 degrees inclusive: 9 for legs (22.5-degree steps, 16 headings in all) and
17 for the upper body (11.25 degrees, 32 headings). The other half is the same
frames drawn mirrored. `SpriteTree::getGesture` (0x00642340) folds the heading
into the half turn, sets the "reverse" flag when it folded, quantises it to
the part's direction count, and for a few gesture groups snaps it to every
fourth direction -- those groups (torso groups 12-17, 19 and 20) only have
packets at directions 0, 4, 8, 12, 16 and the rest are empty.

The frames inside a packet are the gesture's animation, in order (1 frame for
a stand, 16 for a walk cycle, 51 for the longest torso gesture). The gesture
to group mapping is the table at 0x0078DB9C (`0,1,2,3,4,5,6,7,8,4,5,9,...`):
several gestures share a group.

Drawing goes `MechActor::render` -> a `VFXElement` per part -> `scaleDraw`
(0x006B4BC0):

* not reversed, no zoom change: `AG_shape_draw` straight to the screen pane;
* reversed (or scaled): `AG_shape_fill` into a scratch buffer, then
  `CopySprite` blits it flipped horizontally around the hotspot (x from
  `x - xmax` to `x - xmin`), and at half or double scale for the zoom levels,
  through `AlphaTable` -- which is how index 0 in the scratch buffer comes out
  transparent.

`tools/sprites.py` writes frames unmirrored; mirror with `png(..., mirror=True)`.

## Palette and team colours

The battlefield palette is `data/palette/HB.PAL` in `MISC.FST`: a 4-byte header
and 256 6-bit VGA triples (multiply by 4). At run time the engine's
`gamePalette` matches it except entries 216-223, which it animates (the
colour-cycling range).

The shape draw has a translating twin for every routine (`AG_shape_translate_*`,
`VFX_shape_translate_draw`): each pixel goes through a 256-byte lookaside table
(`AG_shape_lookaside`). For 'Mechs, `VFXElement`'s table argument is
`MechActor::render`'s own argument, and `BattleMech::render` passes 0 -- so on
the battlefield 'Mech sprites are drawn **untranslated**: there is no per-team
palette remap of the sprite pixels. (One branch passes `-150` instead, under a
flag at `BattleMech+0x8B4` that was not seen set; what it is for is open.)
The logistics screens' 'Mech icons and repair diagrams do use translate draws,
with their own tables.

## How this was checked

`MCG_SPRITEDUMP=dir` (off by default) arms two hooks in the runtime, emitted
by `run_pipeline.py`'s `HOOK_CALLS`: at `getMechShapeData` it notes the part,
'Mech and packet, and at the `Shape::init` that follows it writes the packet
exactly as the engine read it (`.pkt`) and, for every frame, what the lifted
`AG_shape_draw` draws -- twice, over a buffer of 0x00 and one of 0xFF, so a
pixel the shape left transparent is the one that differs (`.eng`). It also
saves the live palette. Then

    py -3 tools/sprites.py --selftest DIR

re-reads each packet from the PAK files (proving the container path), decodes
every frame and compares.

Result, from a headless run of campaign mission 1 (the conformance click
script: deploy the lance, select a 'Mech, send it into the first fight):

    packets: 212/212 identical to the PAKs
    frames:  2293/2293 pixel-identical to the engine (183827 px), 4 part/mech sets

That run covers all four parts: legs set 7 and torso, right-arm and left-arm
set 9, the 45-pixel files, standing, walking and firing gestures. The capture
stops after 2,000 packets. A longer run, or other missions, would widen the
coverage to more 'Mech sets and to the 90 files. Nothing in the decode path
depends on which set or resolution it is reading.

An earlier capture through `AG_shape_fill` matched the fill model above
(skips written as 0, row tails untouched) on all 2,224 of its frames.

## Could it go HD?

Not the way the RA2 or Mechwarrior remasters do it: there are no source models
or high-resolution renders to re-render from. Every 'Mech exists only as these
8-bit frames (the largest seen in one 'Mech's 90 set is 73x59 pixels). An HD path would be:

1. **Upscale the decoded frames offline** -- every frame of every part, both
   sets, mirrored ones included if the upscaler is not mirror-symmetric -- with
   a model suited to pixel art on a palette (and then re-quantise to the
   palette if it is to go back into the 8-bit pipeline, or keep RGBA if not).
2. **Feed them back through a hook**, not through the files.
   `getMechShapeData` -> `Shape::init` is the one funnel every 'Mech frame
   passes through, and it already knows part, 'Mech and packet: a replacement
   packet could be handed in there.

Whether the engine's renderer would take larger frames:

* **Bigger frames in the same table: yes, up to a limit.** Nothing in the
  decoder cares about size. `scaleDraw` refuses (draws nothing) a frame whose
  bounds exceed 400 in either dimension or whose width and height differ by
  more than 256, and calls `Fatal(" Sprite too damned big ")` when
  `w * h >= 0x1FA40` (129,600 px, the scratch buffer). A 2x upscale of the 90
  set (150x150 cell -> 300x300) fits; 3x does not.
* **But bigger is not sharper.** The engine places a frame in screen pixels
  around its hotspot. A 2x frame draws a 'Mech twice as big on a battlefield
  that is not, which is exactly what the 90 set already is (the close zoom).
  To be sharper the whole frame has to be composed at a higher resolution,
  and the composite is an 8-bit (shell) or 16-bit 565 (mission) surface whose
  size is the display mode -- terrain, buildings, effects and HUD are all drawn
  into it at 1x by the same lifted rasteriser.
* So the practical route is in the **presenter**, not the engine: render at
  the native mode as now, and in `present()` (src/runtime/ddraw.c) replace
  what the 'Mech sprites drew with HD art drawn at the final scale. That needs
  the draw calls (`scaleDraw`'s shape, frame, position and reverse flag, which
  a hook at its entry has) recorded per frame and replayed over the upscaled
  image, with the depth order the element list already sorted. Feasible, but
  it is a renderer feature, not a data swap -- and the same treatment would be
  wanted for vehicles, buildings and terrain for it to look consistent.
