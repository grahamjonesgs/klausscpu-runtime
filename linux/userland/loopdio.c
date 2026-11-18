/*
 * loopdio — switch the loop device under a mount point (or a /dev/loopN
 * given directly) to direct I/O, with 64 KiB readahead.
 *
 *   loopdio /opt/llvm /opt/rust
 *
 * busybox `mount -o loop` sets up a buffered loop device. For an image file
 * on the card's FAT partition, that caches every block twice: once in the
 * ext4 file system on top and once in the FAT file below. With 121 MB of RAM
 * this halves the room left for a large program's pages. On 2026-11-18
 * rustc hello took 79.5 s and read 549 MB from the card buffered, against
 * 42.7 s and 292 MB with direct I/O. sdinit already does this for the root
 * and /data images.
 *
 * Readahead drops from the default 128 KiB to 64 KiB, one page. Pages
 * faulted in from a large program are scattered, so the extra 64 KiB is
 * usually evicted unused. rustc hello (3 alternating cold runs each) took
 * 38.5 s and read 272 MB at 128 KiB, and 34.8 s and 228 MB at 64 KiB; at
 * 512 KiB it took 60 s. clang was the same either way (82.1 s).
 *
 * Prints one line per argument. Exits 1 if any argument could not be
 * switched; the loop device then stays buffered and still works.
 */
#include <errno.h>
#include <libgen.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The device mounted at dir, from /proc/mounts, or NULL. */
static const char *mounted_dev(const char *dir, char *buf, size_t len)
{
	char line[512], dev[256], mnt[256];
	const char *found = NULL;
	FILE *f = fopen("/proc/mounts", "r");

	if (!f)
		return NULL;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%255s %255s", dev, mnt) == 2 && strcmp(mnt, dir) == 0) {
			snprintf(buf, len, "%s", dev);
			found = buf; /* keep going: the last mount on dir is the visible one */
		}
	fclose(f);
	return found;
}

int main(int argc, char **argv)
{
	int status = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: loopdio <mount point | /dev/loopN>...\n");
		return 2;
	}
	for (int i = 1; i < argc; i++) {
		char buf[256];
		const char *dev = argv[i];
		int fd;

		if (strncmp(dev, "/dev/", 5) != 0 && !(dev = mounted_dev(argv[i], buf, sizeof(buf)))) {
			fprintf(stderr, "loopdio: nothing is mounted at %s\n", argv[i]);
			status = 1;
			continue;
		}
		if (strncmp(dev, "/dev/loop", 9) != 0) {
			fprintf(stderr, "loopdio: %s is %s, not a loop device\n", argv[i], dev);
			status = 1;
			continue;
		}
		fd = open(dev, O_RDONLY | O_CLOEXEC);
		if (fd < 0 || ioctl(fd, LOOP_SET_DIRECT_IO, 1UL) != 0) {
			fprintf(stderr, "loopdio: %s (%s): %s; it stays buffered\n", argv[i], dev, strerror(errno));
			status = 1;
		} else {
			char path[128];
			FILE *ra;
			int ok;

			snprintf(path, sizeof(path), "/sys/block/%s/queue/read_ahead_kb", basename((char *)dev));
			ra = fopen(path, "w");
			ok = ra != NULL;
			if (ra) {
				ok = fputs("64\n", ra) != EOF;
				ok = (fclose(ra) == 0) && ok;
			}
			if (!ok) {
				fprintf(stderr, "loopdio: %s: %s: %s\n", argv[i], path, strerror(errno));
				status = 1;
			}
			printf("loopdio: %s (%s): direct I/O%s\n", argv[i], dev, ok ? ", 64 KiB readahead" : "");
		}
		if (fd >= 0)
			close(fd);
	}
	return status;
}
