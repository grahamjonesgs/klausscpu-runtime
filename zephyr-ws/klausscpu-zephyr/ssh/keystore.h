/* keystore.h — where the board keeps its private keys on the SD card.
 *
 * KEYSTORE_DIR holds the SSH host key and the TLS key and certificate.  The
 * HTTP and HTTPS file servers refuse it (httpd.c), so the keys can only be
 * reached with a login (SSH, SFTP). */
#ifndef KLAUSSCPU_KEYSTORE_H
#define KLAUSSCPU_KEYSTORE_H

#define KEYSTORE_DIR "/SD:/sys"

/* Make KEYSTORE_DIR if needed, and move the file `name` into it from the top
 * of the card, where earlier firmware kept it, so the board keeps its
 * identity.  Returns 0 or a negative errno; the caller then just generates
 * a new key. */
int keystore_prepare(const char *name);

#endif
