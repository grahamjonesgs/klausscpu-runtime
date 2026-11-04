/* math.h — Zephyr minimal libc's <math.h> plus the C99 declarations it lacks.
 *
 * Minimal libc declares only sqrt/sqrtf and the M_* constants.  This adds the
 * double-precision functions, HUGE_VAL/INFINITY/NAN and the classification
 * macros (via compiler builtins, so they need no library code).  The
 * functions are declared only; an application that calls them links an
 * implementation (MicroPython builds lib/libm_dbl).
 */
#ifndef KLAUSSCPU_LIBC_EXTRAS_MATH_H
#define KLAUSSCPU_LIBC_EXTRAS_MATH_H

#include_next <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HUGE_VAL
#define HUGE_VAL  (__builtin_huge_val())
#endif
#ifndef HUGE_VALF
#define HUGE_VALF (__builtin_huge_valf())
#endif
#ifndef INFINITY
#define INFINITY  (__builtin_inff())
#endif
#ifndef NAN
#define NAN       (__builtin_nanf(""))
#endif

#ifndef FP_NAN
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#endif

#ifndef fpclassify
#define fpclassify(x) \
	__builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#endif
#ifndef isnan
#define isnan(x)    __builtin_isnan(x)
#endif
#ifndef isinf
#define isinf(x)    __builtin_isinf(x)
#endif
#ifndef isfinite
#define isfinite(x) __builtin_isfinite(x)
#endif
#ifndef isnormal
#define isnormal(x) __builtin_isnormal(x)
#endif
#ifndef signbit
#define signbit(x)  __builtin_signbit(x)
#endif

double acos(double x);
double asin(double x);
double atan(double x);
double atan2(double y, double x);
double cos(double x);
double sin(double x);
double tan(double x);
double acosh(double x);
double asinh(double x);
double atanh(double x);
double cosh(double x);
double sinh(double x);
double tanh(double x);
double exp(double x);
double expm1(double x);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double scalbn(double x, int n);
double log(double x);
double log10(double x);
double log1p(double x);
double log2(double x);
double modf(double x, double *iptr);
double pow(double x, double y);
double fabs(double x);
double ceil(double x);
double floor(double x);
double trunc(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);
double fmod(double x, double y);
double copysign(double x, double y);
double erf(double x);
double erfc(double x);
double erfc(double x);
double lgamma(double x);
double tgamma(double x);

/* Library-free ones: the compiler builtins. */
static inline double nan(const char *tag)
{
	(void)tag;
	return __builtin_nan("");
}

#ifdef __cplusplus
}
#endif

#endif /* KLAUSSCPU_LIBC_EXTRAS_MATH_H */
