/* psa_spike — Zephyr's SSH server on mbedTLS / PSA on KlaussCPU, and a
 * benchmark of the PSA operations SSH and TLS use (ZEPHYR_4_5_PLAN.md step 6).
 *
 * Boot: DHCP, an RSA-2048 host key (timed), then the SSH server on :22 with
 * user admin / password klausscpu and a shell.  `psabench` (UART or SSH)
 * times each operation in software, or through a PSA driver once one exists.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/ssh/keygen.h>
#include <zephyr/net/ssh/server.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_ssh.h>
#include <psa/crypto.h>

LOG_MODULE_REGISTER(psa_spike, LOG_LEVEL_INF);

#include "rsa_test_key.h"

/* Results go to the shell that ran psabench, or to printk at boot. */
static const struct shell *bench_sh;
#define P(...)                                                                     \
	do {                                                                       \
		if (bench_sh) {                                                    \
			shell_print(bench_sh, __VA_ARGS__);                        \
		} else {                                                           \
			printk(__VA_ARGS__);                                       \
			printk("\n");                                              \
		}                                                                  \
	} while (0)

#define HOST_KEY_INDEX 0

static struct net_mgmt_event_callback dhcp_cb;
static struct k_sem dhcp_sem;

static void dhcp_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			 struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (mgmt_event == NET_EVENT_IPV4_DHCP_BOUND) {
		k_sem_give(&dhcp_sem);
	}
}

static int wait_for_dhcp(void)
{
	struct net_if *iface = net_if_get_default();

	k_sem_init(&dhcp_sem, 0, 1);
	net_mgmt_init_event_callback(&dhcp_cb, dhcp_handler, NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&dhcp_cb);
	if (!iface) {
		return -ENODEV;
	}
	net_dhcpv4_start(iface);
	return k_sem_take(&dhcp_sem, K_SECONDS(15));
}

/* ── benchmark ──────────────────────────────────────────────────────────── */

#define BUF 16384
static uint8_t in_buf[BUF], out_buf[BUF + 64];

static uint64_t us_since(uint64_t t0)
{
	return (k_cycle_get_64() - t0) / (sys_clock_hw_cycles_per_sec() / 1000000u);
}

static unsigned long kibps(size_t bytes, uint64_t us)
{
	return us ? (unsigned long)((uint64_t)bytes * 1000000u / us / 1024u) : 0;
}

static psa_key_id_t import_sym(psa_key_type_t type, psa_algorithm_t alg,
			       psa_key_usage_t usage, size_t bytes)
{
	psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
	uint8_t key[32];
	psa_key_id_t id = 0;

	psa_generate_random(key, bytes);
	psa_set_key_type(&a, type);
	psa_set_key_algorithm(&a, alg);
	psa_set_key_usage_flags(&a, usage);
	psa_set_key_bits(&a, bytes * 8);
	if (psa_import_key(&a, key, bytes, &id) != PSA_SUCCESS) {
		return 0;
	}
	return id;
}

static psa_key_id_t gen_asym(psa_key_type_t type, size_t bits, psa_algorithm_t alg,
			     psa_key_usage_t usage, uint64_t *us)
{
	psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	uint64_t t0 = k_cycle_get_64();

	psa_set_key_type(&a, type);
	psa_set_key_bits(&a, bits);
	psa_set_key_algorithm(&a, alg);
	psa_set_key_usage_flags(&a, usage);
	if (psa_generate_key(&a, &id) != PSA_SUCCESS) {
		return 0;
	}
	*us = us_since(t0);
	return id;
}

/* Known answers through the PSA API, whichever driver serves them. */
static int check_eq(const char *name, const uint8_t *got, const char *want_hex)
{
	uint8_t want[32];

	for (int i = 0; i < 32; i++) {
		unsigned int b;
		char t[3] = {want_hex[2 * i], want_hex[2 * i + 1], 0};

		b = (unsigned int)strtoul(t, NULL, 16);
		want[i] = (uint8_t)b;
	}
	bool ok = memcmp(got, want, 32) == 0;

	P("%s  %s", ok ? "PASS" : "FAIL", name);
	return ok ? 0 : 1;
}

static int psacheck(void)
{
	static uint8_t a_chunk[1000];
	uint8_t d[32], d2[32];
	size_t olen;
	int fails = 0;
	psa_hash_operation_t op = PSA_HASH_OPERATION_INIT, op2 = PSA_HASH_OPERATION_INIT;

	psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)"abc", 3, d, sizeof(d), &olen);
	fails += check_eq("SHA-256(\"abc\")", d,
			  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	/* 1,000,000 x 'a' in chunks of 1..1000 bytes, interleaved with a
	 * second operation hashing "abc". */
	memset(a_chunk, 'a', sizeof(a_chunk));
	psa_hash_setup(&op, PSA_ALG_SHA_256);
	psa_hash_setup(&op2, PSA_ALG_SHA_256);
	psa_hash_update(&op2, (const uint8_t *)"a", 1);
	size_t done = 0, n = 1;

	while (done < 1000000) {
		size_t take = MIN(n, 1000000 - done);

		psa_hash_update(&op, a_chunk, take);
		done += take;
		n = n % 963 + 37;   /* 37..999: within a_chunk */
	}
	psa_hash_update(&op2, (const uint8_t *)"bc", 2);
	psa_hash_finish(&op, d, sizeof(d), &olen);
	psa_hash_finish(&op2, d2, sizeof(d2), &olen);
	fails += check_eq("SHA-256(10^6 x 'a'), odd chunks", d,
			  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	fails += check_eq("SHA-256 interleaved with it", d2,
			  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	/* RFC 4231 test cases 1 and 2. */
	static const struct { uint8_t key[20]; size_t klen; const char *msg; const char *mac; } tc[] = {
		{ {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
		   0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b}, 20, "Hi There",
		  "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7" },
		{ {'J', 'e', 'f', 'e'}, 4, "what do ya want for nothing?",
		  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843" },
	};
	for (int t = 0; t < 2; t++) {
		psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
		psa_key_id_t k;
		char name[32];

		psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
		psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
		psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
		psa_import_key(&a, tc[t].key, tc[t].klen, &k);
		psa_mac_compute(k, PSA_ALG_HMAC(PSA_ALG_SHA_256), (const uint8_t *)tc[t].msg,
				strlen(tc[t].msg), d, sizeof(d), &olen);
		psa_destroy_key(k);
		snprintk(name, sizeof(name), "HMAC-SHA-256 RFC 4231 TC%d", t + 1);
		fails += check_eq(name, d, tc[t].mac);
	}
	/* AES-128-CTR, SP 800-38A F.5.1, through the multi-part API split at
	 * 7 / 50 / 7 bytes (a partial block's keystream carried over), then the
	 * one-shot decrypt of IV || ciphertext. */
	{
		static const uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
						0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
		static const uint8_t icb[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
						0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
		static const char *pt_hex = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
					    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
		static const char *ct_hex = "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
					    "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee";
		uint8_t pt[64], ct[64], buf[80];
		psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
		psa_cipher_operation_t cop = PSA_CIPHER_OPERATION_INIT;
		psa_key_id_t k;
		size_t o1 = 0, o2 = 0, o3 = 0, o4 = 0;

		for (int i = 0; i < 64; i++) {
			char t1[3] = {pt_hex[2 * i], pt_hex[2 * i + 1], 0};
			char t2[3] = {ct_hex[2 * i], ct_hex[2 * i + 1], 0};

			pt[i] = (uint8_t)strtoul(t1, NULL, 16);
			ct[i] = (uint8_t)strtoul(t2, NULL, 16);
		}
		psa_set_key_type(&a, PSA_KEY_TYPE_AES);
		psa_set_key_algorithm(&a, PSA_ALG_CTR);
		psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
		psa_import_key(&a, key, 16, &k);
		psa_cipher_encrypt_setup(&cop, k, PSA_ALG_CTR);
		psa_cipher_set_iv(&cop, icb, 16);
		psa_cipher_update(&cop, pt, 7, buf, sizeof(buf), &o1);
		psa_cipher_update(&cop, pt + 7, 50, buf + o1, sizeof(buf) - o1, &o2);
		psa_cipher_update(&cop, pt + 57, 7, buf + o1 + o2, sizeof(buf) - o1 - o2, &o3);
		psa_cipher_finish(&cop, buf + o1 + o2 + o3, sizeof(buf), &o4);
		bool ok = o1 + o2 + o3 + o4 == 64 && memcmp(buf, ct, 64) == 0;

		P("%s  AES-128-CTR F.5.1, multi-part 7/50/7", ok ? "PASS" : "FAIL");
		fails += !ok;
		memcpy(buf, icb, 16);
		memcpy(buf + 16, ct, 64);
		uint8_t back[64];

		psa_cipher_decrypt(k, PSA_ALG_CTR, buf, 80, back, sizeof(back), &o1);
		ok = o1 == 64 && memcmp(back, pt, 64) == 0;
		P("%s  AES-128-CTR F.5.1, one-shot decrypt", ok ? "PASS" : "FAIL");
		fails += !ok;
		psa_destroy_key(k);
	}
	P("psacheck: %d failure(s)", fails);
	return fails;
}

static void psabench(bool asym, bool keygen)
{
	uint8_t hash[32], sig[512], pub[160], secret[64], tag[16], iv[12];
	size_t olen;
	uint64_t t0, us;
	psa_status_t st;
	psa_key_id_t k;
	const int reps = 4;

	psa_generate_random(in_buf, BUF);
	psa_generate_random(hash, sizeof(hash));

	/* Bulk: SSH's only cipher (aes128-ctr + hmac-sha2-256) and TLS 1.3's GCM. */
	k = import_sym(PSA_KEY_TYPE_AES, PSA_ALG_CTR, PSA_KEY_USAGE_ENCRYPT, 16);
	t0 = k_cycle_get_64();
	for (int i = 0; i < reps; i++) {
		st = psa_cipher_encrypt(k, PSA_ALG_CTR, in_buf, BUF, out_buf, sizeof(out_buf),
					&olen);
	}
	us = us_since(t0);
	P("AES-128-CTR      16 KiB  %6lu KiB/s  (status %d)",
		    kibps(BUF * reps, us), st);
	psa_destroy_key(k);

	k = import_sym(PSA_KEY_TYPE_AES, PSA_ALG_GCM, PSA_KEY_USAGE_ENCRYPT, 16);
	psa_generate_random(iv, sizeof(iv));
	t0 = k_cycle_get_64();
	for (int i = 0; i < reps; i++) {
		st = psa_aead_encrypt(k, PSA_ALG_GCM, iv, sizeof(iv), NULL, 0, in_buf, BUF,
				      out_buf, sizeof(out_buf), &olen);
	}
	us = us_since(t0);
	P("AES-128-GCM      16 KiB  %6lu KiB/s  (status %d)",
		    kibps(BUF * reps, us), st);
	psa_destroy_key(k);

	t0 = k_cycle_get_64();
	for (int i = 0; i < reps; i++) {
		st = psa_hash_compute(PSA_ALG_SHA_256, in_buf, BUF, hash, sizeof(hash), &olen);
	}
	us = us_since(t0);
	P("SHA-256          16 KiB  %6lu KiB/s  (status %d)",
		    kibps(BUF * reps, us), st);

	k = import_sym(PSA_KEY_TYPE_HMAC, PSA_ALG_HMAC(PSA_ALG_SHA_256),
		       PSA_KEY_USAGE_SIGN_MESSAGE, 32);
	t0 = k_cycle_get_64();
	for (int i = 0; i < reps; i++) {
		st = psa_mac_compute(k, PSA_ALG_HMAC(PSA_ALG_SHA_256), in_buf, BUF, tag,
				     sizeof(hash), &olen);
	}
	us = us_since(t0);
	P("HMAC-SHA-256     16 KiB  %6lu KiB/s  (status %d)",
		    kibps(BUF * reps, us), st);
	t0 = k_cycle_get_64();
	for (int i = 0; i < 64; i++) {
		st = psa_mac_compute(k, PSA_ALG_HMAC(PSA_ALG_SHA_256), in_buf, 64, out_buf,
				     32, &olen);
	}
	us = us_since(t0);
	P("HMAC-SHA-256     64 B    %6lu us each", (unsigned long)(us / 64));
	psa_destroy_key(k);

	if (!asym) {
		return;
	}

	/* Key exchange: curve25519-sha256 (SSH), x25519 (TLS 1.3). */
	k = gen_asym(PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY), 255, PSA_ALG_ECDH,
		     PSA_KEY_USAGE_DERIVE, &us);
	P("X25519 keygen            %6lu us", (unsigned long)us);
	psa_export_public_key(k, pub, sizeof(pub), &olen);
	t0 = k_cycle_get_64();
	st = psa_raw_key_agreement(PSA_ALG_ECDH, k, pub, olen, secret, sizeof(secret), &olen);
	P("X25519 agreement         %6lu us  (status %d)",
		    (unsigned long)us_since(t0), st);
	psa_destroy_key(k);

	/* Host keys: rsa-sha2-256 (all Zephyr's SSH offers), ECDSA P-256 (TLS
	 * cert, and the alternative SSH host key). */
	k = gen_asym(PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1), 256,
		     PSA_ALG_ECDSA(PSA_ALG_SHA_256),
		     PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH, &us);
	P("ECDSA P-256 keygen       %6lu us", (unsigned long)us);
	t0 = k_cycle_get_64();
	st = psa_sign_hash(k, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash), sig,
			   sizeof(sig), &olen);
	P("ECDSA P-256 sign         %6lu us  (status %d)",
		    (unsigned long)us_since(t0), st);
	t0 = k_cycle_get_64();
	st = psa_verify_hash(k, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash), sig, olen);
	P("ECDSA P-256 verify       %6lu us  (status %d)",
		    (unsigned long)us_since(t0), st);
	psa_destroy_key(k);

	/* RSA-2048 signing on a fixed test key (rsa_test_key.h): the
	 * rsa-sha2-256 host key signature every Zephyr SSH handshake makes. */
	{
		psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;

		psa_set_key_type(&a, PSA_KEY_TYPE_RSA_KEY_PAIR);
		psa_set_key_algorithm(&a, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256));
		psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
		st = psa_import_key(&a, rsa_test_key, sizeof(rsa_test_key), &k);
		if (st != PSA_SUCCESS) {
			P("RSA-2048 import failed (status %d)", st);
			return;
		}
	}
	t0 = k_cycle_get_64();
	st = psa_sign_hash(k, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256), hash, sizeof(hash),
			   sig, sizeof(sig), &olen);
	P("RSA-2048 sign            %6lu us  (status %d)", (unsigned long)us_since(t0), st);
	t0 = k_cycle_get_64();
	st = psa_verify_hash(k, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256), hash, sizeof(hash),
			     sig, olen);
	P("RSA-2048 verify          %6lu us  (status %d)", (unsigned long)us_since(t0), st);
	psa_destroy_key(k);

	if (keygen) {
		k = gen_asym(PSA_KEY_TYPE_RSA_KEY_PAIR, 2048,
			     PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256), PSA_KEY_USAGE_SIGN_HASH,
			     &us);
		P("RSA-2048 keygen          %6lu ms", (unsigned long)(us / 1000));
		psa_destroy_key(k);
	}
}

static int cmd_psabench(const struct shell *sh, size_t argc, char **argv)
{
	bench_sh = sh;
	psabench(argc < 2 || strcmp(argv[1], "sym") != 0,
		 argc >= 2 && strcmp(argv[1], "keygen") == 0);
	bench_sh = NULL;
	return 0;
}

static int cmd_psacheck(const struct shell *sh, size_t argc, char **argv)
{
	bench_sh = sh;
	psacheck();
	bench_sh = NULL;
	return 0;
}

SHELL_CMD_REGISTER(psacheck, NULL, "Known-answer tests through the PSA API", cmd_psacheck);

SHELL_CMD_ARG_REGISTER(psabench, NULL,
		       "Time PSA operations (sym: symmetric only; keygen: add RSA-2048 keygen)",
		       cmd_psabench, 1, 1);

int main(void)
{
	static struct net_sockaddr_in bind_addr;
	uint64_t t0;
	int ret;

	printk("\nKlaussCPU psa_spike: Zephyr SSH on mbedTLS\n");
	ret = psa_crypto_init();
	if (ret != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init: %d", ret);
	}
	if (IS_ENABLED(CONFIG_PSA_SPIKE_BOOT_BENCH)) {
		psacheck();
		psabench(true, false);
		printk("psabench done\n");
		k_sleep(K_FOREVER);
	}
	if (wait_for_dhcp() != 0) {
		LOG_WRN("no DHCP lease");
	}

	t0 = k_cycle_get_64();
	ret = ssh_keygen(HOST_KEY_INDEX, SSH_HOST_KEY_TYPE_RSA, 2048);
	LOG_INF("host key: RSA-2048 generated in %lu ms (%d)",
		(unsigned long)(us_since(t0) / 1000), ret);

	bind_addr.sin_family = NET_AF_INET;
	bind_addr.sin_port = net_htons(22);
	ret = ssh_server_start(ssh_server_instance(0), (struct net_sockaddr *)&bind_addr,
			       HOST_KEY_INDEX, "admin", "klausscpu", NULL, 0,
			       shell_sshd_event_callback,
			       shell_sshd_transport_event_callback, NULL);
	LOG_INF("SSH listening on port 22 (%d)", ret);

	/* Never return from main on this arch (thread exit jumps to PC 0). */
	k_sleep(K_FOREVER);
	return 0;
}
