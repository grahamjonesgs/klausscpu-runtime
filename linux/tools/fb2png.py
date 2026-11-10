#!/usr/bin/env python3
"""fb2png.py — a KlaussCPU /dev/fb0 snapshot to PNG (no third-party modules).

  fb2png.py [--screen] SHOT.klfb OUT.png
      SHOT from `fbtest shot` on the board (linux/userland/fbtest.c):
      "KLFB <xres> <yres> <bpp> <stride>\\n", for 8 bpp a 768-byte palette,
      then the visible page.
  fb2png.py [--screen] --raw W H BPP [--stride N] [--yoffset Y]
            [--palette CMAP] RAW OUT.png
      RAW from `cat /dev/fb0` (or dd); CMAP is `fbtest cmap` output (256
      lines "r g b"), needed for 8 bpp (else a grey ramp).

The PNG is the image at its own resolution, RGB565 widened to 8 bits per
channel.  --screen renders the 640x480 picture as the VGA port shows it:
4 bits per channel, 320-pixel modes doubled, centred vertically, the rest
black (the driver's border colour).

linux/tools/fbshot.sh does the SSH and conversion in one step.
"""
import struct
import sys
import zlib


def png(path, w, h, rows):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + bytes(r) for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def decode(data, w, h, bpp, stride, pal):
    """Rows of r,g,b bytes, 8 bits per channel."""
    rows = []
    for y in range(h):
        line = data[y * stride:y * stride + w * bpp // 8]
        out = bytearray(w * 3)
        if bpp == 8:
            for x, i in enumerate(line):
                out[3 * x:3 * x + 3] = pal[i]
        else:
            for x in range(w):
                p = line[2 * x] | line[2 * x + 1] << 8
                r, g, b = p >> 11, (p >> 5) & 0x3F, p & 0x1F
                out[3 * x] = r << 3 | r >> 2
                out[3 * x + 1] = g << 2 | g >> 4
                out[3 * x + 2] = b << 3 | b >> 2
        rows.append(out)
    return rows


def screen(rows, w, h, bpp):
    """The 640x480 picture: 4-bit channels (the palette is already RGB444
    in the hardware), doubling, centring."""
    dbl = w == 320
    shown = h * 2 if dbl else h
    vstart = (480 - shown) // 2 if shown < 480 else 0
    black = bytearray(640 * 3)
    out = [black] * vstart
    for r in rows:
        q = bytearray(len(r))
        for i, c in enumerate(r):
            v = (c * 15 + 127) // 255 if bpp == 8 else c >> 4
            q[i] = v * 17
        if dbl:
            q2 = bytearray(640 * 3)
            for x in range(320):
                q2[6 * x:6 * x + 3] = q[3 * x:3 * x + 3]
                q2[6 * x + 3:6 * x + 6] = q[3 * x:3 * x + 3]
            q = q2
        out += [q, q] if dbl else [q]
    out = out[:480]
    out += [black] * (480 - len(out))
    return out


def main(argv):
    scr = "--screen" in argv
    argv = [a for a in argv if a != "--screen"]
    if len(argv) >= 2 and argv[0] != "--raw":
        src, dst = argv
        with open(src, "rb") as f:
            blob = f.read()
        nl = blob.index(b"\n")
        magic, w, h, bpp, stride = blob[:nl].split()
        if magic != b"KLFB":
            sys.exit("fb2png: %s is not an `fbtest shot` file" % src)
        w, h, bpp, stride = int(w), int(h), int(bpp), int(stride)
        off = nl + 1
        pal = None
        if bpp == 8:
            pal = [tuple(blob[off + 3 * i:off + 3 * i + 3]) for i in range(256)]
            off += 768
        data = blob[off:]
    elif len(argv) >= 6 and argv[0] == "--raw":
        w, h, bpp = int(argv[1]), int(argv[2]), int(argv[3])
        rest = argv[4:]
        stride = w * bpp // 8
        yoff = 0
        pal = None
        while rest and rest[0].startswith("--"):
            opt, val = rest[0], rest[1]
            rest = rest[2:]
            if opt == "--stride":
                stride = int(val)
            elif opt == "--yoffset":
                yoff = int(val)
            elif opt == "--palette":
                with open(val) as f:
                    pal = [tuple(int(v) for v in l.split()) for l in f if l.strip()]
            else:
                sys.exit("fb2png: unknown option " + opt)
        if len(rest) != 2:
            sys.exit(__doc__)
        src, dst = rest
        with open(src, "rb") as f:
            data = f.read()[yoff * stride:]
        if bpp == 8 and pal is None:
            pal = [(i, i, i) for i in range(256)]
    else:
        sys.exit(__doc__)
    if bpp not in (8, 16):
        sys.exit("fb2png: %d bpp is not a KlaussCPU VGA format" % bpp)
    if len(data) < stride * (h - 1) + w * bpp // 8:
        sys.exit("fb2png: %d bytes, need %d for %dx%d at stride %d"
                 % (len(data), stride * h, w, h, stride))
    rows = decode(data, w, h, bpp, stride, pal)
    if scr:
        rows = screen(rows, w, h, bpp)
        w, h = 640, 480
    png(dst, w, h, rows)
    print("fb2png: %s, %dx%d" % (dst, w, h))


if __name__ == "__main__":
    main(sys.argv[1:])
