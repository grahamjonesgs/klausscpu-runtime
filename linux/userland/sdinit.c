// SPDX-License-Identifier: GPL-2.0
/*
 * sdinit: the initramfs's /init.  Boots the root file system kept on the
 * SD card without repartitioning or reformatting it.  The card's single FAT
 * partition holds two ext4 images in /linux/:
 *   rootfs.img  the system, replaced whole by install-rootfs: loop-mounted
 *               READ-ONLY as /;
 *   data.img    persistent data (/root, /home, local settings in /etc),
 *               made once by install-data: loop-mounted read-write at
 *               /data.  If it is missing or doesn't mount, /data is a tmpfs
 *               with the same directories, so the system still boots (and
 *               rcS warns that nothing there survives a reboot).  It is
 *               never checked or repaired here, beyond the ext4 journal
 *               replay at mount.
 * Anything wrong with the system image (no card, no image, a mount
 * failing) falls back to the initramfs's own userland (busybox init).
 *
 * Order: /test/rootfs.img when a test build put one in the initramfs, then
 * /mnt/sd/linux/rootfs.img on mmcblk0p1; data.img sits beside it.  After
 * the switch the card stays mounted at /mnt (the image files live on it).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <libgen.h>
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
static int loop_attach(const char *image, char *dev, size_t devlen, int ro)
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
	fd = open(image, (ro ? O_RDONLY : O_RDWR) | O_CLOEXEC);
	if (fd < 0)
		return -1;
	lfd = open(dev, O_RDWR | O_CLOEXEC);
	if (lfd < 0) {
		close(fd);
		return -1;
	}
	memset(&cfg, 0, sizeof(cfg));
	cfg.fd = fd;
	cfg.info.lo_flags = LO_FLAGS_AUTOCLEAR | (ro ? LO_FLAGS_READ_ONLY : 0);
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

/*
 * Mount image (ext4) at dir through a new loop device.  A read-only image
 * built before rootfs.img lost its journal may have one that needs
 * recovery, which a read-only device can't do: mount it without the
 * journal then (noload; read-only, so nothing is written).
 */
static int loop_mount(const char *image, const char *dir, int ro)
{
	char dev[32];
	int lfd, r;

	lfd = loop_attach(image, dev, sizeof(dev), ro);
	if (lfd < 0)
		return -1;
	r = mount(dev, dir, "ext4", ro ? MS_RDONLY : 0, "");
	if (r && ro)
		r = mount(dev, dir, "ext4", MS_RDONLY, "noload");
	close(lfd);	/* detaches the device if the mount failed */
	return r;
}

static int try_root(const char *image)
{
	if (access(image, R_OK))
		return -1;
	mkdir("/newroot", 0755);
	if (loop_mount(image, "/newroot", 1)) {
		msg("cannot mount the image (read-only ext4 through a loop device)");
		return -1;
	}
	if (access("/newroot/sbin/init", X_OK)) {
		msg("the image has no /sbin/init");
		umount("/newroot");
		return -1;
	}
	return 0;
}

/*
 * /data in the new root: data.img beside the system image, else a tmpfs
 * with the same directories.  Only for system images that have /data (the
 * layout with /root and /home linked into it).
 */
static void mount_data(const char *sysimg)
{
	char img[256], next[256], old[256], dir[256];
	int staged = 0;

	if (access("/newroot/data", F_OK))
		return;
	snprintf(dir, sizeof(dir), "%s", sysimg);
	snprintf(img, sizeof(img), "%s/data.img", dirname(dir));
	snprintf(next, sizeof(next), "%s.next", img);
	snprintf(old, sizeof(old), "%s.old", img);
	/*
	 * A replacement staged by install-data --force while data.img was in
	 * use.  The old image is kept as data.img.old (the user deletes it),
	 * and comes back if the new one doesn't mount.
	 */
	if (access(next, R_OK) == 0) {
		unlink(old);
		if (rename(img, old) == 0 || errno == ENOENT) {
			if (rename(next, img) == 0) {
				staged = 1;
				msg("installed the staged data.img.next (install-data --force); the old one is data.img.old");
			}
		}
		sync();
	}
	if (access(img, F_OK) == 0) {
		if (loop_mount(img, "/newroot/data", 0) == 0) {
			msg("data.img mounted read-write at /data");
			return;
		}
		if (staged && access(old, F_OK) == 0) {
			snprintf(dir, sizeof(dir), "%s.bad", img);
			unlink(dir);
			rename(img, dir);
			rename(old, img);
			sync();
			msg("WARNING: the new data.img does not mount: kept as data.img.bad, the old one restored");
			if (loop_mount(img, "/newroot/data", 0) == 0) {
				msg("data.img mounted read-write at /data");
				return;
			}
		}
		fprintf(stderr, "sdinit: WARNING: %s does not mount as ext4 (left as it is)\n", img);
	} else {
		fprintf(stderr, "sdinit: WARNING: no %s\n", img);
	}
	msg("WARNING: /data is a tmpfs: /root, /home and /data/etc are lost at reboot");
	if (mount("tmpfs", "/newroot/data", "tmpfs", 0, "mode=755") == 0) {
		mkdir("/newroot/data/root", 0700);
		mkdir("/newroot/data/home", 0755);
		mkdir("/newroot/data/etc", 0755);
	}
}

int main(void)
{
	const char *sysimg = "/test/rootfs.img";
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
			sysimg = "/mnt/sd/linux/rootfs.img";
			if (try_root(sysimg)) {
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

	/* Switch: the card, /data and /dev move under the new root; proc and
	 * sys are mounted again by its rcS. */
	mount_data(sysimg);
	msg("switching to the SD card's root file system (read-only)");
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
