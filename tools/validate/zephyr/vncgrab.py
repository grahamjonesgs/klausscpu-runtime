# vncgrab.py host out.png  -- RFB 3.8, security None, one full raw update, 32bpp truecolour
import socket, struct, sys
from PIL import Image
h, out = sys.argv[1], sys.argv[2]
s = socket.create_connection((h, 5900), timeout=60)
def rx(n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise EOFError
        b += c
    return b
rx(12); s.sendall(b"RFB 003.008\n")
n = rx(1)[0]; types = rx(n); s.sendall(b"\x01"); assert struct.unpack(">I", rx(4))[0] == 0
s.sendall(b"\x01")
w, ht = struct.unpack(">HH", rx(4)); pf = rx(16); nl = struct.unpack(">I", rx(4))[0]; name = rx(nl)
print("server", w, ht, name)
# SetPixelFormat: 32bpp, depth 24, little endian, truecolour, r/g/b max 255, shifts 16/8/0
s.sendall(struct.pack(">B3xBBBBHHHBBB3x", 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
s.sendall(struct.pack(">BBHi", 2, 0, 1, 0))  # SetEncodings: raw
s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, ht))
img = Image.new("RGB", (w, ht))
got = 0
while got < w * ht:
    t = rx(1)[0]
    if t != 0:
        if t == 1: rx(3); n = struct.unpack(">H", rx(2))[0]; rx(6 * n); continue
        if t == 2: continue
        if t == 3: rx(3); rx(struct.unpack(">I", rx(4))[0]); continue
        raise SystemExit("msg %d" % t)
    rx(1); nr = struct.unpack(">H", rx(2))[0]
    for _ in range(nr):
        x, y, rw, rh, enc = struct.unpack(">HHHHi", rx(12))
        assert enc == 0, enc
        d = rx(rw * rh * 4)
        tile = Image.frombytes("RGB", (rw, rh), d, "raw", "BGRX")
        img.paste(tile, (x, y)); got += rw * rh
img.save(out); print("saved", out)
