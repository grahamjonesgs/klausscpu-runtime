/*
 * zephyr_stdio.h — declarations for the stdio file functions that Zephyr's
 * minimal libc lacks, so Doom's <stdio.h> users see correct prototypes (else
 * implicit declarations would truncate the returned FILE* on this LP64 target).
 *
 * Force-included into the Doom engine sources (see CMakeLists.txt).
 * Implementations are in zephyr_stdio.c, backed by Zephyr's fs_* API.
 */

#ifndef KLAUSSCPU_DOOM_ZEPHYR_STDIO_H_
#define KLAUSSCPU_DOOM_ZEPHYR_STDIO_H_

#include <stddef.h>
#include <stdio.h>      /* FILE (== int in minimal libc), EOF */

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#ifdef __cplusplus
extern "C" {
#endif

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
int    fseek(FILE *fp, long offset, int whence);
long   ftell(FILE *fp);
int    feof(FILE *fp);
int    fflush(FILE *fp);
int    fileno(FILE *fp);

/* Also missing from minimal libc (must be declared so pointer returns aren't
 * truncated under implicit declaration). */
int    strcasecmp(const char *a, const char *b);
char  *strdup(const char *s);
int    remove(const char *path);
int    rename(const char *from, const char *to);
int    sscanf(const char *str, const char *fmt, ...);
double strtod(const char *s, char **end);
double atof(const char *s);
int    system(const char *cmd);

/* libm subset Doom uses at renderer init (no libm in this build). */
double fabs(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);

#ifdef __cplusplus
}
#endif

#endif /* KLAUSSCPU_DOOM_ZEPHYR_STDIO_H_ */
