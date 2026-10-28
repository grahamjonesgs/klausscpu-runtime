// test_unaligned.c — unaligned loads: MEMGET32 and the code LLVM builds on it.
//
// MEMGET32 ([r], 32-bit, unsigned) is the CPU's only load that reads an
// unaligned word: within a doubleword, across two doublewords of one 16-byte
// cache line (the read lookahead) and across two lines (a second read).
// Every other 16/32-bit load, and every 64-bit one, aligns its address down,
// and every sub-word store does too (CPU_ARCHITECTURE.md 8.3).  So LLVM
// compiles an under-aligned 32-bit load (packed struct field, memcpy(&w, p,
// 4), bytes ORed into a word) to MEMGET32 of the computed address, and an
// under-aligned 64-bit load to two of them; stores stay byte stores.
//
// This checks, at every offset 0..255 of a 64-byte-aligned buffer and again
// after evicting the D-cache, each load shape against a byte-by-byte
// reference read through a volatile pointer:
//   - MEMGET32 itself (inline asm), and MEMGET32 right after a byte store or
//     an aligned word store to the doubleword or line it spans into;
//   - packed-struct u32 / s32 / u64 / u16 fields, memcpy u32 / u64, 64-bit
//     values at 2- and 4-byte alignment;
//   - 4 and 8 bytes assembled little- and big-endian (network order);
//   - a packed local and a packed global;
//   - an Ethernet/IPv4/UDP header parsed at every offset of a frame buffer
//     (the lwIP shape), checksum included;
//   - a packed source copied into an aligned struct.
// Each group prints a hash of what it read, so a wrong value shows as a
// changed line, and the program ends with "=== ALL PASS ===" or a count.
//
// Build:  make test_unaligned.elf

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NOINLINE __attribute__((noinline))

static int fails;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (!(c)) {                                                                \
      if (fails < 20) {                                                        \
        printf("  FAIL line %d: ", __LINE__);                                 \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
      }                                                                        \
      fails++;                                                                 \
    }                                                                          \
  } while (0)

static uint8_t buf[512] __attribute__((aligned(64)));
static uint8_t evict_buf[64 * 1024];

static void evict(void) {
  volatile uint8_t *b = evict_buf;
  for (unsigned i = 0; i < sizeof evict_buf; i += 16)
    b[i] = (uint8_t)i;
}

static void fill(uint8_t seed) {
  for (unsigned i = 0; i < sizeof buf; i++)
    buf[i] = (uint8_t)(i * 37u + 11u + seed * 101u);
}

// Byte-by-byte references through a volatile pointer: nothing to combine.
static uint64_t ref_le(const uint8_t *p, int n) {
  const volatile uint8_t *v = p;
  uint64_t r = 0;
  for (int i = n - 1; i >= 0; i--)
    r = r << 8 | v[i];
  return r;
}
static uint64_t ref_be(const uint8_t *p, int n) {
  const volatile uint8_t *v = p;
  uint64_t r = 0;
  for (int i = 0; i < n; i++)
    r = r << 8 | v[i];
  return r;
}

static uint64_t hash;
static void mix(uint64_t v) { hash = (hash ^ v) * 0x100000001b3ull; }

// ---- load shapes -----------------------------------------------------------

NOINLINE static uint32_t asm_memget32(const void *p) {
  uint64_t r;
  __asm__ volatile("memget32 %0, %1" : "=r"(r) : "r"(p) : "memory");
  return (uint32_t)r;
}

// A byte store into the doubleword (and maybe line) the load spans into, then
// MEMGET32 straight after it.
NOINLINE static uint32_t asm_store8_memget32(uint8_t *s, uint64_t v,
                                             const void *p) {
  uint64_t r;
  __asm__ volatile("memset8 %1, %2\n\tmemget32 %0, %3"
                   : "=&r"(r)
                   : "r"(v), "r"(s), "r"(p)
                   : "memory");
  return (uint32_t)r;
}

// The same with an aligned 32-bit store.
NOINLINE static uint32_t asm_store32_memget32(uint8_t *s, uint64_t v,
                                              const void *p) {
  uint64_t r;
  __asm__ volatile("memset32 %1, %2\n\tmemget32 %0, %3"
                   : "=&r"(r)
                   : "r"(v), "r"(s), "r"(p)
                   : "memory");
  return (uint32_t)r;
}

struct __attribute__((packed)) P {
  uint8_t pad;
  uint32_t u32;
  int32_t s32;
  uint64_t u64;
  uint16_t u16;
};

NOINLINE static uint32_t ld_u32(const struct P *p) { return p->u32; }
NOINLINE static int64_t ld_s32(const struct P *p) { return p->s32; }
NOINLINE static uint64_t ld_u64(const struct P *p) { return p->u64; }
NOINLINE static uint16_t ld_u16(const struct P *p) { return p->u16; }
NOINLINE static uint32_t ld_u32_idx(const struct P *p, long i) {
  return p[i].u32;
}

NOINLINE static uint32_t mc32(const uint8_t *p) {
  uint32_t w;
  memcpy(&w, p, 4);
  return w;
}
NOINLINE static uint64_t mc64(const uint8_t *p) {
  uint64_t w;
  memcpy(&w, p, 8);
  return w;
}
NOINLINE static uint64_t mc64_idx(const uint8_t *p, long i) {
  uint64_t w;
  memcpy(&w, p + i, 8);
  return w;
}

typedef uint64_t u64_a2 __attribute__((aligned(2)));
typedef uint64_t u64_a4 __attribute__((aligned(4)));
NOINLINE static uint64_t ld64_a2(const u64_a2 *p) { return *p; }
NOINLINE static uint64_t ld64_a4(const u64_a4 *p) { return *p; }

NOINLINE static uint32_t le32(const uint8_t *p) {
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
NOINLINE static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
NOINLINE static uint64_t le64(const uint8_t *p) {
  return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 |
         (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
         (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}
NOINLINE static uint64_t be64(const uint8_t *p) {
  return (uint64_t)p[7] | (uint64_t)p[6] << 8 | (uint64_t)p[5] << 16 |
         (uint64_t)p[4] << 24 | (uint64_t)p[3] << 32 | (uint64_t)p[2] << 40 |
         (uint64_t)p[1] << 48 | (uint64_t)p[0] << 56;
}

// A packed source copied into an aligned struct (store merging / memcpy).
struct A {
  uint64_t u64;
  uint32_t u32;
  uint32_t s32;
};
NOINLINE static void copy_packed(struct A *d, const struct P *s) {
  d->u64 = s->u64;
  d->u32 = s->u32;
  d->s32 = (uint32_t)s->s32;
}

// ---- checks ----------------------------------------------------------------

static void check_offsets(int do_evict) {
  hash = 0xcbf29ce484222325ull;
  for (unsigned off = 0; off < 256; off++) {
    if (do_evict)
      evict();
    uint8_t *p = buf + off;
    uint32_t r32 = (uint32_t)ref_le(p, 4);
    uint64_t r64 = ref_le(p, 8);

    uint32_t a = asm_memget32(p);
    CHECK(a == r32, "memget32 off %u: %08x want %08x", off, a, r32);
    uint32_t m = mc32(p);
    CHECK(m == r32, "memcpy32 off %u: %08x want %08x", off, m, r32);
    uint64_t m8 = mc64(p);
    CHECK(m8 == r64, "memcpy64 off %u: %016lx want %016lx", off,
          (unsigned long)m8, (unsigned long)r64);
    uint64_t m8i = mc64_idx(buf, off);
    CHECK(m8i == r64, "memcpy64 idx off %u", off);
    uint32_t l = le32(p);
    CHECK(l == r32, "le32 off %u: %08x want %08x", off, l, r32);
    uint32_t b = be32(p), rb = (uint32_t)ref_be(p, 4);
    CHECK(b == rb, "be32 off %u: %08x want %08x", off, b, rb);
    uint64_t l8 = le64(p);
    CHECK(l8 == r64, "le64 off %u", off);
    uint64_t b8 = be64(p), rb8 = ref_be(p, 8);
    CHECK(b8 == rb8, "be64 off %u", off);

    // Each struct starts where its field lands on p.
    if (off >= 1) {
      uint32_t f = ld_u32((const struct P *)(p - 1));
      CHECK(f == r32, "packed u32 off %u: %08x want %08x", off, f, r32);
      uint32_t fi = ld_u32_idx((const struct P *)(p - 1), 0);
      CHECK(fi == r32, "packed u32 idx off %u", off);
    }
    if (off >= 5) {
      int64_t fs = ld_s32((const struct P *)(p - 5));
      CHECK(fs == (int64_t)(int32_t)r32, "packed s32 off %u", off);
    }
    if (off >= 9) {
      uint64_t f8 = ld_u64((const struct P *)(p - 9));
      CHECK(f8 == r64, "packed u64 off %u", off);
    }
    if (off >= 17) {
      uint16_t h = ld_u16((const struct P *)(p - 17));
      CHECK(h == (uint16_t)r32, "packed u16 off %u", off);
    }
    if (!(off & 1)) {
      uint64_t a2 = ld64_a2((const u64_a2 *)p);
      CHECK(a2 == r64, "u64 align2 off %u", off);
    }
    if (!(off & 3)) {
      uint64_t a4 = ld64_a4((const u64_a4 *)p);
      CHECK(a4 == r64, "u64 align4 off %u", off);
    }
    mix(a);
    mix(m8);
    mix(b8);
  }
  printf("offsets%s: hash %016lx\n", do_evict ? " (evicted)" : "",
         (unsigned long)hash);
}

// MEMGET32 straight after a store into what it spans: every offset whose word
// crosses a doubleword (off & 7 in 5..7), the store hitting the second
// doubleword (byte store at each of the bytes read there, or the aligned word
// store covering them), with and without eviction.
static void check_store_then_load(void) {
  hash = 0xcbf29ce484222325ull;
  for (int do_evict = 0; do_evict < 2; do_evict++)
    for (unsigned off = 0; off < 256; off++) {
      if ((off & 7) < 5)
        continue;
      unsigned next = (off + 8) & ~7u; // the doubleword it spans into
      for (unsigned k = next; k < off + 4; k++) {
        fill((uint8_t)(off + k));
        if (do_evict)
          evict();
        uint8_t v = (uint8_t)(k * 13u + off);
        uint32_t got = asm_store8_memget32(buf + k, v, buf + off);
        uint32_t want = (uint32_t)ref_le(buf + off, 4);
        CHECK(got == want, "store8@%u + memget32 off %u%s: %08x want %08x", k,
              off, do_evict ? " evicted" : "", got, want);
        mix(got);
      }
      fill((uint8_t)off);
      if (do_evict)
        evict();
      uint32_t w = 0xA5000000u | off;
      uint32_t got = asm_store32_memget32(buf + next, w, buf + off);
      uint32_t want = (uint32_t)ref_le(buf + off, 4);
      CHECK(got == want, "store32@%u + memget32 off %u%s: %08x want %08x",
            next, off, do_evict ? " evicted" : "", got, want);
      mix(got);
    }
  fill(0);
  printf("store then memget32: hash %016lx\n", (unsigned long)hash);
}

static struct P g_packed[3];

static void check_frame_global(void) {
  struct P l[3];
  hash = 0xcbf29ce484222325ull;
  for (int i = 0; i < 3; i++) {
    l[i].pad = (uint8_t)i;
    l[i].u32 = 0x11223344u * (unsigned)(i + 1);
    l[i].s32 = -0x01020304 * (i + 1);
    l[i].u64 = 0x0102030405060708ull * (unsigned)(i + 3);
    l[i].u16 = (uint16_t)(0xBEEF + i);
    g_packed[i] = l[i];
  }
  for (int i = 0; i < 3; i++) {
    const uint8_t *lb = (const uint8_t *)&l[i];
    CHECK(ld_u32(&l[i]) == (uint32_t)ref_le(lb + 1, 4), "frame u32 %d", i);
    CHECK(ld_s32(&l[i]) == (int32_t)ref_le(lb + 5, 4), "frame s32 %d", i);
    CHECK(ld_u64(&l[i]) == ref_le(lb + 9, 8), "frame u64 %d", i);
    CHECK(g_packed[i].u32 == l[i].u32, "global u32 %d", i);
    CHECK(g_packed[i].s32 == l[i].s32, "global s32 %d", i);
    CHECK(g_packed[i].u64 == l[i].u64, "global u64 %d", i);
    CHECK(g_packed[i].u16 == l[i].u16, "global u16 %d", i);
    struct A d;
    copy_packed(&d, &g_packed[i]);
    CHECK(d.u64 == l[i].u64 && d.u32 == l[i].u32 &&
              d.s32 == (uint32_t)l[i].s32,
          "copy packed %d", i);
    mix(l[i].u64 + l[i].u32);
  }
  printf("packed frame/global/copy: hash %016lx\n", (unsigned long)hash);
}

// ---- packet parsing (lwIP's shape: packed headers, network byte order) -----

struct __attribute__((packed)) eth_hdr {
  uint8_t dst[6], src[6];
  uint16_t type;
};
struct __attribute__((packed)) ip_hdr {
  uint8_t vhl, tos;
  uint16_t len, id, off;
  uint8_t ttl, proto;
  uint16_t chksum;
  uint32_t src, dst;
};
struct __attribute__((packed)) udp_hdr {
  uint16_t src, dst, len, chksum;
};

static uint16_t bswap16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }

NOINLINE static uint16_t ip_chksum(const void *hdr, int len) {
  const uint8_t *p = hdr;
  uint32_t sum = 0;
  for (int i = 0; i < len; i += 2) {
    uint16_t w;
    memcpy(&w, p + i, 2);
    sum += bswap16(w);
  }
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return (uint16_t)~sum;
}

NOINLINE static uint64_t parse(const uint8_t *frame) {
  const struct eth_hdr *e = (const void *)frame;
  const struct ip_hdr *ip = (const void *)(frame + sizeof *e);
  const struct udp_hdr *u = (const void *)(frame + sizeof *e + sizeof *ip);
  if (bswap16(e->type) != 0x0800 || (ip->vhl >> 4) != 4)
    return 1;
  if (ip_chksum(ip, sizeof *ip) != 0)
    return 2;
  uint32_t src = __builtin_bswap32(ip->src), dst = __builtin_bswap32(ip->dst);
  return (uint64_t)src << 32 ^ dst ^ (uint64_t)bswap16(u->src) << 16 ^
         bswap16(u->dst) ^ (uint64_t)bswap16(u->len) << 48 ^
         (uint64_t)bswap16(ip->len) << 40;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) {
  put16(p, v >> 16);
  put16(p + 2, (uint16_t)v);
}

static void check_packets(void) {
  hash = 0xcbf29ce484222325ull;
  for (unsigned off = 0; off < 64; off++) {
    memset(buf, 0, sizeof buf);
    uint8_t *f = buf + off;
    for (int i = 0; i < 12; i++)
      f[i] = (uint8_t)(0x10 + i + off);
    put16(f + 12, 0x0800);
    uint8_t *ip = f + 14;
    ip[0] = 0x45;
    put16(ip + 2, (uint16_t)(28 + off));
    put16(ip + 4, (uint16_t)(0x1234 + off));
    ip[8] = 64;
    ip[9] = 17;
    put32(ip + 12, 0xC0A80100u + off);
    put32(ip + 16, 0x0A000001u + off * 3);
    put16(ip + 10, 0);
    put16(ip + 10, ip_chksum(ip, 20));
    uint8_t *udp = ip + 20;
    put16(udp, (uint16_t)(5000 + off));
    put16(udp + 2, 53);
    put16(udp + 4, 8);

    uint64_t want = (uint64_t)(0xC0A80100u + off) << 32 ^
                    (0x0A000001u + off * 3) ^ (uint64_t)(5000 + off) << 16 ^
                    53 ^ (uint64_t)8 << 48 ^ (uint64_t)(28 + off) << 40;
    uint64_t got = parse(f);
    CHECK(got == want, "packet off %u: %016lx want %016lx", off,
          (unsigned long)got, (unsigned long)want);
    mix(got);
  }
  printf("packets: hash %016lx\n", (unsigned long)hash);
}

int main(void) {
  printf("=== test_unaligned ===\n");
  fill(0);
  check_offsets(0);
  check_offsets(1);
  check_store_then_load();
  check_frame_global();
  check_packets();
  if (fails == 0)
    printf("=== ALL PASS ===\n");
  else
    printf("=== %d FAIL ===\n", fails);
  return 0;
}
