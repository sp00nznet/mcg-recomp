#!/usr/bin/env python3
"""Decode MechCommander Gold 'Mech sprites from the retail files (docs/sprites.md).

    py -3 tools/sprites.py --list
    py -3 tools/sprites.py --mech 0 [--part torsos] [--90] [--out out/sprites]
    py -3 tools/sprites.py --selftest DUMPDIR

Reads game/DATA/SPRITES/{LEGS,TORSOS,RARMS,LARMS}[90].PAK and the palette from
game/MISC.FST, and writes one PNG per frame. Output is game art: it goes to a
gitignored directory (default out/sprites) and is never committed.

--selftest compares against what the engine itself decoded: run the recomp
with MCG_SPRITEDUMP=DUMPDIR (see docs/sprites.md) and point this at DUMPDIR.
Stdlib only.
"""
import argparse
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(ROOT, 'game')
PARTS = ['legs', 'torsos', 'rarms', 'larms']     # SpriteManager part index 0..3
# Packets per gesture group: one per direction over a half turn, the other
# half drawn mirrored. Legs turn in 22.5-degree steps, the upper body in 11.25.
DIRS = {'legs': 9, 'torsos': 17, 'rarms': 17, 'larms': 17}


# ---------------------------------------------------------------- containers
def lzw(src, size):
    """FASA's LZW: 9..12-bit codes, 256 = clear, 257 = end, LSB-first."""
    out = bytearray()
    bb = nb = pos = 0
    bits, maxi, free = 9, 512, 258
    tab = {}
    oc = os_ = 0
    first = True
    while len(out) < size:
        while nb < bits and pos < len(src):
            bb |= src[pos] << nb
            pos += 1
            nb += 8
        if nb < bits:
            break
        c = bb & ((1 << bits) - 1)
        bb >>= bits
        nb -= bits
        if c == 257:
            break
        if c == 256:
            bits, maxi, free = 9, 512, 258
            first = True
            continue
        if first:
            oc = os_ = c
            out.append(c)
            first = False
            continue
        new, st = c, []
        if c >= free:
            st.append(os_)
            c = oc
        while c >= 258:
            c, sf = tab[c]
            st.append(sf)
        os_ = c
        out.append(c)
        out += bytes(reversed(st))
        tab[free] = (oc, os_)
        free += 1
        if free >= maxi and bits < 12:
            bits += 1
            maxi <<= 1
        oc = new
    return bytes(out)


def pak(b):
    """PacketFile: FEEDFACE, u32 first-packet offset, then one u32 per packet
    (low 29 bits offset, high 3 bits storage type). Returns packet payloads."""
    if b[:4] != b'\xce\xfa\xed\xfe':
        raise ValueError('not a PAK')
    first = struct.unpack_from('<I', b, 4)[0]
    n = first // 4 - 2
    ent = struct.unpack_from('<%dI' % n, b, 8)
    out = []
    for i, e in enumerate(ent):
        off = e & 0x1FFFFFFF
        end = (ent[i + 1] & 0x1FFFFFFF) if i + 1 < n else len(b)
        kind, data = e >> 29, b[off:end]
        if kind == 2:                       # LZD: u32 unpacked size + LZW
            data = lzw(data[4:], struct.unpack_from('<I', data)[0]) if data else b''
        elif kind == 7:                     # NUL
            data = b''
        elif kind != 0:
            raise ValueError('packet %d: storage type %d' % (i, kind))
        out.append(data)
    return out


def fst_file(fst, name):
    f = open(os.path.join(GAME, fst), 'rb').read()
    for i in range(struct.unpack_from('<I', f)[0]):
        o, c, u = struct.unpack_from('<III', f, 4 + i * 262)
        p = f[16 + i * 262:16 + i * 262 + 250].split(b'\0')[0].decode('latin1')
        if p.lower() == name.lower():
            return lzw(f[o:o + c], u) if c < u else f[o:o + c]
    raise KeyError(name)


_outer = {}


def mech_packets(part, mech, hires=False):
    """One 'Mech part's packets: legs 21 groups x 9 directions (189), the
    others 23 x 17 (391). Some groups exist only at every 4th direction."""
    key = (part, hires)
    if key not in _outer:
        fn = os.path.join(GAME, 'DATA', 'SPRITES', part.upper() + ('90' if hires else '') + '.PAK')
        _outer[key] = pak(open(fn, 'rb').read())     # outer packets stay raw
    return pak(_outer[key][mech])


def palette():
    """data/palette/HB.PAL: 4-byte header, 256 x 6-bit VGA RGB."""
    p = fst_file('MISC.FST', 'data\\palette\\HB.PAL')[4:4 + 768]
    return bytes(min(255, v * 4) for v in p)


# ---------------------------------------------------------------- shapes
def shape_table(pkt):
    """A 'Mech packet is a 6-byte prefix (u16 0, u16 frame count, u16 0) and
    then a standard VFX shape table. Shape::init skips the prefix unless the
    packet already starts with the "1.10" version tag."""
    return pkt if pkt[:4] == b'1.10' else pkt[6:]


def frame_count(tbl):
    return struct.unpack_from('<I', tbl, 4)[0]


def decode(tbl, i):
    """-> (xmin, ymin, w, h, pixels, opaque). Coordinates are relative to the
    hotspot; pixels/opaque are w*h bytearrays."""
    off = struct.unpack_from('<I', tbl, 8 + 8 * i)[0]
    _bounds, _origin, x0, y0, x1, y1 = struct.unpack_from('<IIiiii', tbl, off)
    w, h = x1 - x0 + 1, y1 - y0 + 1
    pix, opq = bytearray(w * h), bytearray(w * h)
    s = off + 24
    for y in range(h):
        x = y * w
        while True:
            t = tbl[s]
            s += 1
            n = t >> 1
            if t == 0:                      # end of row
                break
            if t == 1:                      # skip: next byte is the count
                x += tbl[s]
                s += 1
            elif t & 1:                     # odd: n literal bytes follow
                pix[x:x + n] = tbl[s:s + n]
                opq[x:x + n] = b'\1' * n
                s += n
                x += n
            else:                           # even: run of n copies of one byte
                pix[x:x + n] = bytes([tbl[s]]) * n
                opq[x:x + n] = b'\1' * n
                s += 1
                x += n
    return x0, y0, w, h, pix, opq


def png(path, w, h, pix, opq, pal, xlat=None, mirror=False):
    rows = []
    for y in range(h):
        r = bytearray(b'\0')
        xs = range(w - 1, -1, -1) if mirror else range(w)
        for x in xs:
            k = y * w + x
            v = xlat[pix[k]] if xlat else pix[k]
            r += pal[v * 3:v * 3 + 3] + (b'\xff' if opq[k] else b'\0')
        rows.append(bytes(r))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))


# ---------------------------------------------------------------- commands
def cmd_list():
    for part in PARTS:
        for hires in (False, True):
            fn = os.path.join(GAME, 'DATA', 'SPRITES', part.upper() + ('90' if hires else '') + '.PAK')
            n = len(pak(open(fn, 'rb').read()))
            print('%-8s %-5s %d mechs' % (part, '90' if hires else '45', n))


def cmd_dump(a):
    pal = palette()
    out = os.path.join(a.out, '%s%s_m%02d' % (a.part, '90' if a.hires else '', a.mech))
    os.makedirs(out, exist_ok=True)
    nfr = 0
    for k, pkt in enumerate(mech_packets(a.part, a.mech, a.hires)):
        if not pkt:
            continue
        tbl = shape_table(pkt)
        for i in range(frame_count(tbl)):
            x0, y0, w, h, pix, opq = decode(tbl, i)
            if w > 0 and h > 0:
                png(os.path.join(out, 'g%02d_d%02d_f%02d.png' % (k // DIRS[a.part], k % DIRS[a.part], i)),
                    w, h, pix, opq, pal)
                nfr += 1
    print('%d frames -> %s' % (nfr, out))


def cmd_selftest(dump):
    """Every packet the engine loaded must (1) equal what this script reads out
    of the PAKs for the same part/'Mech/packet and (2) decode, frame by frame,
    to exactly what the engine's AG_shape_fill drew: the .eng file holds each
    frame filled over 0x00 and over 0xFF, so opaque = the two agree."""
    pk = ok_pk = fr = ok_fr = px = 0
    mechs = set()
    bad = []
    for name in sorted(os.listdir(dump)):
        if not name.endswith('.pkt'):
            continue
        stem = name[:-4]
        f = stem.split('_')                      # partN[_90]_mM_pP
        part, hires = PARTS[int(f[0][4:])], f[1] == '90'
        mech, packet = int(f[-2][1:]), int(f[-1][1:])
        mechs.add((part, hires, mech))
        eng_pkt = open(os.path.join(dump, name), 'rb').read()
        pk += 1
        if mech_packets(part, mech, hires)[packet] == eng_pkt:
            ok_pk += 1
        else:
            bad.append('%s: packet bytes differ from the PAK' % stem)
        tbl = shape_table(eng_pkt)
        e = open(os.path.join(dump, stem + '.eng'), 'rb').read()
        o = 0
        while o < len(e):
            i, ex0, ey0, ew, eh = struct.unpack_from('<5i', e, o)
            o += 20
            a, b = e[o:o + ew * eh], e[o + ew * eh:o + 2 * ew * eh]
            o += 2 * ew * eh
            if ew <= 0 or eh <= 0:
                continue
            fr += 1
            x0, y0, w, h, pix, opq = decode(tbl, i)
            same = (x0, y0, w, h) == (ex0, ey0, ew, eh) and all(
                (a[k] == b[k] == pix[k]) if opq[k] else (a[k] == 0 and b[k] == 0xFF)
                for k in range(w * h))
            if same:
                ok_fr += 1
                px += w * h
            else:
                bad.append('%s frame %d differs' % (stem, i))
    for m in bad[:20]:
        print('  FAIL', m)
    print('packets: %d/%d identical to the PAKs' % (ok_pk, pk))
    print('frames:  %d/%d pixel-identical to the engine (%d px), %d part/mech sets'
          % (ok_fr, fr, px, len(mechs)))
    return 0 if pk and ok_pk == pk and ok_fr == fr else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--mech', type=int)
    ap.add_argument('--part', default='legs', choices=PARTS)
    ap.add_argument('--90', dest='hires', action='store_true', help='the 90-pixel (zoomed) set')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'sprites'))
    ap.add_argument('--selftest', metavar='DUMPDIR')
    a = ap.parse_args()
    if a.selftest:
        return cmd_selftest(a.selftest)
    if a.list:
        return cmd_list()
    if a.mech is None:
        ap.error('--mech, --list or --selftest')
    cmd_dump(a)


if __name__ == '__main__':
    sys.exit(main())
