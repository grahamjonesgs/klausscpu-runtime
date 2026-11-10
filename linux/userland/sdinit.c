// SPDX-License-Identifier: GPL-2.0
/*
 * sdinit: the initramfs's /init.  Boots the root file system kept on the
 * SD card without repartitioning or reformatting it: the card's single FAT
 * partition holds /linux/rootfs.img, an ext4 image, which is loop-mounted
 * and switched to.  Anything missing (no card, no image, a mount failing)
 * falls back to the initramfs's own userland (busybox init).
 *
 * Order: /test/rootfs.img when a test build put one in the initramfs, then
 * /mnt/sd/linux/rootfs.img on mmcblk0p1.  After the switch the card stays
 * mounted at /mnt (the image file lives on it).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static void msg(const char *s)
{
	fprintf(stderr, "sdinit: %s\n", s);
}

/*
 * Attach image to a free loop device, its path in dev.  Returns the loop
 * device's open fd, or -1.  The caller closes it only after mounting:
 * with LO_FLAGS_AUTOCLEAR the last close of an unmounted loop device
 * detaches it.
 */
static int loop_attach(const char *image, char *dev, size_t devlen)
{
	struct loop_config cfg;
	int ctl, n, fd, lfd;

	ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		return -1;
	n = ioctl(ctl, LOOP_CTL_GET_FREE);
	close(ctl);
	if (n < 0)
		return -1;
	snprintf(dev, devlen, "/dev/loop%d", n);
	fd = open(image, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	lfd = open(dev, O_RDWR | O_CLOEXEC);
	if (lfd < 0) {
		close(fd);
		return -1;
	}
	memset(&cfg, 0, sizeof(cfg));
	cfg.fd = fd;
	cfg.info.lo_flags = LO_FLAGS_AUTOCLEAR;
	strncpy((char *)cfg.info.lo_file_name, image, LO_NAME_SIZE - 1);
	n = ioctl(lfd, LOOP_CONFIGURE, &cfg);
	close(fd);
	if (n) {
		close(lfd);
		return -1;
	}
	return lfd;
}

/*
 * The native SD controller's card comes through the mmc core, which finds
 * it asynchronously, maybe after /init starts; the SPI driver's disk is
 * there before.  With an mmc host present, wait a little for the partition.
 */
static void wait_for_card(void)
{
	struct dirent *e;
	DIR *d = opendir("/sys/class/mmc_host");
	int i, hosts = 0;

	if (!d)
		return;
	while ((e = readdir(d)))
		hosts += e->d_name[0] != '.';
	closedir(d);
	for (i = 0; hosts && i < 30 && access("/dev/mmcblk0p1", F_OK); i++)
		usleep(100000);
}

static int try_root(const char *image)
{
	char dev[32];
	int lfd, r;

	if (access(image, R_OK))
		return -1;
	lfd = loop_attach(image, dev, sizeof(dev));
	if (lfd < 0) {
		msg("cannot attach the image to a loop device");
		return -1;
	}
	mkdir("/newroot", 0755);
	r = mount(dev, "/newroot", "ext4", 0, "");
	close(lfd);	/* detaches the device if the mount failed */
	if (r) {
		msg("cannot mount the image as ext4");
		return -1;
	}
	if (access("/newroot/sbin/init", X_OK)) {
		msg("the image has no /sbin/init");
		umount("/newroot");
		return -1;
	}
	return 0;
}

int main(void)
{
	int sd = 0;

	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mkdir("/dev", 0755);
	mount("proc", "/proc", "proc", 0, "");
	mount("sysfs", "/sys", "sysfs", 0, "");
	mount("devtmpfs", "/dev", "devtmpfs", 0, "");

	if (try_root("/test/rootfs.img")) {
		mkdir("/mnt", 0755);
		mkdir("/mnt/sd", 0755);
		wait_for_card();
		if (mount("/dev/mmcblk0p1", "/mnt/sd", "vfat", 0, "utf8") == 0) {
			sd = 1;
			/* An update staged by install-rootfs while this image was
			 * in use: swap it in now, before anything opens the old
			 * one (replacing an open file on FAT leaks its clusters). */
			if (access("/mnt/sd/linux/rootfs.img.next", R_OK) == 0) {
				unlink("/mnt/sd/linux/rootfs.img");
				if (rename("/mnt/sd/linux/rootfs.img.next", "/mnt/sd/linux/rootfs.img") == 0)
					msg("installed the staged /linux/rootfs.img.next");
				else
					msg("cannot rename /linux/rootfs.img.next");
				sync();
			}
			if (try_root("/mnt/sd/linux/rootfs.img")) {
				msg("no usable /linux/rootfs.img on the SD card");
				umount("/mnt/sd");
				sd = 0;
				goto fallback;
			}
		} else {
			msg("no SD card (mmcblk0p1)");
			goto fallback;
		}
	}

	/* Switch: the card and /dev move under the new root; proc and sys are
	 * mounted again by its rcS. */
	msg("switching to the SD card's root file system");
	if (sd) {
		mkdir("/newroot/mnt", 0755);
		mount("/mnt/sd", "/newroot/mnt", NULL, MS_MOVE, NULL);
	}
	mount("/dev", "/newroot/dev", NULL, MS_MOVE, NULL);
	umount("/proc");
	umount("/sys");
	if (chdir("/newroot") || mount(".", "/", NULL, MS_MOVE, NULL) || chroot(".") || chdir("/")) {
		perror("sdinit: switch");
		return 1;
	}
	execl("/sbin/init", "init", (char *)NULL);
	perror("sdinit: exec /sbin/init");
	return 1;

fallback:
	msg("staying in the initramfs");
	umount("/proc");
	umount("/sys");
	umount("/dev");
	execl("/sbin/init", "init", (char *)NULL);
	perror("sdinit: exec /sbin/init");
	return 1;
}
