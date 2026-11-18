/*
 * loopdio — switch the loop device under a mount point (or a /dev/loopN
 * given directly) to direct I/O.
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
 * Prints one line per argument. Exits 1 if any argument could not be
 * switched; the loop device then stays buffered and still works.
 */
#include <errno.h>
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
			printf("loopdio: %s (%s): direct I/O\n", argv[i], dev);
		}
		if (fd >= 0)
			close(fd);
	}
	return status;
}
