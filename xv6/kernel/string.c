#include "types.h"

// klausscpu: 8 bytes at a time where it can (pages are 64 KiB).
void *
memset(void *dst, int c, uint n)
{
  char *cdst = (char *)dst;
  uint i = 0;
  if ((((uint64)dst) & 7) == 0) {
    uint64 v = (uchar)c;
    v |= v << 8;
    v |= v << 16;
    v |= v << 32;
    for (; i + 8 <= n; i += 8)
      *(uint64 *)(cdst + i) = v;
  }
  for (; i < n; i++) {
    cdst[i] = c;
  }
  return dst;
}

int
memcmp(const void *v1, const void *v2, uint n)
{
  const uchar *s1, *s2;

  s1 = v1;
  s2 = v2;
  while (n-- > 0) {
    if (*s1 != *s2)
      return *s1 - *s2;
    s1++, s2++;
  }

  return 0;
}

void *
memmove(void *dst, const void *src, uint n)
{
  const char *s;
  char *d;

  if (n == 0)
    return dst;

  s = src;
  d = dst;
  // klausscpu: 8 bytes at a time when both are aligned and don't overlap.
  if (((((uint64)s) | ((uint64)d)) & 7) == 0 && (s + n <= d || d + n <= s)) {
    uint i = 0;
    for (; i + 8 <= n; i += 8)
      *(uint64 *)(d + i) = *(const uint64 *)(s + i);
    for (; i < n; i++)
      d[i] = s[i];
    return dst;
  }
  if (s < d && s + n > d) {
    s += n;
    d += n;
    while (n-- > 0)
      *--d = *--s;
  } else
    while (n-- > 0)
      *d++ = *s++;

  return dst;
}

// memcpy exists to placate GCC.  Use memmove.
void *
memcpy(void *dst, const void *src, uint n)
{
  return memmove(dst, src, n);
}

int
strncmp(const char *p, const char *q, uint n)
{
  while (n > 0 && *p && *p == *q)
    n--, p++, q++;
  if (n == 0)
    return 0;
  return (uchar)*p - (uchar)*q;
}

char *
strncpy(char *s, const char *t, int n)
{
  char *os;

  os = s;
  while (n-- > 0 && (*s++ = *t++) != 0)
    ;
  while (n-- > 0)
    *s++ = 0;
  return os;
}

// Like strncpy but guaranteed to NUL-terminate.
char *
safestrcpy(char *s, const char *t, int n)
{
  char *os;

  os = s;
  if (n <= 0)
    return os;
  while (--n > 0 && (*s++ = *t++) != 0)
    ;
  *s = 0;
  return os;
}

int
strlen(const char *s)
{
  int n;

  for (n = 0; s[n]; n++)
    ;
  return n;
}
