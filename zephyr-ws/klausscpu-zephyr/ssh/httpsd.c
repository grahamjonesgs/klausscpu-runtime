/*
 * httpsd.c — HTTPS file server for KlaussCPU Zephyr (HTTP over TLS, :443).
 *
 * wolfSSL is already linked for wolfSSH (SSH), and CONFIG_WOLFSSL builds the
 * full TLS layer (ssl.c/tls.c/tls13.c), so HTTPS is mostly wiring: present a
 * self-signed ECDSA P-256 cert (tlscert.c), wrap each accepted TCP socket in a
 * WOLFSSL session, and run the shared transport-agnostic request dispatch
 * (httpd_serve) over wolfSSL_read/wolfSSL_write instead of raw sockets.
 *
 * Supports TLS 1.2 + 1.3 (wolfSSLv23_server_method, min TLS 1.2).  Cipher
 * suites are ECDHE-ECDSA-AES-GCM (1.2) / TLS_AES_*_GCM (1.3) — all ECC, so no
 * RSA/DH is required (matching the SSH-tuned wolfSSL config).
 *
 * user_settings.h sets WOLFSSL_NO_SOCK + WOLFSSL_USER_IO, so the encrypted
 * bytes move over the socket via custom I/O callbacks (net_recv/net_send),
 * exactly as ssh_server.c does for wolfSSH.
 *
 * Single connection at a time (one worker thread, blocking accept loop) — same
 * model as the plain :80 server; the board is a single-user dev target.
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <errno.h>

#include <wolfssl/ssl.h>
#include <wolfssl/wolfio.h>

#include "httpd.h"
#include "httpsd.h"
#include "tlscert.h"

LOG_MODULE_REGISTER(httpsd, LOG_LEVEL_INF);

#define HTTPS_PORT        443
#define XFER_SZ           2048

/* TLS handshakes on KlaussCPU's 64-bit stack frames are heavy — the wolfSSH
 * connection threads use 16 KiB for the same reason (see ssh_server.c). */
#define HTTPSD_STACK_SIZE 16384
#define HTTPSD_PRIO       7

/* Concurrent TLS connections: the accept loop hands each accepted socket to a
 * free worker (own stack + WOLFSSL session + transfer buffer) and keeps
 * accepting, so one keep-alive client (e.g. the dashboard polling) can't block
 * others or starve the TCP connection-context pool.  Mirrors ssh_server.c.
 * Sized for a busy browser's parallel keep-alive connections plus other clients. */
#define HTTPSD_MAX_CONNS  8

/* Per-connection recv timeout / keep-alive idle timeout — short so idle spare
 * connections free their worker (and TLS session) quickly; see httpd.c. */
#define HTTPSD_RECV_TIMEOUT_S  5

/* Accept-loop thread. */
static K_THREAD_STACK_DEFINE(httpsd_stack, HTTPSD_STACK_SIZE);
static struct k_thread httpsd_thread;

/* Per-connection worker pool. */
static K_THREAD_STACK_ARRAY_DEFINE(httpsd_conn_stacks, HTTPSD_MAX_CONNS,
				   HTTPSD_STACK_SIZE);
static struct k_thread httpsd_conn_threads[HTTPSD_MAX_CONNS];
static int      httpsd_conn_sock[HTTPSD_MAX_CONNS];  /* fd (stable per slot) */
static WOLFSSL *httpsd_conn_ssl[HTTPSD_MAX_CONNS];
static bool     httpsd_conn_busy[HTTPSD_MAX_CONNS];
static bool     httpsd_conn_used[HTTPSD_MAX_CONNS];  /* slot's k_thread ever created */
static char     httpsd_xfer[HTTPSD_MAX_CONNS][XFER_SZ];
static K_MUTEX_DEFINE(httpsd_conn_mutex);

static WOLFSSL_CTX *s_ctx;

/* ── wolfSSL <-> socket I/O callbacks (NO_SOCK / USER_IO) ─────────────────── */

static int net_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
	ARG_UNUSED(ssl);
	int sock = *(int *)ctx;
	int n = zsock_recv(sock, buf, sz, 0);

	if (n == 0) {
		return WOLFSSL_CBIO_ERR_CONN_CLOSE;
	}
	if (n < 0) {
		return WOLFSSL_CBIO_ERR_GENERAL;
	}
	return n;
}

static int net_send(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
	ARG_UNUSED(ssl);
	int sock = *(int *)ctx;
	int n = zsock_send(sock, buf, sz, 0);

	if (n < 0) {
		return WOLFSSL_CBIO_ERR_GENERAL;
	}
	return n;
}

/* ── httpd_conn transport over a TLS session ─────────────────────────────── */

static int tls_recv(void *ctx, void *buf, size_t len)
{
	return wolfSSL_read((WOLFSSL *)ctx, buf, (int)len);
}

static int tls_send(void *ctx, const void *buf, size_t len)
{
	return wolfSSL_write((WOLFSSL *)ctx, buf, (int)len);
}

/* ── per-connection worker ────────────────────────────────────────────────── */

/* Run the TLS handshake + serve loop for one accepted socket on its own thread
 * (the WOLFSSL session was created in the accept loop), then tear it down and
 * release the pool slot.  Each worker uses its own transfer buffer. */
static void httpsd_conn_entry(void *p1, void *p2, void *p3)
{
	int slot = (int)(uintptr_t)p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int cfd = httpsd_conn_sock[slot];
	WOLFSSL *ssl = httpsd_conn_ssl[slot];

	/* Bound blocking recv (handshake + request reads + keep-alive idle) so an
	 * idle or stalled connection frees its worker (and TLS session) quickly. */
	struct zsock_timeval tv = { .tv_sec = HTTPSD_RECV_TIMEOUT_S, .tv_usec = 0 };

	(void)zsock_setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* Point wolfSSL's I/O at this slot's stable fd storage. */
	wolfSSL_SetIOReadCtx(ssl, &httpsd_conn_sock[slot]);
	wolfSSL_SetIOWriteCtx(ssl, &httpsd_conn_sock[slot]);

	int rc = wolfSSL_accept(ssl);

	if (rc != WOLFSSL_SUCCESS) {
		LOG_WRN("TLS handshake failed (slot %d): err=%d", slot,
			wolfSSL_get_error(ssl, rc));
	} else {
		struct httpd_conn conn = {
			.io_ctx = ssl,
			.io_recv = tls_recv,
			.io_send = tls_send,
			.xfer = httpsd_xfer[slot],
			.xfer_sz = XFER_SZ,
		};

		/* Serve multiple requests over this one TLS session (keep-alive)
		 * until the client closes, errors, or the recv timeout fires. */
		while (httpd_serve(&conn)) {
		}
		(void)wolfSSL_shutdown(ssl);
	}

	wolfSSL_free(ssl);
	(void)zsock_close(cfd);

	k_mutex_lock(&httpsd_conn_mutex, K_FOREVER);
	httpsd_conn_busy[slot] = false;
	k_mutex_unlock(&httpsd_conn_mutex);
}

/* ── server thread ──────────────────────────────────────────────────────── */

static void httpsd_main(void *a, void *b, void *cc)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(cc);

	int srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (srv < 0) {
		LOG_ERR("socket failed: %d", -errno);
		return;
	}

	int yes = 1;

	(void)zsock_setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(HTTPS_PORT),
		.sin_addr.s_addr = INADDR_ANY,
	};

	if (zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("bind :%d failed: %d", HTTPS_PORT, -errno);
		(void)zsock_close(srv);
		return;
	}
	if (zsock_listen(srv, HTTPSD_MAX_CONNS) < 0) {
		LOG_ERR("listen failed: %d", -errno);
		(void)zsock_close(srv);
		return;
	}

	LOG_INF("HTTPS file server on :%d (root /SD:)", HTTPS_PORT);

	for (;;) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int cfd = zsock_accept(srv, (struct sockaddr *)&peer, &plen);

		if (cfd < 0) {
			continue;
		}

		/* Hand off to a free worker thread; keep accepting meanwhile. */
		int slot = -1;

		k_mutex_lock(&httpsd_conn_mutex, K_FOREVER);
		for (int i = 0; i < HTTPSD_MAX_CONNS; i++) {
			if (!httpsd_conn_busy[i]) {
				httpsd_conn_busy[i] = true;
				slot = i;
				break;
			}
		}
		k_mutex_unlock(&httpsd_conn_mutex);

		if (slot < 0) {
			LOG_WRN("HTTPS :%d busy (%d conns), rejecting",
				HTTPS_PORT, HTTPSD_MAX_CONNS);
			(void)zsock_close(cfd);
			continue;
		}

		/* Create the WOLFSSL session here (accept loop, serialised) —
		 * like ssh_server's wolfSSH_new — so concurrent workers don't
		 * race on the shared CTX; the worker runs the handshake + serve. */
		WOLFSSL *ssl = wolfSSL_new(s_ctx);

		if (ssl == NULL) {
			LOG_ERR("wolfSSL_new failed");
			(void)zsock_close(cfd);
			k_mutex_lock(&httpsd_conn_mutex, K_FOREVER);
			httpsd_conn_busy[slot] = false;
			k_mutex_unlock(&httpsd_conn_mutex);
			continue;
		}

		httpsd_conn_sock[slot] = cfd;
		httpsd_conn_ssl[slot] = ssl;

		/* Join the slot's previous worker before recreating its k_thread:
		 * the worker clears httpsd_conn_busy[slot] just before returning,
		 * so without this the struct could be recreated mid-teardown and
		 * corrupt the slot.  Returns immediately on a free slot. */
		if (httpsd_conn_used[slot]) {
			(void)k_thread_join(&httpsd_conn_threads[slot], K_FOREVER);
		}
		httpsd_conn_used[slot] = true;

		(void)k_thread_create(&httpsd_conn_threads[slot],
				      httpsd_conn_stacks[slot], HTTPSD_STACK_SIZE,
				      httpsd_conn_entry, (void *)(uintptr_t)slot,
				      NULL, NULL, HTTPSD_PRIO, 0, K_NO_WAIT);

		char tname[16];

		(void)snprintk(tname, sizeof(tname), "httpsdc%d", slot);
		(void)k_thread_name_set(&httpsd_conn_threads[slot], tname);
	}
}

/* ── public API ─────────────────────────────────────────────────────────── */

int httpsd_start(void)
{
	/* wolfSSL_Init is idempotent; wolfSSH calls its own init separately. */
	if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
		LOG_ERR("wolfSSL_Init failed");
		return -1;
	}

	/* Board IPv4 (network byte order) for the cert's iPAddress SAN, so
	 * browsers accept https://<ip> without a name-mismatch error.  DHCP is
	 * already bound by the time httpsd_start() runs (see main.c). */
	const uint8_t *ip4 = NULL;
	uint8_t ipbuf[4];
	struct net_if *iface = net_if_get_default();

	if (iface != NULL) {
		struct net_if_config *cfg = net_if_get_config(iface);

		if (cfg != NULL && cfg->ip.ipv4 != NULL) {
			memcpy(ipbuf,
			       &cfg->ip.ipv4->unicast[0].ipv4.address.in_addr.s_addr,
			       4);
			ip4 = ipbuf;
		}
	}

	if (tlscert_init(ip4) != 0) {
		LOG_ERR("TLS cert init failed");
		return -1;
	}

	s_ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
	if (s_ctx == NULL) {
		LOG_ERR("wolfSSL_CTX_new failed");
		return -1;
	}

	/* TLS 1.2 + 1.3 only (NO_OLD_TLS already excludes 1.0/1.1). */
	(void)wolfSSL_CTX_SetMinVersion(s_ctx, WOLFSSL_TLSV1_2);

	wolfSSL_SetIORecv(s_ctx, net_recv);
	wolfSSL_SetIOSend(s_ctx, net_send);

	if (wolfSSL_CTX_use_certificate_buffer(s_ctx, tlscert_get_cert(),
					       (long)tlscert_get_cert_len(),
					       WOLFSSL_FILETYPE_ASN1) !=
	    WOLFSSL_SUCCESS) {
		LOG_ERR("use_certificate_buffer failed");
		wolfSSL_CTX_free(s_ctx);
		s_ctx = NULL;
		return -1;
	}

	if (wolfSSL_CTX_use_PrivateKey_buffer(s_ctx, tlscert_get_key(),
					      (long)tlscert_get_key_len(),
					      WOLFSSL_FILETYPE_ASN1) !=
	    WOLFSSL_SUCCESS) {
		LOG_ERR("use_PrivateKey_buffer failed");
		wolfSSL_CTX_free(s_ctx);
		s_ctx = NULL;
		return -1;
	}

	(void)k_thread_create(&httpsd_thread, httpsd_stack,
			      K_THREAD_STACK_SIZEOF(httpsd_stack),
			      httpsd_main, NULL, NULL, NULL,
			      HTTPSD_PRIO, 0, K_NO_WAIT);
	(void)k_thread_name_set(&httpsd_thread, "httpsd");

	LOG_INF("HTTPS server started");
	return 0;
}
