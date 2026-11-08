/* keystore.c — the board's private keys (keystore.h).
 *
 * Keys live in the QSPI configuration flash when the bitstream has the
 * qspi_ctrl block (klaussCPU MMIO 0xF012_0000): NVS on the `storage`
 * partition (0xF0_0000, above the FPGA bitstream), so they survive reboots,
 * SD card swaps and bitstream updates.  On an older bitstream, or if the
 * flash cannot be used, they stay in KEYSTORE_DIR on the SD card as before.
 *
 * Migration: when a key is not in flash but is on the card (KEYSTORE_DIR, or
 * the top of the card from older firmware), it is copied into flash so the
 * board keeps its identity.  The card copy is left in place; `keystore
 * purge_sd` deletes card copies that flash holds byte-for-byte.
 *
 * Booting a bitstream without qspi_ctrl after purge_sd generates new keys on
 * the card; back on a qspi bitstream the flash keys win again, so the SSH
 * host key changes between the two (known_hosts warnings).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_NVS) && DT_NODE_EXISTS(DT_NODELABEL(storage_partition))
#define KEYSTORE_HAVE_FLASH 1
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/kvss/nvs.h>
#else
#define KEYSTORE_HAVE_FLASH 0
#endif

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

#include "keystore.h"

LOG_MODULE_REGISTER(keystore, LOG_LEVEL_INF);

static K_MUTEX_DEFINE(s_lock);

/* ---- SD card backend --------------------------------------------------- */

int keystore_prepare(const char *name)
{
	char from[64];
	char to[64];
	struct fs_dirent ent;
	int rc;

	/* fs_mkdir logs an error for a folder that exists: look first. */
	if (fs_stat(KEYSTORE_DIR, &ent) != 0) {
		rc = fs_mkdir(KEYSTORE_DIR);
		if (rc != 0) {
			LOG_ERR("Cannot create " KEYSTORE_DIR ": %d", rc);
			return rc;
		}
	}

	(void)snprintf(to, sizeof(to), "%s/%s", KEYSTORE_DIR, name);
	(void)snprintf(from, sizeof(from), "/SD:/%s", name);
	if (fs_stat(from, &ent) != 0) {
		return 0;
	}
	if (fs_stat(to, &ent) == 0) {
		/* One value per LOG line (CLAUDE.md: packed multi-arg logs). */
		LOG_WRN("Not used, and readable over HTTP: delete /SD:/%s", name);
		return 0;
	}
	rc = fs_rename(from, to);
	if (rc != 0) {
		LOG_WRN("Cannot move %s into " KEYSTORE_DIR, name);
		return rc;
	}
	LOG_INF("Moved %s into " KEYSTORE_DIR, name);
	return 0;
}

/* Bytes read, or a negative errno (-ENOENT if there is no file).  prepare:
 * run keystore_prepare (mkdir / move from the top of the card) first. */
static int sd_read(const char *name, uint8_t *buf, size_t cap, bool prepare)
{
	char path[64];
	struct fs_file_t f;
	ssize_t br;

	if (prepare) {
		(void)keystore_prepare(name);
	}
	(void)snprintf(path, sizeof(path), "%s/%s", KEYSTORE_DIR, name);
	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) {
		return -ENOENT;
	}
	br = fs_read(&f, buf, cap);
	fs_close(&f);
	return (int)br;
}

static int sd_save(const char *name, const uint8_t *buf, size_t len)
{
	char path[64];
	struct fs_file_t f;
	ssize_t bw;

	(void)keystore_prepare(name);
	(void)snprintf(path, sizeof(path), "%s/%s", KEYSTORE_DIR, name);
	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC) != 0) {
		LOG_ERR("Cannot open %s for writing", path);
		return -EIO;
	}
	bw = fs_write(&f, buf, len);
	fs_close(&f);
	if (bw != (ssize_t)len) {
		LOG_ERR("Write %s failed: %d bytes", path, (int)bw);
		return -EIO;
	}
	return 0;
}

/* ---- QSPI flash backend (NVS on the `storage` partition) ---------------
 *
 * Each key is one NVS entry whose id is a CRC-16 of its name; the entry holds
 * the name too ("name\0" + data), so an id collision reads as "absent" rather
 * than returning another key.  NVS replaces an entry atomically (a power cut
 * mid-write leaves the old value) and spreads wear over the sectors.
 */
#if KEYSTORE_HAVE_FLASH

#define KS_NAME_MAX  32
#define KS_ENTRY_MAX (KS_NAME_MAX + KEYSTORE_VALUE_MAX)

static struct nvs_fs s_nvs;
static int s_flash_state;          /* 0 = not tried, 1 = mounted, <0 = unavailable */
static uint8_t s_entry[KS_ENTRY_MAX];

static uint16_t name_id(const char *name)
{
	uint16_t id = crc16_ccitt(0xffff, (const uint8_t *)name, strlen(name));

	return (id == 0xffff) ? 0xfffe : id;
}

static int flash_mount(void)
{
	const struct flash_area *fa;
	struct flash_pages_info info;
	int rc;

	if (s_flash_state != 0) {
		return (s_flash_state > 0) ? 0 : s_flash_state;
	}
	/* -ENODEV when the flash device is not ready (no qspi_ctrl). */
	rc = flash_area_open(PARTITION_ID(storage_partition), &fa);
	if (rc != 0) {
		LOG_INF("No QSPI flash (%d): keys stay on the SD card", rc);
		s_flash_state = rc;
		return rc;
	}
	rc = flash_get_page_info_by_offs(fa->fa_dev, fa->fa_off, &info);
	if (rc == 0) {
		s_nvs.flash_device = fa->fa_dev;
		s_nvs.offset = fa->fa_off;
		s_nvs.sector_size = info.size;
		s_nvs.sector_count = fa->fa_size / info.size;
	}
	flash_area_close(fa);
	if (rc != 0) {
		s_flash_state = rc;
		return rc;
	}

	rc = nvs_mount(&s_nvs);
	if (rc == -EDEADLK) {
		/* Every sector closed: not an NVS area (foreign data).  Only
		 * then start afresh.  Any other error (an I/O error during
		 * recovery after a power cut, say) leaves the flash alone, and the
		 * keys come from the SD card for this boot. */
		LOG_WRN("Keystore flash area is not NVS: erasing it");
		rc = flash_erase(s_nvs.flash_device, s_nvs.offset,
				 (size_t)s_nvs.sector_size * s_nvs.sector_count);
		if (rc == 0) {
			rc = nvs_mount(&s_nvs);
		}
	}
	if (rc != 0) {
		LOG_ERR("Keystore flash mount failed (%d): using the SD card", rc);
		s_flash_state = rc;
		return rc;
	}
	LOG_INF("Keystore on QSPI flash at 0x%06lx", (long)s_nvs.offset);
	s_flash_state = 1;
	return 0;
}

static int flash_load(const char *name, uint8_t *buf, size_t cap)
{
	size_t nl = strlen(name) + 1;
	ssize_t n;

	n = nvs_read(&s_nvs, name_id(name), s_entry, sizeof(s_entry));
	if (n < 0) {
		return (int)n;             /* -ENOENT when absent */
	}
	if ((size_t)n > sizeof(s_entry) || (size_t)n < nl || memcmp(s_entry, name, nl) != 0) {
		return -ENOENT;            /* id collision, or too large to be ours */
	}
	n -= nl;
	if ((size_t)n > cap) {
		return -ENOSPC;
	}
	memcpy(buf, s_entry + nl, n);
	return (int)n;
}

static int flash_save(const char *name, const uint8_t *buf, size_t len)
{
	size_t nl = strlen(name) + 1;
	ssize_t n;

	if (nl > KS_NAME_MAX || len > KEYSTORE_VALUE_MAX) {
		return -EINVAL;
	}
	memcpy(s_entry, name, nl);
	memcpy(s_entry + nl, buf, len);
	n = nvs_write(&s_nvs, name_id(name), s_entry, nl + len);
	return (n < 0) ? (int)n : 0;   /* 0 bytes written = value unchanged */
}

static bool use_flash(void)
{
	return flash_mount() == 0;
}

#else
static bool use_flash(void)
{
	return false;
}
#endif /* KEYSTORE_HAVE_FLASH */

/* ---- Public API -------------------------------------------------------- */

int keystore_load(const char *name, uint8_t *buf, size_t cap)
{
	int n;

	k_mutex_lock(&s_lock, K_FOREVER);
#if KEYSTORE_HAVE_FLASH
	if (use_flash()) {
		n = flash_load(name, buf, cap);
		if (n != -ENOENT) {
			k_mutex_unlock(&s_lock);
			return n;
		}
		/* Not in flash yet: take the card's copy, if any, and keep it. */
		n = sd_read(name, buf, cap, true);
		if (n > 0) {
			int rc = flash_save(name, buf, n);

			if (rc == 0) {
				LOG_INF("Copied %s from the SD card into QSPI flash", name);
			} else {
				LOG_WRN("Could not copy %s into QSPI flash: %d", name, rc);
			}
		}
		k_mutex_unlock(&s_lock);
		return (n > 0) ? n : -ENOENT;
	}
#endif
	n = sd_read(name, buf, cap, true);
	k_mutex_unlock(&s_lock);
	return (n > 0) ? n : -ENOENT;
}

int keystore_save(const char *name, const uint8_t *buf, size_t len)
{
	int rc;

	if (len > KEYSTORE_VALUE_MAX) {
		return -EINVAL;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
#if KEYSTORE_HAVE_FLASH
	if (use_flash()) {
		rc = flash_save(name, buf, len);
		k_mutex_unlock(&s_lock);
		return rc;
	}
#endif
	rc = sd_save(name, buf, len);
	k_mutex_unlock(&s_lock);
	return rc;
}

const char *keystore_backend(void)
{
	const char *b;

	k_mutex_lock(&s_lock, K_FOREVER);
	b = use_flash() ? "QSPI flash" : "SD card";
	k_mutex_unlock(&s_lock);
	return b;
}

/* ---- `keystore` shell command ------------------------------------------ */
#if defined(CONFIG_SHELL)

/* The keys this firmware keeps (sshkeys.c, tlscert.c). */
static const char *const known_names[] = {
	"ssh_host_key.bin",
	"tls_key.bin",
	"tls_cert.bin",
};

static uint8_t s_a[KEYSTORE_VALUE_MAX];
static uint8_t s_b[KEYSTORE_VALUE_MAX];

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Keys are kept in: %s", keystore_backend());
	k_mutex_lock(&s_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(known_names); i++) {
		const char *name = known_names[i];
		int f = -ENODEV;
		int s = sd_read(name, s_b, sizeof(s_b), false);
		const char *fs_txt;
		const char *sd_txt;

#if KEYSTORE_HAVE_FLASH
		if (use_flash()) {
			f = flash_load(name, s_a, sizeof(s_a));
		}
#endif
		fs_txt = (f > 0) ? "yes" : ((f == -ENODEV) ? "n/a" : "no");
		if (s <= 0) {
			sd_txt = "no";
		} else if (f == s && memcmp(s_a, s_b, s) == 0) {
			sd_txt = "yes (same as flash)";
		} else {
			sd_txt = "yes";
		}
		shell_print(sh, "  %-18s flash: %-4s SD: %s", name, fs_txt, sd_txt);
	}
	k_mutex_unlock(&s_lock);
	return 0;
}

static int cmd_purge_sd(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int removed = 0;

	k_mutex_lock(&s_lock, K_FOREVER);
	if (!use_flash()) {
		k_mutex_unlock(&s_lock);
		shell_error(sh, "Keys are not in QSPI flash; nothing removed");
		return -ENODEV;
	}
#if KEYSTORE_HAVE_FLASH
	for (size_t i = 0; i < ARRAY_SIZE(known_names); i++) {
		const char *name = known_names[i];
		char path[64];
		int f = flash_load(name, s_a, sizeof(s_a));
		int s = sd_read(name, s_b, sizeof(s_b), false);

		if (s <= 0) {
			continue;
		}
		if (f != s || memcmp(s_a, s_b, s) != 0) {
			shell_warn(sh, "  %s: flash copy differs or is missing; kept on SD", name);
			continue;
		}
		(void)snprintf(path, sizeof(path), "%s/%s", KEYSTORE_DIR, name);
		if (fs_unlink(path) == 0) {
			shell_print(sh, "  removed %s", path);
			removed++;
		}
	}
#endif
	k_mutex_unlock(&s_lock);
	shell_print(sh, "%d file(s) removed from the SD card", removed);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_keystore,
	SHELL_CMD(status, NULL, "Where each private key is kept", cmd_status),
	SHELL_CMD(purge_sd, NULL, "Delete SD copies of keys that QSPI flash holds", cmd_purge_sd),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(keystore, &sub_keystore, "Private-key storage (QSPI flash or SD card)",
		   cmd_status);

#endif /* CONFIG_SHELL */
