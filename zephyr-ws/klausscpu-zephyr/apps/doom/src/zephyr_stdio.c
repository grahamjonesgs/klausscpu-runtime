/*
 * zephyr_stdio.c — fs_*-backed implementations of the stdio file functions
 * Doom needs (fopen/fread/fseek/ftell/fclose/feof), since Zephyr's minimal libc
 * provides only console output (fwrite/fputs/fprintf to stdout/stderr).
 *
 * FILE is `typedef int FILE` in minimal libc, so a FILE* is just an opaque
 * handle.  We return a pointer into file_ids[] (distinct from the (FILE*)1/2/3
 * used for stdin/stdout/stderr); the pointed-to int is the slot index.
 *
 * Doom's WAD reading goes fopen->fread/fseek through here onto /SD:.  POSIX
 * directory/stat calls used only by the IWAD *search* are stubbed to fail —
 * we pass an explicit -iwad path, so the search isn't exercised.
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "zephyr_stdio.h"

#define MAX_FILES 8

static struct {
	struct fs_file_t f;
	bool used;
	bool eof;
} files[MAX_FILES];

/* Returned to callers as FILE*; the value at each slot is its own index. */
static int  file_ids[MAX_FILES];
static bool ids_init;
static K_MUTEX_DEFINE(files_mutex);

static int slot_of(FILE *fp)
{
	int *p = (int *)fp;

	if (p < &file_ids[0] || p > &file_ids[MAX_FILES - 1]) {
		return -1;   /* not one of ours (e.g. stdin/stdout/stderr) */
	}
	return (int)(p - file_ids);
}

FILE *fopen(const char *path, const char *mode)
{
	bool want_read = false, want_write = false, append = false;

	for (const char *m = mode; *m; m++) {
		if (*m == 'r') {
			want_read = true;
		} else if (*m == 'w') {
			want_write = true;
		} else if (*m == 'a') {
			want_write = true;
			append = true;
		} else if (*m == '+') {
			want_read = true;
			want_write = true;
		}
	}

	fs_mode_t flags;

	if (want_write && want_read) {
		flags = FS_O_RDWR | FS_O_CREATE;
	} else if (want_write) {
		flags = FS_O_WRITE | FS_O_CREATE;
	} else {
		flags = FS_O_READ;
	}
	if (append) {
		flags |= FS_O_APPEND;
	}

	k_mutex_lock(&files_mutex, K_FOREVER);

	if (!ids_init) {
		for (int i = 0; i < MAX_FILES; i++) {
			file_ids[i] = i;
		}
		ids_init = true;
	}

	int slot = -1;

	for (int i = 0; i < MAX_FILES; i++) {
		if (!files[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		k_mutex_unlock(&files_mutex);
		return NULL;
	}

	fs_file_t_init(&files[slot].f);
	if (fs_open(&files[slot].f, path, flags) < 0) {
		k_mutex_unlock(&files_mutex);
		return NULL;
	}
	if (want_write && !append) {
		(void)fs_truncate(&files[slot].f, 0);
	}
	files[slot].used = true;
	files[slot].eof = false;

	k_mutex_unlock(&files_mutex);
	return (FILE *)&file_ids[slot];
}

int fclose(FILE *fp)
{
	int slot = slot_of(fp);

	if (slot < 0 || !files[slot].used) {
		return EOF;
	}
	fs_close(&files[slot].f);
	files[slot].used = false;
	return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp)
{
	int slot = slot_of(fp);

	if (slot < 0 || size == 0) {
		return 0;
	}

	size_t total = size * nmemb;
	ssize_t r = fs_read(&files[slot].f, ptr, total);

	if (r <= 0) {
		files[slot].eof = true;
		return 0;
	}
	if ((size_t)r < total) {
		files[slot].eof = true;
	}
	return (size_t)r / size;
}

int fseek(FILE *fp, long offset, int whence)
{
	int slot = slot_of(fp);

	if (slot < 0) {
		return -1;
	}

	int w = (whence == SEEK_CUR) ? FS_SEEK_CUR :
		(whence == SEEK_END) ? FS_SEEK_END : FS_SEEK_SET;

	files[slot].eof = false;
	return (fs_seek(&files[slot].f, offset, w) < 0) ? -1 : 0;
}

long ftell(FILE *fp)
{
	int slot = slot_of(fp);

	if (slot < 0) {
		return -1;
	}
	return (long)fs_tell(&files[slot].f);
}

int feof(FILE *fp)
{
	int slot = slot_of(fp);

	return (slot < 0) ? 1 : files[slot].eof;
}

int fflush(FILE *fp)
{
	ARG_UNUSED(fp);
	return 0;
}

int fileno(FILE *fp)
{
	/* stdin/stdout/stderr are (FILE*)1/2/3 in minimal libc -> fds 0/1/2. */
	if (fp == stdin)  { return 0; }
	if (fp == stdout) { return 1; }
	if (fp == stderr) { return 2; }
	int slot = slot_of(fp);

	return (slot < 0) ? -1 : slot + 3;
}

/* ── POSIX fd I/O on the same file table (fd == slot + 3) ────────────────── */

/* Local copies of the compat <fcntl.h> flag values (this TU uses Zephyr
 * headers, not the compat ones). */
#define DG_O_WRONLY 1
#define DG_O_RDWR   2
#define DG_O_CREAT  0x40
#define DG_O_TRUNC  0x200
#define DG_O_APPEND 0x400

int open(const char *path, int flags, ...)
{
	int acc = flags & 3;
	fs_mode_t f;

	if (acc == DG_O_WRONLY) {
		f = FS_O_WRITE | FS_O_CREATE;
	} else if (acc == DG_O_RDWR) {
		f = FS_O_RDWR | FS_O_CREATE;
	} else {
		f = FS_O_READ;
	}
	if (flags & DG_O_APPEND) {
		f |= FS_O_APPEND;
	}
	(void)DG_O_CREAT;

	k_mutex_lock(&files_mutex, K_FOREVER);
	if (!ids_init) {
		for (int i = 0; i < MAX_FILES; i++) {
			file_ids[i] = i;
		}
		ids_init = true;
	}
	int slot = -1;

	for (int i = 0; i < MAX_FILES; i++) {
		if (!files[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		k_mutex_unlock(&files_mutex);
		return -1;
	}
	fs_file_t_init(&files[slot].f);
	if (fs_open(&files[slot].f, path, f) < 0) {
		k_mutex_unlock(&files_mutex);
		return -1;
	}
	if ((flags & DG_O_TRUNC) && acc != 0) {
		(void)fs_truncate(&files[slot].f, 0);
	}
	files[slot].used = true;
	files[slot].eof = false;
	k_mutex_unlock(&files_mutex);
	return slot + 3;
}

static int fd_slot(int fd)
{
	int slot = fd - 3;

	if (slot < 0 || slot >= MAX_FILES || !files[slot].used) {
		return -1;
	}
	return slot;
}

int close(int fd)
{
	int slot = fd_slot(fd);

	if (slot < 0) {
		return -1;
	}
	fs_close(&files[slot].f);
	files[slot].used = false;
	return 0;
}

ssize_t read(int fd, void *buf, size_t n)
{
	int slot = fd_slot(fd);

	return (slot < 0) ? -1 : fs_read(&files[slot].f, buf, n);
}

ssize_t write(int fd, const void *buf, size_t n)
{
	int slot = fd_slot(fd);

	return (slot < 0) ? -1 : fs_write(&files[slot].f, buf, n);
}

off_t lseek(int fd, off_t off, int whence)
{
	int slot = fd_slot(fd);

	if (slot < 0) {
		return -1;
	}

	int w = (whence == SEEK_CUR) ? FS_SEEK_CUR :
		(whence == SEEK_END) ? FS_SEEK_END : FS_SEEK_SET;

	if (fs_seek(&files[slot].f, off, w) < 0) {
		return -1;
	}
	return (off_t)fs_tell(&files[slot].f);
}

/* ── string/file helpers minimal libc lacks ─────────────────────────────── */

int strcasecmp(const char *a, const char *b)
{
	while (*a && *b) {
		int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);

		if (d) {
			return d;
		}
		a++;
		b++;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

char *strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);

	if (p) {
		memcpy(p, s, n);
	}
	return p;
}

int remove(const char *path)
{
	return (fs_unlink(path) < 0) ? -1 : 0;
}

int rename(const char *from, const char *to)
{
	return (fs_rename(from, to) < 0) ? -1 : 0;
}
