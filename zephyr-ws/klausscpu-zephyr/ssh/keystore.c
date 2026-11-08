/* keystore.c — the private-key folder on the SD card (keystore.h). */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdio.h>

#include "keystore.h"

LOG_MODULE_REGISTER(keystore, LOG_LEVEL_INF);

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
