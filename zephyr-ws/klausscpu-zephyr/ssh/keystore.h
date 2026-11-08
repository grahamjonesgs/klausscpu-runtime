/* keystore.h — where the board keeps its private keys.
 *
 * The SSH host key and the TLS key and certificate live in the QSPI
 * configuration flash (NVS on the `storage` partition, above the FPGA
 * bitstream) when the bitstream has klaussCPU's qspi_ctrl, so they survive
 * SD card swaps.  Otherwise they are files in KEYSTORE_DIR on the SD card.
 * The HTTP and HTTPS file servers refuse KEYSTORE_DIR (httpd.c), so the card
 * copies can only be reached with a login (SSH, SFTP). */
#ifndef KLAUSSCPU_KEYSTORE_H
#define KLAUSSCPU_KEYSTORE_H

#define KEYSTORE_DIR "/SD:/sys"

#include <stddef.h>
#include <stdint.h>

/* Largest value keystore_save() takes (a key or certificate in DER). */
#define KEYSTORE_VALUE_MAX 1024

/* Make KEYSTORE_DIR if needed, and move the file `name` into it from the top
 * of the card, where earlier firmware kept it, so the board keeps its
 * identity.  Returns 0 or a negative errno; the caller then just generates
 * a new key.  (keystore_load/save call it for the SD backend.) */
int keystore_prepare(const char *name);

/* Read the value `name` into buf.  Returns its length, -ENOENT if there is
 * none, or another negative errno.  With the QSPI flash keystore (a
 * bitstream with qspi_ctrl) it reads flash; a key found only on the SD card
 * is copied into flash on the way, so the board keeps its identity. */
int keystore_load(const char *name, uint8_t *buf, size_t cap);

/* Store `name` (up to KEYSTORE_VALUE_MAX bytes): in QSPI flash when it is
 * available, else in KEYSTORE_DIR on the SD card.  0 or a negative errno. */
int keystore_save(const char *name, const uint8_t *buf, size_t len);

/* "QSPI flash" or "SD card": where keystore_save() puts keys. */
const char *keystore_backend(void);

#endif
