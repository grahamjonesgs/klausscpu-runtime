# Handover — `klausscpu-runtime` standalone repo (Phase 3+)

**You are a fresh Claude session running inside the newly-cloned
`klausscpu-runtime` repo.** This is the software layer that was just split out of
the KlaussCPU LLVM fork. Your job: finish making it build standalone, then help
cut over. Read `REPO_SPLIT_PLAN.md` (in this repo root) first — it is the master
plan; this doc is the operational handover for what's left.

---

## Where things stand

- **Phase 0–1 are DONE.** This repo was extracted from the fork with full history
  (85 commits) via `git subtree split` of the old
  `llvm/lib/Target/KlaussCPU/runtime/` subtree, hoisted to the repo root, and
  pushed to `main`. The dangling `zephyr-ws/modules/fs/fatfs` submodule gitlink
  was removed and `/modules/` gitignored (it's a west-fetched module, like
  `/zephyr/`).
- **Two repos now:**
  | Repo | Local path (typical) | Role |
  |---|---|---|
  | `klausscpu-llvm` (the fork) | `~/Documents/src/llvm-project` | Builds the toolchain (`clang`, `ld.lld`, `llc`, `llvm-objcopy`…). **Still contains a copy of `runtime/` — to be removed in Phase 2.** |
  | `klausscpu-runtime` (this) | wherever you cloned it | The software. Depends on a **built** toolchain via the `KLAUSSCPU_LLVM_BIN` env var — **not** a submodule. |
- **The contract between them is one env var:** `KLAUSSCPU_LLVM_BIN` →
  `<klausscpu-llvm>/build/bin`. No submodule. Document it; don't embed the 3.6 GB
  fork.

## Prerequisites the user already has

- The LLVM fork is built; `build/bin/` has `clang`, `ld.lld`, `llc`,
  `llvm-objcopy`, `llvm-objdump`, `llvm-readelf`, `llvm-ar`, `llvm-nm`,
  `llvm-ranlib`, `llvm-strip`, `llvm-size`, and `libclang_rt.builtins.a` in the
  clang resource dir (built by `build-builtins.sh`).
- Vendored upstreams are **fetched, not committed** (all gitignored): picolibc
  (`build-picolibc.sh`), lwIP (`get-lwip.sh`), FreeRTOS-Kernel
  (`freertos/get-freertos.sh`), wolfSSL/wolfSSH (`freertos/wolfssl/build-*.sh`),
  Zephyr + west modules (`west init`/`west update` in `zephyr-ws/`). On a truly
  fresh clone these must be re-fetched before a build.

---

## Phase 3 — de-hardcode toolchain paths (YOUR MAIN TASK)

Set `export KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin` and make every build
entry point default `BUILD_DIR`/`LLVM_BIN` from it. **The root problem:** paths
were calibrated to the runtime living *inside* the fork at
`llvm/lib/Target/KlaussCPU/runtime/`. At this repo's root they break two ways —
`git rev-parse --show-toplevel` now returns *this* repo (which has no `/build`),
and the relative `../../../..` depth counts now climb above the repo.

**Re-run the audit to get current line numbers** (paths drift):
```sh
grep -rInE "BUILD_DIR|build/bin|show-toplevel|\.\./\.\./\.\." \
  --include=Makefile --include=*.mk --include=*.sh --include=*.cmake .
```

Known breakages to fix (verify line numbers against the grep above):

1. **`Makefile`** (root) — `BUILD_DIR ?= $(shell git rev-parse --show-toplevel)/build`.
   After the split this resolves to *this* repo's root + `/build` (nonexistent).
   → `BUILD_DIR ?= $(KLAUSSCPU_LLVM_BIN:/bin=)` or, cleaner:
   `LLVM_BIN ?= $(KLAUSSCPU_LLVM_BIN)` and derive `CC/LLC/LLD` from `$(LLVM_BIN)`.
   Fail loudly with a helpful message if `KLAUSSCPU_LLVM_BIN` is unset.

2. **`build-picolibc.sh`** — `BUILD_DIR="$(git -C ../../../.. rev-parse --show-toplevel)/build"`.
   Both the `../../../..` and the toplevel assumption are wrong now.
   → `LLVM_BIN="${KLAUSSCPU_LLVM_BIN:?set KLAUSSCPU_LLVM_BIN}"`.

3. **`freertos/Makefile`** — `BUILD_DIR := ../../../../../../build` (6×`../`,
   calibrated to the old nesting). At `<root>/freertos/` this is nonsense.
   → derive from `KLAUSSCPU_LLVM_BIN`.

4. **`build-builtins.sh`** — already honours `$KLAUSSCPU_LLVM_BIN` but falls back
   to `build/bin`; make the fallback fail loudly instead of silently using a
   wrong path.

5. **`freertos/wolfssl/build-wolfssl.sh` / `build-wolfssh.sh`** — their own
   `BUILD_DIR` (`wolfssl-build`, etc.) is repo-local and fine, **but** check the
   `--toolchain`/`CMAKE_TOOLCHAIN_FILE` they pass; if it points at the fork, route
   it through `KLAUSSCPU_LLVM_BIN`.

6. **Zephyr toolchain** — `zephyr-ws/klausscpu-zephyr/cmake/toolchain/klausscpu-clang/generic.cmake`
   already uses `KLAUSSCPU_LLVM_BIN` ✓. Confirm the `west build -D…` invocations
   in the README / any helper scripts pass it through (grep `west build`).

After each fix, the smoke test is: with `KLAUSSCPU_LLVM_BIN` exported, the build
finds `clang`/`ld.lld` and produces a `.elf`.

---

## Phase 3.5 — docs & contract

- Add a top-level `README.md` to this repo: "Requires the `klausscpu-llvm` fork
  built; `export KLAUSSCPU_LLVM_BIN=<fork>/build/bin`." List the fetch scripts
  (picolibc/lwip/freertos/wolfssl/west) needed on a fresh clone. Pin the expected
  fork commit/branch.
- The backend dev notes (`CLAUDE.md`, `FPGA_FIXES_HISTORY.md`, `RTOS_NOTES.md`)
  stay in the fork. If useful, create a runtime-focused `CLAUDE.md` here covering
  just the software layer.

## Phase 5 — verify standalone (do BEFORE Phase 2)

Build against the toolchain and confirm both still build:
- `zephyr.elf` for the `ssh_shell` app (`zephyr-ws/klausscpu-zephyr/apps/ssh_shell`).
- A FreeRTOS image (`freertos/`, `make`).

Only once these pass is the new repo proven to stand alone.

## Phase 2 — remove `runtime/` from the fork (LAST, in the OTHER repo)

This happens in `klausscpu-llvm`, not here, and only after Phase 5 passes:
```sh
cd <klausscpu-llvm>
git rm -r llvm/lib/Target/KlaussCPU/runtime
git commit -m "Move runtime/ to the klausscpu-runtime repo"
```
Do **not** rewrite the fork's history to purge `runtime/` — `.git` stays
LLVM-sized regardless, and rewriting breaks clean rebases onto upstream LLVM.
Also update the fork's `CLAUDE.md` "Systems built on this backend" section to
point at this repo, and update the user's Claude memory index paths (Phase 6).

---

## Useful facts / gotchas

- This repo's history starts at `Step 21: linker script + crt0` (the first commit
  that touched `runtime/`) and runs through the latest `webapi` work — 85 commits.
- `picolibc` uses 64-bit `long`, so printf is `%lu` not `%llu` (picolibc lacks the
  `ll` modifier).
- lwIP checksum: `LWIP_CHKSUM_ALGORITHM=1` in `lwipopts.h` (LDIDX16 reads BE).
- The top-level `fatfs/` entry here is the **FreeRTOS** FatFs glue (tracked) — NOT
  the west-managed `zephyr-ws/modules/fs/fatfs` (untracked/ignored). Don't confuse
  them.
- **This `HANDOVER.md` and `REPO_SPLIT_PLAN.md` can be deleted from the repo once
  the split is fully cut over** — they're transitional.
