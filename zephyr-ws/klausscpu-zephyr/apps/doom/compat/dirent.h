/* Minimal POSIX <dirent.h> for the Doom port (IWAD search only — stubbed). */
#ifndef DOOM_COMPAT_DIRENT_H
#define DOOM_COMPAT_DIRENT_H

struct dirent {
	char d_name[256];
};
typedef struct DIR DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
int            closedir(DIR *d);

#endif
