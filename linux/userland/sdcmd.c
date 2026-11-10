// SPDX-License-Identifier: GPL-2.0
/*
 * sdcmd: SD commands with odd data sizes, through mmc_block's MMC_IOC_CMD,
 * for tools/validate/linux.sh's sd-native mode (the emulator's card).
 *   sdcmd scr <dev>             ACMD51: the 8-byte SCR, in hex
 *   sdcmd ssr <dev>             ACMD13: the 64-byte SD status, first bytes
 *   sdcmd wtest <dev> <sector>  write 8 sectors from <sector> with O_DIRECT
 *                               (one CMD25), ACMD22 (4 bytes: the blocks
 *                               the last write got onto the card), then
 *                               read them back and compare
 * wtest overwrites raw sectors: test images only, never a card with files.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/mmc/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* include/linux/mmc/core.h (not uapi) */
#define MMC_RSP_PRESENT	(1 << 0)
#define MMC_RSP_CRC	(1 << 2)
#define MMC_RSP_OPCODE	(1 << 4)
#define MMC_RSP_R1	(MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)
#define MMC_CMD_ADTC	(1 << 5)

static int acmd_read(int fd, unsigned int op, void *buf, unsigned int len)
{
	struct mmc_ioc_cmd c;

	memset(&c, 0, sizeof(c));
	c.is_acmd = 1;
	c.opcode = op;
	c.flags = MMC_RSP_R1 | MMC_CMD_ADTC;
	c.blksz = len;
	c.blocks = 1;
	mmc_ioc_cmd_set_data(c, buf);
	if (ioctl(fd, MMC_IOC_CMD, &c)) {
		perror("sdcmd: MMC_IOC_CMD");
		return -1;
	}
	return 0;
}

static void hex(const char *what, const unsigned char *b, int n)
{
	printf("%s:", what);
	for (int i = 0; i < n; i++)
		printf(" %02x", b[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	unsigned char small[64];
	unsigned char *w, *r;
	long long sector;
	int fd, i;

	if (argc < 3)
		goto usage;
	fd = open(argv[2], O_RDWR | (strcmp(argv[1], "wtest") ? 0 : O_DIRECT));
	if (fd < 0) {
		perror(argv[2]);
		return 1;
	}
	if (!strcmp(argv[1], "scr")) {
		if (acmd_read(fd, 51, small, 8))
			return 1;
		hex("scr", small, 8);
		return 0;
	}
	if (!strcmp(argv[1], "ssr")) {
		if (acmd_read(fd, 13, small, 64))
			return 1;
		hex("ssr", small, 8);
		return 0;
	}
	if (strcmp(argv[1], "wtest") || argc < 4)
		goto usage;
	sector = atoll(argv[3]);
	if (posix_memalign((void **)&w, 4096, 4096) || posix_memalign((void **)&r, 4096, 4096))
		return 1;
	for (i = 0; i < 4096; i++)
		w[i] = (unsigned char)(i * 7 + 3 + (i >> 9));
	if (pwrite(fd, w, 4096, sector * 512) != 4096) {
		perror("sdcmd: pwrite");
		return 1;
	}
	if (acmd_read(fd, 22, small, 4))
		return 1;
	/* ACMD22's count is big-endian. */
	printf("acmd22: %u blocks written\n",
	       (unsigned int)small[0] << 24 | small[1] << 16 | small[2] << 8 | small[3]);
	if (pread(fd, r, 4096, sector * 512) != 4096) {
		perror("sdcmd: pread");
		return 1;
	}
	printf("wtest: read back %s\n", memcmp(w, r, 4096) ? "DIFFERS" : "same");
	return 0;
usage:
	fprintf(stderr, "usage: sdcmd scr|ssr <dev> | sdcmd wtest <dev> <sector>\n");
	return 2;
}
