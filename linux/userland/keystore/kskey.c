// SPDX-License-Identifier: GPL-2.0
/*
 * kskey: read a key from the Zephyr port's QSPI keystore (ssh/keystore.c):
 * Zephyr NVS (subsys/kvss/nvs, v4.5, CONFIG_NVS_DATA_CRC=y) on the flash's
 * "storage" partition, 64 KiB sectors.  Each key is one NVS entry whose id
 * is crc16_ccitt(0xffff, name) (0xffff -> 0xfffe) and whose data is
 * "name\0" + the value; the host key ("ssh_host_key.bin") is an ECC P-256
 * private key in SEC1 DER (wolfSSL wc_EccKeyToDer).
 *
 *   kskey [-n name] [-s sector_size] [-d] <partition image or /dev/mtdN>
 *
 * prints the value as PEM ("EC PRIVATE KEY"), or raw DER with -d.  Read-only.
 * Builds for the host (unit tests) and for the board (musl).
 *
 * NVS on flash (nvs.c): sectors are used in a ring.  ATEs (8 bytes: id,
 * offset, len, part, crc8) grow down from the sector's end, whose last slot
 * is the "close" ATE; data grows up from the start.  The newest sector is
 * the open one after a closed one.  Replaying sectors oldest -> newest and
 * each sector's ATEs in write order (high -> low address), the last valid
 * ATE with the id wins (len 0 = deleted).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATE 8

static uint8_t crc8_ccitt(uint8_t val, const uint8_t *p, size_t n)
{
	static const uint8_t t[16] = { 0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15,
				       0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d };
	for (size_t i = 0; i < n; i++) {
		val ^= p[i];
		val = (uint8_t)(val << 4) ^ t[val >> 4];
		val = (uint8_t)(val << 4) ^ t[val >> 4];
	}
	return val;
}

static uint16_t crc16_ccitt(uint16_t seed, const uint8_t *src, size_t len)
{
	for (; len > 0; len--) {
		uint8_t e = seed ^ *src++, f = e ^ (uint8_t)(e << 4);

		seed = (seed >> 8) ^ ((uint16_t)f << 8) ^ ((uint16_t)f << 3) ^ ((uint16_t)f >> 4);
	}
	return seed;
}

static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;

	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & -(c & 1));
	}
	return ~c;
}

static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int erased(const uint8_t *p, size_t n)
{
	while (n--)
		if (*p++ != 0xff)
			return 0;
	return 1;
}

/* Find the newest value of id: 0 (with data and len set), or -1. */
static int nvs_find(const uint8_t *img, size_t size, size_t ss, uint16_t id,
		    const uint8_t **data, size_t *len)
{
	size_t count = size / ss, i, ws = (size_t)-1, closed = 0;
	const uint8_t *found = NULL;
	size_t found_len = 0;

	if (count < 2)
		return -1;
	for (i = 0; i < count; i++) {
		if (!erased(img + i * ss + ss - ATE, ATE)) {
			closed++;
			if (erased(img + ((i + 1) % count) * ss + ss - ATE, ATE)) {
				ws = (i + 1) % count;
				break;
			}
		}
	}
	if (closed == count)
		return -1;			/* every sector closed: not NVS */
	if (ws == (size_t)-1)			/* none closed (nvs_startup's rule) */
		ws = erased(img + (count - 1) * ss + ss - 2 * ATE, ATE) ? 0 : count - 1;

	for (i = 1; i <= count; i++) {
		size_t s = (ws + i) % count;
		const uint8_t *sec = img + s * ss;
		size_t a;

		for (a = ss - 2 * ATE; a + ATE <= ss; a -= ATE) {
			const uint8_t *e = sec + a;
			uint16_t eid = rd16(e), off = rd16(e + 2), elen = rd16(e + 4);

			if (erased(e, ATE))
				break;			/* no more ATEs in this sector */
			if (crc8_ccitt(0xff, e, 7) != e[7] || (size_t)off + elen > a)
				continue;		/* torn write: skip */
			if (eid == id) {
				found = elen ? sec + off : NULL;
				found_len = elen;
			}
			if (a < ATE)
				break;
		}
	}
	if (!found || found_len < 4)
		return -1;
	/* CONFIG_NVS_DATA_CRC: a CRC-32 of the data follows it. */
	if (crc32_ieee(found, found_len - 4) != rd32(found + found_len - 4)) {
		fprintf(stderr, "kskey: data CRC mismatch\n");
		return -1;
	}
	*data = found;
	*len = found_len - 4;
	return 0;
}

static void pem(const uint8_t *d, size_t n)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, col = 0;

	puts("-----BEGIN EC PRIVATE KEY-----");
	for (i = 0; i < n; i += 3) {
		uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
		char q[4] = { b64[v >> 18], b64[(v >> 12) & 63],
			      i + 1 < n ? b64[(v >> 6) & 63] : '=', i + 2 < n ? b64[v & 63] : '=' };

		fwrite(q, 1, 4, stdout);
		if ((col += 4) == 64) {
			putchar('\n');
			col = 0;
		}
	}
	if (col)
		putchar('\n');
	puts("-----END EC PRIVATE KEY-----");
}

int main(int argc, char **argv)
{
	const char *name = "ssh_host_key.bin", *path = NULL;
	size_t ss = 0x10000, size = 0, cap = 0, len, nl;
	const uint8_t *data;
	uint8_t *img = NULL;
	int der = 0, i;
	uint16_t id;
	FILE *f;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc)
			name = argv[++i];
		else if (!strcmp(argv[i], "-s") && i + 1 < argc)
			ss = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-d"))
			der = 1;
		else
			path = argv[i];
	}
	if (!path) {
		fprintf(stderr, "usage: kskey [-n name] [-s sector_size] [-d] <image|/dev/mtdN>\n");
		return 2;
	}
	f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}
	for (;;) {
		size_t n;

		if (size == cap) {
			cap = cap ? cap * 2 : 0x10000;
			img = realloc(img, cap);
			if (!img)
				return 1;
		}
		n = fread(img + size, 1, cap - size, f);
		if (!n)
			break;
		size += n;
	}
	fclose(f);

	nl = strlen(name) + 1;
	id = crc16_ccitt(0xffff, (const uint8_t *)name, nl - 1);
	if (id == 0xffff)
		id = 0xfffe;
	if (nvs_find(img, size, ss, id, &data, &len) || len < nl || memcmp(data, name, nl)) {
		fprintf(stderr, "kskey: no \"%s\" in %s\n", name, path);
		return 1;
	}
	data += nl;
	len -= nl;
	if (der)
		fwrite(data, 1, len, stdout);
	else
		pem(data, len);
	return 0;
}
