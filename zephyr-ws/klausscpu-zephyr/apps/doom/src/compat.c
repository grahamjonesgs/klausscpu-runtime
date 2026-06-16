/*
 * compat.c — small POSIX stubs the Doom engine references but minimal libc
 * doesn't provide.  Most are only on code paths we don't exercise (IWAD
 * directory search, raw-tty handling, mmap WAD loading); access()/stat() are
 * implemented via the fopen shim so file-existence/size checks work, and
 * usleep/gettimeofday map onto the Zephyr clock.
 *
 * Zephyr's <kernel.h> is included first so ssize_t/off_t come from Zephyr; the
 * compat headers re-typedef them identically (-Wno-typedef-redefinition).
 */

#include <zephyr/kernel.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>

#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "zephyr_stdio.h"   /* fopen/fseek/ftell/fclose/remove */

int isatty(int fd)
{
	ARG_UNUSED(fd);
	return 0;
}

int usleep(unsigned long usec)
{
	k_usleep((int32_t)usec);
	return 0;
}

unsigned sleep(unsigned sec)
{
	k_sleep(K_SECONDS(sec));
	return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
	ARG_UNUSED(tz);
	if (tv) {
		int64_t ms = k_uptime_get();

		tv->tv_sec = (long)(ms / 1000);
		tv->tv_usec = (long)((ms % 1000) * 1000);
	}
	return 0;
}

int access(const char *path, int mode)
{
	ARG_UNUSED(mode);
	FILE *f = fopen(path, "rb");

	if (!f) {
		return -1;
	}
	fclose(f);
	return 0;
}

int stat(const char *path, struct stat *st)
{
	FILE *f = fopen(path, "rb");

	if (!f) {
		return -1;
	}
	fseek(f, 0, SEEK_END);
	if (st) {
		st->st_size = ftell(f);
		st->st_mode = S_IFREG;
	}
	fclose(f);
	return 0;
}

int fstat(int fd, struct stat *st)
{
	ARG_UNUSED(fd);
	ARG_UNUSED(st);
	return -1;
}

int unlink(const char *path)
{
	return remove(path);
}

int mkdir(const char *path, mode_t mode)
{
	ARG_UNUSED(path);
	ARG_UNUSED(mode);
	return 0;   /* pretend success (savegame/config dir) */
}

int getpid(void)
{
	return 1;
}

char *getcwd(char *buf, size_t size)
{
	if (buf && size >= 2) {
		buf[0] = '/';
		buf[1] = '\0';
		return buf;
	}
	return NULL;
}

DIR *opendir(const char *path)
{
	ARG_UNUSED(path);
	return NULL;   /* IWAD search only — bypassed via explicit -iwad */
}

struct dirent *readdir(DIR *d)
{
	ARG_UNUSED(d);
	return NULL;
}

int closedir(DIR *d)
{
	ARG_UNUSED(d);
	return 0;
}

int ioctl(int fd, unsigned long request, ...)
{
	ARG_UNUSED(fd);
	ARG_UNUSED(request);
	return -1;
}

int tcgetattr(int fd, struct termios *t)
{
	ARG_UNUSED(fd);
	ARG_UNUSED(t);
	return 0;
}

int tcsetattr(int fd, int actions, const struct termios *t)
{
	ARG_UNUSED(fd);
	ARG_UNUSED(actions);
	ARG_UNUSED(t);
	return 0;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(len);
	ARG_UNUSED(prot);
	ARG_UNUSED(flags);
	ARG_UNUSED(fd);
	ARG_UNUSED(off);
	return MAP_FAILED;
}

int munmap(void *addr, size_t len)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(len);
	return 0;
}

int system(const char *cmd)
{
	ARG_UNUSED(cmd);
	return -1;
}

/* ── libm subset (Doom calls these once, at renderer init) ───────────────── */

#define DG_PI      3.14159265358979323846
#define DG_HALF_PI 1.57079632679489661923
#define DG_TWO_PI  6.28318530717958647692

double fabs(double x)
{
	return __builtin_fabs(x);
}

double sin(double x)
{
	/* Range-reduce to [-pi, pi], then a 5-term Taylor series (accurate to
	 * ~1e-6 over the range — ample for building Doom's projection LUTs). */
	while (x > DG_PI) {
		x -= DG_TWO_PI;
	}
	while (x < -DG_PI) {
		x += DG_TWO_PI;
	}
	double x2 = x * x;

	return x * (1.0 - x2 / 6.0 *
		    (1.0 - x2 / 20.0 *
		     (1.0 - x2 / 42.0 *
		      (1.0 - x2 / 72.0))));
}

double cos(double x)
{
	return sin(x + DG_HALF_PI);
}

double tan(double x)
{
	return sin(x) / cos(x);
}

double atan(double x)
{
	int neg = 0, inv = 0;

	if (x < 0) {
		neg = 1;
		x = -x;
	}
	if (x > 1.0) {
		inv = 1;
		x = 1.0 / x;
	}
	double x2 = x * x;
	double r = x * (0.9998660 + x2 * (-0.3302995 + x2 * (0.1801410 +
		   x2 * (-0.0851330 + x2 * 0.0208351))));

	if (inv) {
		r = DG_HALF_PI - r;
	}
	return neg ? -r : r;
}

/* ── number parsing minimal libc lacks ──────────────────────────────────── */

double strtod(const char *s, char **end)
{
	while (isspace((unsigned char)*s)) {
		s++;
	}
	int sign = 1;

	if (*s == '+' || *s == '-') {
		sign = (*s == '-') ? -1 : 1;
		s++;
	}
	double val = 0.0;

	while (isdigit((unsigned char)*s)) {
		val = val * 10.0 + (*s - '0');
		s++;
	}
	if (*s == '.') {
		s++;
		double f = 0.1;

		while (isdigit((unsigned char)*s)) {
			val += (*s - '0') * f;
			f *= 0.1;
			s++;
		}
	}
	if (*s == 'e' || *s == 'E') {
		s++;
		int es = 1;

		if (*s == '+' || *s == '-') {
			es = (*s == '-') ? -1 : 1;
			s++;
		}
		int e = 0;

		while (isdigit((unsigned char)*s)) {
			e = e * 10 + (*s - '0');
			s++;
		}
		double p = 1.0;

		for (int i = 0; i < e; i++) {
			p *= 10.0;
		}
		val = (es < 0) ? val / p : val * p;
	}
	if (end) {
		*end = (char *)s;
	}
	return sign * val;
}

double atof(const char *s)
{
	return strtod(s, NULL);
}

/* Minimal sscanf — covers the integer/float/string conversions Doom uses
 * (%d %i %u %x %X %o %f %s %c and literal/whitespace matching). */
int sscanf(const char *str, const char *fmt, ...)
{
	va_list ap;
	const char *s = str;
	int count = 0;

	va_start(ap, fmt);
	for (const char *f = fmt; *f; f++) {
		if (isspace((unsigned char)*f)) {
			while (isspace((unsigned char)*s)) {
				s++;
			}
			continue;
		}
		if (*f != '%') {
			if (*s == *f) {
				s++;
				continue;
			}
			break;
		}

		f++;
		int width = 0;

		while (isdigit((unsigned char)*f)) {
			width = width * 10 + (*f - '0');
			f++;
		}
		while (*f == 'l' || *f == 'h' || *f == 'z') {
			f++;
		}
		char conv = *f;
		char *end;

		if (conv == 'c') {
			char *out = va_arg(ap, char *);

			if (!*s) {
				break;
			}
			*out = *s++;
			count++;
			continue;
		}

		while (isspace((unsigned char)*s)) {
			s++;
		}

		if (conv == 'd' || conv == 'i' || conv == 'u' ||
		    conv == 'x' || conv == 'X' || conv == 'o') {
			int base = (conv == 'x' || conv == 'X') ? 16 :
				   (conv == 'o') ? 8 :
				   (conv == 'i') ? 0 : 10;
			long v = strtol(s, &end, base);

			if (end == s) {
				break;
			}
			*va_arg(ap, int *) = (int)v;
			s = end;
			count++;
		} else if (conv == 'f' || conv == 'g' || conv == 'e') {
			double v = strtod(s, &end);

			if (end == s) {
				break;
			}
			*va_arg(ap, float *) = (float)v;
			s = end;
			count++;
		} else if (conv == 's') {
			char *out = va_arg(ap, char *);
			int n = 0;

			while (*s && !isspace((unsigned char)*s) &&
			       (width == 0 || n < width)) {
				*out++ = *s++;
				n++;
			}
			*out = '\0';
			if (n == 0) {
				break;
			}
			count++;
		} else if (conv == '%') {
			if (*s == '%') {
				s++;
			} else {
				break;
			}
		} else {
			break;
		}
	}
	va_end(ap);
	return count;
}
