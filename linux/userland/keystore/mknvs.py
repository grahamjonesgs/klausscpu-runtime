#!/usr/bin/env python3
"""mknvs.py: build Zephyr NVS images (subsys/kvss/nvs, CONFIG_NVS_DATA_CRC=y,
write-block-size 1) for kskey's tests, the way nvs.c writes them: data from a
sector's start (with its CRC-32), 8-byte ATEs down from the sector's end, the
last slot reserved for the close ATE, a gc_done ATE after a sector advance.

  mknvs.py <out> <scenario> <key.der>
"""
import struct
import sys
import zlib

SS = 0x10000
COUNT = 12
ATE = 8


def crc8_ccitt(val, data):
    t = [0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15,
         0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d]
    for b in data:
        val ^= b
        val = ((val << 4) & 0xff) ^ t[val >> 4]
        val = ((val << 4) & 0xff) ^ t[val >> 4]
    return val


def crc16_ccitt(seed, data):
    for b in data:
        e = (seed ^ b) & 0xff
        f = (e ^ (e << 4)) & 0xff
        seed = ((seed >> 8) ^ (f << 8) ^ (f << 3) ^ (f >> 4)) & 0xffff
    return seed


def name_id(name):
    i = crc16_ccitt(0xffff, name.encode())
    return 0xfffe if i == 0xffff else i


def ate(eid, off, length, part=0xff, bad_crc=False):
    head = struct.pack('<HHHB', eid, off, length, part)
    c = crc8_ccitt(0xff, head)
    return head + bytes([c ^ 0x55 if bad_crc else c])


class Nvs:
    def __init__(self):
        self.img = bytearray(b'\xff' * SS * COUNT)
        self.sec = 0
        self.ate_wra = SS - 2 * ATE
        self.data_wra = 0

    def _put(self, addr, b):
        base = self.sec * SS
        self.img[base + addr:base + addr + len(b)] = b

    def write(self, eid, data, bad_ate=False, bad_data_crc=False):
        crc = zlib.crc32(data) & 0xffffffff
        if bad_data_crc:
            crc ^= 1
        blob = data + struct.pack('<I', crc) if data else b''
        self._put(self.data_wra, blob)
        self._put(self.ate_wra, ate(eid, self.data_wra, len(blob), bad_crc=bad_ate))
        self.data_wra += len(blob)
        self.ate_wra -= ATE

    def keystore(self, name, value, **kw):
        self.write(name_id(name), name.encode() + b'\0' + value, **kw)

    def advance(self):
        """nvs_sector_close, then the next sector with its gc_done ATE."""
        self._put(SS - ATE, ate(0xffff, self.ate_wra + ATE, 0))
        self.sec = (self.sec + 1) % COUNT
        self.ate_wra = SS - 2 * ATE
        self.data_wra = 0
        self._put(self.ate_wra, ate(0xffff, self.data_wra, 0))
        self.ate_wra -= ATE


def main():
    out, scenario, keyfile = sys.argv[1:4]
    key = open(keyfile, 'rb').read()
    n = Nvs()
    other = bytes(range(64))
    if scenario == 'newest':        # three versions; the last one wins
        n.keystore('tls_key.der', other)
        n.keystore('ssh_host_key.bin', b'\x30OLD-1')
        n.keystore('ssh_host_key.bin', b'\x30OLD-2')
        n.keystore('ssh_host_key.bin', key)
    elif scenario == 'torn':        # a newer entry whose ATE is torn
        n.keystore('ssh_host_key.bin', key)
        n.keystore('ssh_host_key.bin', b'\x30TORN', bad_ate=True)
    elif scenario == 'rollover':    # old copy in a closed sector, new one after
        n.keystore('ssh_host_key.bin', b'\x30OLD')
        n.keystore('tls_key.der', other)
        n.advance()
        n.keystore('ssh_host_key.bin', key)
    elif scenario == 'wrap':        # the ring wraps: newest sector is 0 again
        for _ in range(COUNT - 1):
            n.keystore('ssh_host_key.bin', b'\x30OLD')
            n.advance()
        n.img[0:SS] = b'\xff' * SS   # sector 0 was garbage-collected
        n.sec = COUNT - 1
        n.advance()
        n.keystore('ssh_host_key.bin', key)
    elif scenario == 'deleted':     # written, then deleted (len 0)
        n.keystore('ssh_host_key.bin', key)
        n.write(name_id('ssh_host_key.bin'), b'')
    elif scenario == 'badcrc':      # the data CRC does not match
        n.keystore('ssh_host_key.bin', key, bad_data_crc=True)
    else:
        sys.exit('unknown scenario ' + scenario)
    open(out, 'wb').write(n.img)


main()
