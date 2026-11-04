# Zephyr 3.7 → 4.5 migration plan

Status: started 2026-11-03 on branch `zephyr-4.5` (git worktree
`../klausscpu-runtime-zephyr45`).  Work happens on that branch in a
second west workspace, so the 3.7 tree keeps building and validating on `main`
until the branch merges.

Upstream facts below were checked against a v4.5.0 checkout on 2026-11-02/03.
Recheck them before starting, and rebase onto v4.6.0 (LTS4, due April 2027)
once it ships.  Zephyr froze its stable APIs at 4.4 for that LTS, so the
4.5 → 4.6 step should be small.

## 1. The crypto decision (read first)

Zephyr 4.5 does not force mbedTLS on us.  wolfSSL and wolfSSH are external
modules that we build ourselves (`freertos/wolfssl/build-*.sh`) and could stay.
What 4.5 offers is its own SSH server and client (`subsys/net/lib/ssh`) and TLS
sockets, and both sit on mbedTLS 4 / TF-PSA-Crypto.  The plan moves to those,
which leaves one crypto stack and one hardware driver.  These are the gaps that
move creates:

| | Today (wolfSSH / wolfSSL) | Zephyr 4.5 native (mbedTLS / PSA) |
|---|---|---|
| SSH ciphers | aes128/256-gcm, aes128/256-ctr | **aes128-ctr + hmac-sha2-256 only** (chacha20 is commented out) |
| SSH host key | ECDSA P-256 (121-byte DER on SD) | **RSA only** (rsa-sha2-256/512) |
| SSH auth | password | password, publickey |
| SFTP | yes (wolfSSH SFTP, root `/SD:`) | **none**.  The `subsystem` request is parsed, but nothing implements SFTP. |
| Shell over SSH | custom transport (`ssh_shell_transport.c`) | `SHELL_BACKEND_SSH` built in |
| TLS (:443) | wolfSSL API in `httpsd.c` | TLS sockets with credentials from `tls_credential_add` |
| Hardware crypto hook | wolfCrypt callback (`ssh/wolfssl_hw.c`) | PSA accelerator driver (`TF_PSA_CRYPTO_DRIVER_CONFIG_FILE` + `TF_PSA_CRYPTO_DISPATCH_DIR`; STM32 is the in-tree example) |
| Randomness | TRNG seeds wolfSSL directly | **needs an entropy driver**.  Without one, Kconfig quietly selects `TEST_RANDOM_GENERATOR`, which is not secure. |

Consequences:

- **SFTP goes away** unless we write an SFTP subsystem (SFTP v3, a sizeable job
  that upstream would probably take).  The tooling doesn't need it:
  `tools/validate/zephyr/llext_sd.py` already uploads over HTTP `PUT`.  Proposal:
  drop SFTP and use HTTP(S) `PUT`/`GET` for files.
- **RSA host key.** Every SSH handshake does an RSA-2048 private-key operation in
  software.  Measure it in the spike (step 6).  If it's too slow, the options are
  adding `ecdsa-sha2-nistp256` host keys to Zephyr's SSH (upstreamable, since
  PSA already does ECDSA P-256) or hardware help (section 4).
- **The SSH hot path becomes AES-CTR plus HMAC-SHA-256 per packet.** The AES
  hardware still helps with CTR.  The SHA/HMAC hardware only helps once the SHA
  block can save and restore its state (section 4).  The GHASH unit then only
  serves TLS.

## 2. Already done on main

- The wolfSSL hardware path was fixed and verified on the board (commit
  "Zephyr: route wolfSSL's AES-128 CTR/GCM to the crypto block").  It stays in
  use until step 7 removes wolfSSL.
- The findings that feed this plan (crypto benchmarks, cipher negotiation, the
  SFTP stall) are in that commit message.

## 3. Steps

Each step ends with something that builds and runs, so the branch can stop at
any of them.  Validate with `ZEPHYR_WS=<4.5 workspace> tools/validate/validate.sh zephyr`
once the apps build; until then use `zephyr/build.sh hello` and the emulator.

### Step 1: workspace and hardware model v2 (blocks everything else)
- New workspace `zephyr-ws-4.5/` (gitignored like `zephyr-ws/zephyr`).  Point
  `klausscpu-zephyr/west.yml` at `v4.5.0`; the allowlist needs `fatfs`, `lvgl`,
  `mbedtls`, `tf-psa-crypto` (plus `tf-psa-crypto-drivers` if step 8 uses it).
- Hardware model v2 (v1 was removed in 4.2):
  - add `arch/archs.yml`;
  - add `soc/klausscpu/soc.yml` and a v2 `Kconfig.soc`;
  - add `boards/klausscpu/nexys_a7/board.yml` (with `full_name`) and `Kconfig.nexys_a7`;
  - delete `arch/Kconfig.v1` and `Kconfig.board`;
  - fix the comments in the top-level `CMakeLists.txt` that mention `arch_v1`/`soc_v1`.
- Add `arch_root`, `board_root`, `soc_root` and `dts_root` to `zephyr/module.yml`.
  That removes the four `-D*_ROOT` flags from `build.sh` and CLAUDE.md.
- Kconfig: drop `select REQUIRES_STD_C11` (the symbol is gone; C17 is the
  default, which keeps `BUILD_ASSERT` working).  Set `CONFIG_MINIMAL_LIBC=y` in
  the defconfig, because 4.x defaults to picolibc where it can.
- Toolchain cmake: 4.5 has no `cmake/bintools/llvm/generic.cmake` (our include
  is `OPTIONAL`).  Re-check `cmake/toolchain/klausscpu-clang/*` against 4.x.
- Rebase `zephyr-patches/klausscpu-core.patch`.  The four header hunks only
  conflict on surrounding lines; drop the FatFs hunk (step 4 replaces it).
  `cbprintf-klausscpu.patch` applies unchanged.
- Done when: `hello` and `dbg_synchronization` boot on the emulator and the board.

### Step 2: kernel and arch interface
- Let the compile errors drive the work in `thread.c`, `swap.S`, `irq.c`,
  `fatal.c`, `offsets.c` (for example `z_arch_esf_t` → `struct arch_esf`) against
  the 4.0–4.5 migration guides.
- Done when: `irq_soak`, `irq_dev`, `irq_dev_spurious`, `tick_cost` and both
  negative builds pass, and perf is within baseline.

### Step 3: LLEXT
- Rebase `llext-klausscpu.patch`.  Upstream now has general RELA support, the
  shared string table, the SHF_MERGE/SHF_STRINGS mask and region alignment, so drop those.
  Keep `LLEXT_ELF_CLASS32`, `keep_symtab` and `arch_elf_check_header`.  Diff
  upstream's alignment code against ours, including the misaligned-section error.
- Rewrite `arch_elf_relocate()` in `arch/klausscpu/core/elf.c` for the new
  signature `(ldr, ext, rel, shdr)`.
- Optional: `fs_loader` (load `.llext` straight from the SD card instead of a
  `k_malloc` copy) and `CONFIG_LLEXT_HEAP_DYNAMIC`.
- Done when: `llext_demo` passes and `llext_sd.sh` passes against the bare-metal baselines.

### Step 4: drivers and configuration
- Ethernet: in 4.5 the `ethernet_api` callbacks take a `struct net_if *` (this
  includes `get_capabilities`).
- Re-check the UART, timer and disk drivers against the migration guides.
- FatFs: `CONFIG_FS_FATFS_HAS_RTC=y` replaces the `FF_FS_NORTC` patch hunk.
- ssh_shell `prj.conf`: remove `CONFIG_NET_SOCKETS_POLL_MAX` and
  `CONFIG_NET_SOCKETS_POSIX_NAMES` (both gone).  Fix any other Kconfig the build rejects.
- Done when: ssh_shell (still wolfSSH/wolfSSL) builds, boots, serves SSH,
  HTTP(S) and VNC.  Whether wolfSSL builds as-is on 4.5 is part of this step;
  if it needs real porting, skip ahead to step 7 instead.

### Step 5: TRNG entropy driver
- `drivers/entropy/entropy_klausscpu.c` (Zephyr entropy API), a `klausscpu,trng`
  binding, a DT node, and `chosen { zephyr,entropy = &trng; }`.  Reuse the
  bare-metal init from `src/crypto_hw.c`: drain stale FIFO words, reseed, wait for
  READY.
- `get_entropy` returns `-EIO` unless `HEALTH_OK` is set.  The current wolfSSL
  seed also accepts READY alone, which is too weak.
- Add a build check that `CONFIG_TEST_RANDOM_GENERATOR` is not set for ssh_shell
  (add a line to `expect-*.txt`, or a `BUILD_ASSERT`).
- Done when: the net stack and mbedTLS take their randomness from the TRNG, and a
  shell `entropy` test reads varying data.

### Step 6: spike for the mbedTLS stack (measure before committing to it)
On a throwaway app built from Zephyr's SSH sample with `SHELL_BACKEND_SSH`, measure:
- the SSH handshake time with an RSA-2048 host key (sign + curve25519);
- aes128-ctr + hmac-sha2-256 throughput in software;
- TLS 1.3 handshake time and AES-128-GCM throughput through TLS sockets;
- RAM and flash size against the wolfSSL build.

Decision gate: continue with step 7, or keep wolfSSL on 4.5 and revisit at 4.6.

### Step 7: move SSH and HTTPS to Zephyr's stacks (deferred, see 3b)
- SSH: replace `ssh_server.c`, `ssh_shell_transport.c` and `sshkeys.c` with
  Zephyr's SSH server and `SHELL_BACKEND_SSH`.  Generate the RSA host key once,
  store it on the SD card (PSA export), and load it at boot.  Keep the `admin`
  password login.
- Port `llext_loader.c`'s output (it writes to `wolfSSH_stream_send`) and the
  `svc` commands to the shell API.
- HTTPS: `httpsd.c` moves to TLS sockets.  `tlscert.c` generates its self-signed
  ECDSA cert with PSA key generation plus mbedTLS `x509write`.  Keep `httpd.c`
  and `webapi.c`; they don't depend on the transport.
- Files: drop SFTP, and document HTTP `PUT`/`GET` for files (already used by `llext_sd.py`).
- Remove wolfSSL/wolfSSH: `ssh/wolfssl_hw.c`, `ssh/user_settings.h`, the
  `EXTRA_ZEPHYR_MODULES` entries in `build.sh` and CLAUDE.md, and the
  wolfSSL gotchas.  `freertos/` keeps its own copy.
- Update `expect-board.txt`.  The SSH banner changes, and ssh_shell's session
  checks no longer depend on wolfSSH's prompt handling.

### Step 8: PSA accelerator driver for the crypto block
- A driver config header plus a dispatch directory, set from the SoC's Kconfig
  defaults (model: `soc/st/stm32/Kconfig.defconfig` and the STM32 HAL PSA drivers).
- AES-128 ECB, CTR and GCM on `src/crypto_hw.c`.  First speed up its GCM loop:
  it costs 88 cycles per byte against 45 for the old (wrong) loop, because of
  per-8-byte `memcpy` calls and block copies.
- SHA-256 and HMAC only once the hardware can restore state (section 4).
  Without that, PSA's multipart hash and MAC operations can't share one
  stateless block.
- Test: known-answer and random cross-checks against PSA software for every
  length and AAD case `hwcrypto test` covered (lengths 0–4096, AAD 0/13/16/20,
  split CTR calls), then end-to-end SSH and HTTPS transfers.

### Step 9: the remaining apps
- `gui_lvgl` and the VNC display path: LVGL 8 → 9 (`lv_disp_*` → `lv_display_*`,
  renamed Kconfig, `LV_Z_COLOR_24_BGR_TO_RGB` removed, colour-format changes).
- `doom`, `mandel`, `gui_demo`: rebuild, and fix whatever changed.
- Done when: `validate.sh zephyr` passes in full, and the perf baselines are updated.

### Step 10: documentation and merge
- README, CLAUDE.md, `west.yml` comments, the expected boot banner (v4.5.0), and
  the patch list.  Merge to main, then retire `zephyr-ws/` 3.7.

## 3b. Progress and step 6 results (2026-11-03)

Done on this branch (all pushed):
- **Steps 1-5:** hardware model v2, the kernel/arch interface, LLEXT, drivers
  and config.  Every app builds on 4.5; ssh_shell still runs on wolfSSL.
  The TRNG entropy driver passes Zephyr's entropy API tests on the board.
- **Validation (second full run):** `validate.sh zephyr` (12 apps, emulator
  and board, SSH, VNC grabs) passes once two things are done:
  - the ISR0 stack regex accepts padded numbers (now fixed);
  - the perf baselines move to the 4.5 figures.  LVGL 9's renderer is mixed
    against LVGL 8 (blend50 +12%, rounded +7%, text -29%; the six frames
    total -3%); IRQ throughput is +7.9% against 3.7.
- **Performance:** with timeslicing off as on 3.7, the tick and IRQ round
  trip match or beat 3.7.
- **Step 8 (the PSA driver):** SHA-256/HMAC and AES-128-CTR on the crypto
  block.
- **Compiler:** klausscpu-llvm 6d29d71 fixes the backend crash that
  mbedTLS's inline asm triggered.

Step 6 measurements (apps/psa_spike, board at 100 MHz, through PSA):

| | mbedTLS sw, 32-bit limbs | 64-bit limbs (patch) | + PSA driver | wolfSSL today |
|---|---|---|---|---|
| AES-128-CTR, 16 KiB | 680 KiB/s | 743 | **11440** | 932 (sw) / 3537 (hw) |
| SHA-256, 16 KiB | 867 KiB/s | 863 | **18577** | 1430 (sw) |
| HMAC-SHA-256, 64 B | 438 us | 440 | **81 us** | ~35 us (sw) |
| X25519 agreement | 209 ms | 147 | 135 | |
| ECDSA P-256 sign | 116 ms | 78 | 70 | |
| RSA-2048 sign | 3120 ms | 1099 | 1091 | |
| RSA-2048 keygen (first boot) | 76-373 s | | | |

Login, step by step (OpenSSH 10.2 -> psa_spike, connect to authenticated):

| | Login |
|---|---|
| mbedTLS as shipped, RSA-2048 host key | 4.2 s |
| + ECDSA P-256 host key (ssh-ecdsa-host-key.patch) | 0.96 s |
| + Everest X25519 agreement | ~0.85 s |
| + Everest X25519 public keys, cached host point | **0.45-0.77 s** |
| wolfSSH today | ~65 ms |

Reading:
- The PSA driver makes bulk crypto and the per-packet MAC fast.
- ECDSA host keys (now in ssh-ecdsa-host-key.patch, upstreamable) and the
  Everest changes cut a login from 4.2 s to about 0.5 s.  That is still
  roughly 7-10x wolfSSH.  What is left:
  - about 70 ms per message after the key exchange, not yet explained (no
    poll timeouts in the server loop; next to look at are TCP ACK
    behaviour and per-packet work);
  - the ECDSA signature, 69 ms;
  - X25519, 2 x 13 ms.
- A Montgomery-multiplier block in hardware would mainly help the signature.
- **Decided (2026-11-04): stay on wolfSSL/wolfSSH on 4.5.**  It is fast
  (~65 ms logins), works on 4.5 (the compat shims in ssh/compat and
  user_settings.h), uses the crypto block through ssh/wolfssl_hw.c, and has
  SFTP.  Zephyr documents wolfSSH as a supported external module.  Steps
  7-8's mbedTLS work stays on this branch, unused by ssh_shell:
  apps/psa_spike, drivers/crypto, ssh-ecdsa-host-key.patch and the
  TF-PSA-Crypto patch.
- **Revisit the move when Zephyr's SSH has:**
  - SFTP;
  - a fast host key (Ed25519 once TF-PSA-Crypto implements EdDSA, or
    ECDSA upstream, e.g. from ssh-ecdsa-host-key.patch);
  - logins close to wolfSSH's: the ~70 ms per message after key exchange
    is not explained yet.
  Recheck at Zephyr 4.6 (LTS4).

## 4. Hardware changes for this migration

Listed by how much they matter once the stack is mbedTLS + Zephyr SSH.

1. **SHA-256 state save and restore (high).**  Make `SHA_DIGEST0..3` writable
   (or add `SHA_STATE` load registers) and allow a message-length restore, so a
   driver can switch between hash operations.  Zephyr SSH MACs every packet with
   HMAC-SHA-256 (its only MAC), and PSA hashes and MACs are multipart operations
   that interleave.  Measured: hardware SHA-256 at 7.2 MB/s against 1.4 MB/s in
   software (5×), and 64-byte HMAC 5.6× faster.  None of that is reachable
   without this change.
2. **A CTR mode block (medium).**  Have the hardware hold and increment the
   128-bit big-endian counter: write data, read data, no counter writes.  Add a
   fused GCM step (CTR + GHASH from one write/read pair) for TLS.  Today each
   16-byte block costs about 1,400 cycles of register traffic, while the AES core
   itself needs 20.
3. **Public-key help (only if step 6 says so).**  A Montgomery modular multiplier
   for RSA-2048 signing (Zephyr's SSH host keys are RSA only) and for P-256 in
   TLS.  The cheaper alternative is ECDSA host keys in Zephyr's SSH (software).
   Today's wolfSSL handshake takes about 65 ms, so this is only needed if RSA
   turns out too slow.
4. **TRNG: no change needed.**  The block passes its self-test, and READY and
   HEALTH_OK are enough for an entropy driver.  Nice to have: a sticky
   health-failure bit, so a transient fault can't be missed between polls.
5. **Not needed:** AES-256 (clients and Zephyr SSH use AES-128), and ChaCha20
   (disabled in Zephyr SSH).

### Agreed with the klaussCPU RTL session (2026-11-03)
Bitstream 1 (SHA RESTORE/ZERO, AES CTR/INC32, capability registers) is built
and on QSPI as klaussCPU 27ff737; the reference is MMIO_MAP.md there.  The
runtime driver (`src/crypto_hw.c`) and `crypto_selftest2` support it in
runtime 9a34059.  The step 8 PSA driver builds on `aes_ctr_hw`,
`sha256_hw_r*` and `hmac_sha256_hw_r*`.  Bitstream 2 (GCM_FUSE, GCM_DEC,
LEN, writable GCM_TAG) waits until TLS GCM speed matters.

Everything is additive, so the old map and behaviour are unchanged when the new
registers are at their reset values.
- **SHA (0xF00B_0000):**
  - `SHA_HIN0..3` (0x0C0–0x0D8, RW), packed like `SHA_DIGEST`.
  - `SHA_CTRL[2]` RESTORE (H ← HIN) and `[3]` ZERO.  Write priority is
    INIT > RESTORE > START, and RESTORE|START in one write is allowed.
  - HMAC: the driver computes the ipad/opad midstates at setkey and keeps them
    per operation, so the legacy HMAC wrapper isn't used.
- **AES (0xF00A_0000):**
  - `AES_CTR0/1` (0x0C0/0x0C8, RW).  Byte 15 is the counter's least significant byte.
  - `AES_MODE` (0x0D0): `[0]` CTR (writing `AES_IN1` starts a block; OUT = E(CTR)
    XOR IN, then CTR++), `[3]` INC32.
  - In the second bitstream: `[1]` GCM_FUSE, `[2]` GCM_DEC, and `[12:8]` LEN,
    which masks only the GHASH input; OUT is never masked, so CTR keeps its
    leftover keystream.
- **Capability bits** advertise RESTORE / CTR / GCM_FUSE, so the software picks
  a path at run time.
- **First bitstream:** SHA RESTORE + CTR (the Zephyr SSH path).  GCM_FUSE (TLS)
  comes second.
- **TRNG:** unchanged.  HEALTH_OK is already sticky until RESEED.

## 5. Risks

- RSA host-key cost on this CPU (step 6 measures it).
- SFTP loss.  This is a user-visible feature change, not a regression in tooling.
- mbedTLS's software AES-CTR + HMAC speed until step 8 lands.
- LLEXT ABI: ssh_shell's exported symbols change when the SSH layer changes, so
  rebuild the card's extensions (`llext_sd.sh`).
- 4.5 → 4.6 churn is expected to be small (the API freeze for the LTS was at 4.4).
