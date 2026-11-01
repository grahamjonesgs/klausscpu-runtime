// test_misalign.c — the misaligned-access trap of klaussCPU 3508f87
// (ISA_ENCODING_V2_MAP.md §8.5): a 16-bit load or store at an odd address, or
// a 32-bit one that is not 4-aligned, stops the core with a crash dump
// (ERR=0A, parked, no retire line); older cores aligned the address down.
// Not trapped: MEMGET32, byte accesses and 64-bit accesses (a 64-bit access
// reaches the aligned doubleword).
//
// The compiler never emits a trapping access for defined C, so each case
// builds one through a volatile pointer cast (undefined behaviour, on purpose).
//
//   CASE=0   (default) nothing may trap: byte, MEMGET32 and 64-bit accesses at
//            every misalignment; prints values and "=== ALL PASS ===".
//   CASE=1.. one trapping access, three calls deep (level3 <- level2 <-
//            level1 <- main), as the compiler emits it.  The program prints
//            the three function addresses, then traps: the crash dump has
//            ERR=0A and F1/F3 hold return addresses inside level2/level1.
//            tools/validate/misalign.sh builds each case, runs it on the
//            emulator (stop = Misaligned) and the board, and checks the dump.
//
// Build:  make test_misalign.elf [CASEFLAGS=-DCASE=3]
#include <stdint.h>
#include <stdio.h>

#ifndef CASE
#define CASE 0
#endif

#define NOINLINE __attribute__((noinline))

// The compiler splits an access whose misalignment it can see into bytes (or
// MEMGET32), so the address goes through an empty asm first: it then trusts
// the pointer type's alignment, as for any pointer it knows nothing about.
#define HIDE(p) ({ uintptr_t _a = (uintptr_t)(p); __asm__ volatile("" : "+r"(_a)); _a; })

static uint64_t buf[8] __attribute__((aligned(64))) = {
    0x1122334455667788ull, 0x99aabbccddeeff00ull, 0x0123456789abcdefull,
    0xfedcba9876543210ull,
};

#if CASE == 0

static int fails;
static void check(const char *what, int ok) {
  if (!ok) {
    printf("FAIL %s\n", what);
    fails++;
  }
}

NOINLINE static uint32_t memget32(const void *p) {
  uint64_t r;
  __asm__ volatile("memget32 %0, %1" : "=r"(r) : "r"(p) : "memory");
  return (uint32_t)r;
}

int main(void) {
  const volatile uint8_t *b = (const volatile uint8_t *)buf;
  uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned off = 0; off < 16; off++) {
    // bytes
    uint8_t v = *(const volatile uint8_t *)(b + off);
    // MEMGET32 reads the unaligned word byte by byte
    uint32_t w = memget32((const void *)(b + off));
    uint32_t ref = b[off] | b[off + 1] << 8 | b[off + 2] << 16 |
                   (uint32_t)b[off + 3] << 24;
    check("memget32", w == ref);
    // 64-bit load: the aligned doubleword
    uint64_t q = *(const volatile uint64_t *)HIDE(b + off);
    uint64_t base = *(const volatile uint64_t *)(b + (off & ~7u));
    check("ld64 aligns down", q == base);
    h = (h ^ v ^ w ^ q) * 0x100000001b3ull;
  }
  // byte stores at every offset
  for (unsigned off = 0; off < 8; off++) {
    volatile uint8_t *p = (volatile uint8_t *)buf + 32 + off;
    *p = (uint8_t)(0xA0 + off);
    check("st8", *p == (uint8_t)(0xA0 + off));
  }
  // 64-bit store at a misaligned address: the aligned doubleword
  *(volatile uint64_t *)HIDE((uint8_t *)buf + 40 + 3) = 0x0807060504030201ull;
  check("st64 aligns down", buf[5] == 0x0807060504030201ull);
  printf("hash %016llx\n", (unsigned long long)h);
  printf(fails ? "FAILED %d\n" : "=== ALL PASS ===\n", fails);
  return fails != 0;
}

#else

// level3's access, selected by CASE.
#define A(off) ((void *)HIDE((uint8_t *)buf + (off)))
NOINLINE static uint64_t level3(int unused) {
  uint64_t r = 0;
  printf("level3: access next\n");
#if CASE == 1
  r = *(volatile uint16_t *)A(1);              // LD16, odd
#elif CASE == 2
  *(volatile uint16_t *)A(3) = 0x1234;         // ST16, odd
#elif CASE == 3
  r = ((volatile uint32_t *)A(2))[unused + 1]; // LD32 (LDIDX32), +2
#elif CASE == 4
  r = ((volatile uint32_t *)A(1))[unused];     // LD32 (LDIDX32), +1
#elif CASE == 5
  *(volatile uint32_t *)A(2) = 0x12345678;     // ST32, +2
#elif CASE == 6
  *(volatile uint32_t *)A(3) = 0x12345678;     // ST32, +3
#elif CASE == 7
  r = *(volatile int16_t *)A(5);               // signed LD16, odd
#elif CASE == 8
  r = *(volatile int32_t *)A(6);               // signed LD32, +2
#elif CASE == 9
  r = ((volatile uint16_t *)A(1))[unused + 2]; // indexed LD16, odd base
#elif CASE == 10
  ((volatile uint32_t *)A(2))[unused + 1] = 7; // indexed ST32, +2 base
#else
#error unknown CASE
#endif
  return r;
}
NOINLINE static uint64_t level2(int u) { return level3(u) + 1; }
NOINLINE static uint64_t level1(int u) { return level2(u) + 2; }
int main(void) {
  printf("level1=%p level2=%p level3=%p\n", (void *)level1, (void *)level2, (void *)level3);
  printf("unexpected: %llx\n", (unsigned long long)level1(0));
  return 0;
}
#endif
