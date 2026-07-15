// repro.c — minimal reproducer for the -O2 crypto (SHA-256) hang.
// Mirrors sha256()'s padding structure: the byte-copy loop followed by
// pad[len]=0x80 and the padlen computation keep `len` live past the loop,
// which (hypothesis) makes -O2 emit an UNSIGNED trip-count guard (JMPULT) that
// misfires for len==0 -> the copy loop becomes an unguarded do-while(--n) ->
// ~2^64 iterations.  sha_pad(_, 0) must terminate.
#include <stdint.h>

static uint8_t pad[256];

__attribute__((noinline))
void sha_pad(const uint8_t *msg, int len) {
    for (int i = 0; i < len; i++) pad[i] = msg[i];        // the hanging copy loop
    pad[len] = 0x80;                                       // keeps len live
    int padlen = ((len + 9 + 63) / 64) * 64;
    for (int i = len + 1; i < padlen - 8; i++) pad[i] = 0; // second (fill) loop
    uint64_t bitlen = (uint64_t)len * 8;
    pad[padlen - 8] = (uint8_t)(bitlen >> 56);
    pad[padlen - 1] = (uint8_t)(bitlen);
}

int main(void) {
    static uint8_t m[8];
    sha_pad(m, 0);        // len 0 — must not hang
    return (int)pad[0];
}
