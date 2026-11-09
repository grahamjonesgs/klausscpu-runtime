/* __builtin_* the kernel uses */
#include <stdint.h>
int a(unsigned long x) { return __builtin_ctzl(x) + __builtin_clzl(x) + __builtin_popcountl(x) + __builtin_ffsl(x); }
int b(unsigned x) { return __builtin_ctz(x) + __builtin_clz(x) + __builtin_popcount(x) + __builtin_parity(x); }
uint64_t c(uint64_t x) { return __builtin_bswap64(x) ^ __builtin_bswap32((uint32_t)x) ^ __builtin_bswap16((uint16_t)x); }
int d(long x, long y, long *r) { return __builtin_add_overflow(x, y, r) | __builtin_mul_overflow(x, y, r) | __builtin_sub_overflow(x, y, r); }
void *e(void) { return __builtin_return_address(0); }
void *f(void) { return __builtin_frame_address(0); }
int g(int x) { if (__builtin_expect(x, 0)) __builtin_trap(); return __builtin_constant_p(x); }
void h(void *p) { __builtin_prefetch(p); }
long i(long x) { return __builtin_choose_expr(1, x, 0) + __builtin_types_compatible_p(int, int); }
void j(void) { __builtin_unreachable(); }
unsigned long k(unsigned long *p) { return __atomic_load_n(p, __ATOMIC_RELAXED) + __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST); }
int l(int *p, int o, int n) { return __atomic_compare_exchange_n(p, &o, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
void m(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); __sync_synchronize(); }
__int128 n(__int128 a, __int128 b) { return a * b; }
unsigned long o(unsigned long a, unsigned long b) { return a / b + a % b; }
long p(long a) { return __builtin_object_size(&a, 0); }
void q(void *d, const void *s, unsigned long n) { __builtin_memcpy(d, s, n); __builtin_memset(d, 0, n); __builtin_memmove(d, s, n); }
