# KlaussCPU build Makefile
#
# Usage:
#   make hello          — hello world (uart test)
#   make adventure      — text adventure game
#   make test_64bit     — KlaussCPU test suite (mixed widths, heap, recursion)
#   make expr           — recursive-descent expression evaluator
#   make bst            — binary search tree workout (heap stress)
#   make crypto         — CRC32 + SHA-256 + Base64 round-trip
#   make queens         — N-queens backtracker
#   make test_switch    — switch/case → BR_JT → JMPR_R dispatch test
#   make test_fp        — soft-FP test (links compiler-rt builtins)
#   make test_asm       — inline assembly test
#   make test_printf    — varargs / printf test
#   make test_atomic    — C atomics (src/atomic.c) incl. a timer-ISR stress test
#   make test_flags     — the flags each instruction writes vs the compiler's model
#   make test_divshadow — a divide right behind a fused branch (board only)
#   make test_unaligned — unaligned loads (MEMGET32, packed structs, packets)
#   make test_setjmp    — setjmp / longjmp (SP, callee-saved registers, stack args after longjmp)
#   make test_task_exit — RTOS tasks that return or call rtos_task_exit
#   make fs_demo        — FatFs + SD card demo (mount/list/read/write)
#   make coremark       — CoreMark (bench-build/<OPT>/), see benchmarks/README.md
#   make embench        — Embench-IoT suite (bench-build/<OPT>/embench/)
#   make all            — every .elf
#   make clean

# Every target here is built by an explicit rule.  Cancel make's built-in
# "build an executable from X" implicit rules (empty-recipe pattern rules);
# otherwise make tries to host-link/assemble stray files (e.g. crt0.o -> crt0,
# setjmp.S -> setjmp) and fails with "library 'System' not found".  (Setting
# --no-builtin-rules via MAKEFLAGS inside the makefile is too late under the
# GNU Make 3.81 shipped on macOS, so cancel the specific rules explicitly.)
%: %.o
%: %.c
%: %.cc
%: %.cpp
%: %.s
%: %.S
%.s: %.S

# ── Toolchain location ────────────────────────────────────────────────────────
# The KlaussCPU toolchain (clang/llc/ld.lld) is built in the separate
# klausscpu-llvm fork; this repo only holds the software.  Point
# KLAUSSCPU_LLVM_BIN at <klausscpu-llvm>/build/bin.  See README.md.
ifeq ($(strip $(KLAUSSCPU_LLVM_BIN)),)
$(error KLAUSSCPU_LLVM_BIN is not set. Export it to the built toolchain's bin dir, e.g. `export KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin`. See README.md)
endif
# Expand a leading `~`/`~/` to $(HOME).  make's $(abspath ...) and prerequisite
# matching do NOT perform shell tilde expansion, so a value like `~/.../bin`
# silently breaks the $(BUILTINS) derivation below and the crt-*.o rules fail
# with "No rule to make target 'crt-addsf3.o'".  (The clang/ld command words
# still work because the shell expands the leading ~ there.)
KLAUSSCPU_LLVM_BIN := $(patsubst ~,$(HOME),$(patsubst ~/%,$(HOME)/%,$(KLAUSSCPU_LLVM_BIN)))
LLVM_BIN ?= $(KLAUSSCPU_LLVM_BIN)

CC  = $(LLVM_BIN)/clang
LLC = $(LLVM_BIN)/llc
LLD = $(LLVM_BIN)/ld.lld

TARGET  = klausscpu-unknown-elf
TRIPLE  = $(TARGET)

# ── Repo-root location ────────────────────────────────────────────────────────
# This Makefile and the bare-metal program sources (programs/) live in
# baremetal/.  The shared runtime sources (src/), FatFs glue, linker script,
# picolibc install, lwIP and headers live one level up at the repo root, shared
# with the FreeRTOS and Zephyr builds.  Running make here keeps build artifacts
# (.o/.elf/.d) out of the repo root.  Program sources reach the root-level
# headers via ../../ includes (they sit two levels below the root).
ROOT := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))/..

# ── Source search paths ───────────────────────────────────────────────────────
# Runtime library sources live in ../src, program sources in programs/ (local),
# FatFs vendor sources in ../fatfs.
#
# Use per-extension `vpath` for SOURCES only — never a blanket `VPATH`.  A blanket
# VPATH also searches these dirs for object files, so a stale src/foo.o left by the
# freertos build (which compiles src/ sources in place) gets found and shadows the
# local build, breaking the link with "cannot open foo.o".  Restricting to source
# extensions means objects are always built/linked here in baremetal/.
vpath %.c $(ROOT)/src:programs:$(ROOT)/fatfs
vpath %.S $(ROOT)/src
vpath %.s $(ROOT)/src

# ── picolibc install path ─────────────────────────────────────────────────────
# Run ../build-picolibc.sh once to populate this directory.
PICOLIBC ?= $(ROOT)/picolibc-install

# ── Compiler flags ────────────────────────────────────────────────────────────
# -D__IEEE_LITTLE_ENDIAN and -D_LDBL_EQ_DBL are needed because the installed
# machine/ieeefp.h is the generic one; our arch-specific overrides are only
# applied during the picolibc build itself, not after installation.
# Program optimisation level.  Default -O1 (validated); M8 A/B knob: `make OPT=-O2`
# is now viable because M7's I-cache removed the fetch-bound penalty that made
# unrolling/inlining a loss (LLVM's partial/runtime unroll only runs at -O2+).
# The compiler-rt builtins below stay -O1 (stable).
OPT ?= -O1
CFLAGS = -target $(TRIPLE) $(OPT) \
         -isystem $(PICOLIBC)/include \
         -nostdlib \
         -ffunction-sections -fdata-sections \
         -fPIC \
         -MMD -MP \
         -D__IEEE_LITTLE_ENDIAN \
         -D_LDBL_EQ_DBL

# ── Link-time optimisation: `make LTO=1` ─────────────────────────────────────
# The objects built with CFLAGS become LLVM bitcode, and ld.lld optimises and
# compiles each program as one module (--lto-O1/2/3 from OPT; -Os and -Oz are
# function attributes in the bitcode and take the -O2 module pipeline, as
# clang's own LTO link does).  Native: the compiler-rt builtins (CRT_FLAGS),
# atomic.o (it defines the __sync_* functions that code generation calls, which
# the LTO module would otherwise drop as unreferenced), assembly, the LLEXT
# extensions (EXT_CFLAGS) and picolibc.  `make clean` when switching, as
# after any toolchain change.  clang's -moutline / -mno-outline are code
# generator options that the bitcode does not record (-mno-outline also marks
# the functions, -moutline does not), so they are passed to the LTO code
# generator here, as clang's own LTO link passes them (-plugin-opt).
LTO ?= 0
ifeq ($(LTO),1)
CFLAGS += -flto
LTO_LDFLAGS = --lto-O$(or $(patsubst -O%,%,$(firstword $(filter -O0 -O1 -O3,$(OPT)))),2)
LTO_LDFLAGS += $(if $(filter -moutline,$(OPT)),-mllvm -enable-machine-outliner)
LTO_LDFLAGS += $(if $(filter -mno-outline,$(OPT)),-mllvm -enable-machine-outliner=never)
endif

# ── compiler-rt builtins (soft-FP + integer division) ────────────────────────
# compiler-rt sources live in the klausscpu-llvm fork, alongside the toolchain.
# Derive the fork root from $(LLVM_BIN) (= <fork>/build/bin).  Like
# compiler-rt's own build, they are -ffreestanding: the compiler must not
# turn their code into calls to C library functions (the driver no longer
# implies it, and the programs above are compiled hosted).
BUILTINS    = $(abspath $(LLVM_BIN)/../../compiler-rt/lib/builtins)
CRT_FLAGS   = -target $(TRIPLE) -O1 -nostdlibinc -ffreestanding \
              -I$(BUILTINS) -D__SOFTFP__

CRT_SF_NAMES = addsf3 subsf3 mulsf3 divsf3 negsf2 comparesf2 \
               floatsisf floatunsisf floatdisf floatundisf \
               fixsfsi fixsfdi fixunssfsi fixunssfdi

CRT_DF_NAMES = adddf3 subdf3 muldf3 divdf3 negdf2 comparedf2 \
               floatsidf floatunsidf floatdidf floatundidf \
               fixdfsi fixdfdi fixunsdfsi fixunsdfdi \
               extendsfdf2 truncdfsf2

CRT_INT_NAMES = udivsi3 divsi3 udivdi3 divdi3 umoddi3 moddi3 udivmoddi4 \
                udivti3 umodti3 udivmodti4 multi3

CRT_FP_NAMES = $(CRT_SF_NAMES) $(CRT_DF_NAMES) $(CRT_INT_NAMES)
CRT_FP_OBJS  = $(patsubst %, crt-%.o, $(CRT_FP_NAMES)) fp_mode_stub.o

LD_SCRIPT = $(ROOT)/klausscpu.ld

FATFS_DIR = $(ROOT)/fatfs

# Objects linked into every program.
RUNTIME_OBJS = crt0.o uart_stubs.o
# atomic.o: the __sync_* libcalls behind C atomics (src/atomic.c); with
# --gc-sections only the functions a program uses are linked.
LIBC_OBJS    = syscalls.o stdio_handles.o setjmp.o atomic.o

# Hardware crypto driver (MMIO wrappers for all 5 crypto blocks).
CRYPTO_HW_OBJ = crypto_hw.o
crypto_hw.o: $(ROOT)/src/crypto_hw.c
	$(CC) $(CFLAGS) -c -o $@ $<
LIBC_LINK    = $(PICOLIBC)/lib/libc.a

# FatFs objects (compiled with -I fatfs/ so ffconf.h / diskio.h are found).
# Also used by sd.c and diskio.c which reference ff.h / diskio.h.  -I$(ROOT) is
# for the shared headers (mmio.h, crypto_hw.h, rtos.h) at the repo root.
FATFS_FLAGS = $(CFLAGS) -Os -I$(FATFS_DIR) -I$(ROOT)
FATFS_OBJS  = ff.o ffunicode.o ffsystem.o sd.o diskio.o

# ── Default target ────────────────────────────────────────────────────────────

PROGRAMS = hello adventure test_64bit expr bst crypto queens \
           test_switch test_fp test_asm test_printf test_varargs test_atomic test_flags test_divshadow perf_baseline perf_haz fs_demo \
           test_fatfs_printf test_big test_cache test_rtos test_sd test_sync \
           test_eth eth_test lwip_demo ping_demo tcp_echo http_server net_client \
           netboot loader crypto_selftest crypto_test dhrystone blit_selftest blit_capture \
           test_unaligned test_setjmp test_task_exit

.PHONY: all clean $(PROGRAMS)

all: $(addsuffix .elf, $(PROGRAMS))

# ── Compile rules ─────────────────────────────────────────────────────────────

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# KlaussCPU assembly (same CC/CFLAGS as objects — routes through the target
# backend, unlike a hand-run clang with an unset triple).  `make foo.s OPT=-O2`.
%.s: %.c
	$(CC) $(CFLAGS) -S -o $@ $<

%.ll: %.c
	$(CC) $(CFLAGS) -S -emit-llvm -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

# Explicit rules for src/ objects to avoid VPATH finding stale copies
# (e.g. src/setjmp.o left by the freertos build) and confusing make.
setjmp.o:      $(ROOT)/src/setjmp.S      ; $(CC) $(CFLAGS) -c -o $@ $<
syscalls.o:    $(ROOT)/src/syscalls.c    ; $(CC) $(CFLAGS) -c -o $@ $<
stdio_handles.o: $(ROOT)/src/stdio_handles.c ; $(CC) $(CFLAGS) -c -o $@ $<
uart_stubs.o:  $(ROOT)/src/uart_stubs.c  ; $(CC) $(CFLAGS) -I$(ROOT) -c -o $@ $<
crt0.o:        $(ROOT)/src/crt0.c        ; $(CC) $(CFLAGS) -c -o $@ $<
atomic.o:      $(ROOT)/src/atomic.c      ; $(CC) $(CFLAGS) -fno-lto -c -o $@ $<


# compiler-rt builtins compiled from in-tree source
crt-%.o: $(BUILTINS)/%.c
	$(CC) $(CRT_FLAGS) -c -o $@ $<

# fp_mode_stub.c lives in src/ but needs CRT_FLAGS (includes fp_mode.h from builtins)
fp_mode_stub.o: $(ROOT)/src/fp_mode_stub.c
	$(CC) $(CRT_FLAGS) -c -o $@ $<

# FatFs core files — compiled with -I fatfs/ to find ffconf.h
ff.o: $(FATFS_DIR)/ff.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

ffunicode.o: $(FATFS_DIR)/ffunicode.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

ffsystem.o: $(FATFS_DIR)/ffsystem.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

# SD driver and diskio porting layer also need the FatFs include path
sd.o: $(ROOT)/src/sd.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

diskio.o: $(ROOT)/src/diskio.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

# fs_demo needs the FatFs include path for ff.h
fs_demo.o: programs/fs_demo.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

test_sd.o: programs/test_sd.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

# ── Link helper macro ─────────────────────────────────────────────────────────
# $(call link, prog.elf, extra-objs...)
define link
	$(LLD) -T $(LD_SCRIPT) --gc-sections $(LTO_LDFLAGS) -o $1 \
	    $(RUNTIME_OBJS) $(LIBC_OBJS) $2 $(LIBC_LINK)
	@echo "==> $1 built"
	@file $1
endef

# ── Program link rules ────────────────────────────────────────────────────────

hello.elf: hello.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,hello.o)

adventure.elf: adventure.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,adventure.o)

test_64bit.elf: test_64bit.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_64bit.o)

expr.elf: expr.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,expr.o)

bst.elf: bst.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,bst.o)

crypto.elf: crypto.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,crypto.o)

blit_selftest.elf: blit_selftest.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,blit_selftest.o)

blit_capture.elf: blit_capture.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,blit_capture.o)

queens.elf: queens.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,queens.o)

test_switch.elf: test_switch.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_switch.o)

test_setjmp.elf: test_setjmp.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_setjmp.o)

test_fp.elf: test_fp.o $(CRT_FP_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(CRT_FP_OBJS) test_fp.o)

test_asm.elf: test_asm.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_asm.o)

test_printf.elf: test_printf.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_printf.o)

test_varargs.elf: test_varargs.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_varargs.o)

test_unaligned.elf: test_unaligned.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_unaligned.o)

test_atomic.elf: test_atomic.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_atomic.o)

# test_flags: one generated assembly function per instruction
# (programs/gen_test_flags.py writes test_flags_cases.S / .h).
test_flags_cases.o: programs/test_flags_cases.S
	$(CC) $(CFLAGS) -c -o $@ $<

test_flags.elf: test_flags.o test_flags_cases.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_flags_cases.o test_flags.o)

test_divshadow.elf: test_divshadow.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_divshadow.o)

# Debugger smoke-test program (../debug/run_debug_smoke.sh); build with
# OPT="-O1 -g" to debug it with lldb / VS Code (see ../DEBUGGING.md).
dbgtest.elf: dbgtest.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,dbgtest.o)

perf_baseline.elf: perf_baseline.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,perf_baseline.o)

perf_haz.elf: perf_haz.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,perf_haz.o)

repro.elf: repro.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,repro.o)

dhrystone.elf: dhrystone.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,dhrystone.o)

fs_demo.elf: fs_demo.o $(FATFS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(FATFS_OBJS) fs_demo.o)

test_sd.elf: test_sd.o $(FATFS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(FATFS_OBJS) test_sd.o)

# FatFs objects linked but no FatFs calls — isolation test for printf+FatFs interaction
test_fatfs_printf.o: programs/test_fatfs_printf.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

test_fatfs_printf.elf: test_fatfs_printf.o $(FATFS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(FATFS_OBJS) test_fatfs_printf.o)

test_big.o: programs/test_big.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

test_big.elf: test_big.o $(FATFS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(FATFS_OBJS) test_big.o)

# ── Ethernet / lwIP ───────────────────────────────────────────────────────────

LWIP_DIR  = $(ROOT)/lwip
LWIP_PORT = $(ROOT)/lwip_port

LWIP_FLAGS = $(CFLAGS) \
    -I$(LWIP_DIR)/src/include \
    -I$(LWIP_PORT)

# lwIP source files (core + IPv4 + netif + port)
LWIP_CORE_NAMES = altcp altcp_alloc altcp_tcp def dns inet_chksum init ip \
                  mem memp netif pbuf raw stats sys tcp tcp_in tcp_out timeouts udp
LWIP_IPV4_NAMES = acd autoip dhcp etharp icmp igmp ip4 ip4_addr ip4_frag
LWIP_NETIF_NAMES = ethernet
LWIP_PORT_NAMES  = sys_arch ethernetif

LWIP_OBJS = $(patsubst %, lwip_%.o, \
    $(LWIP_CORE_NAMES) $(LWIP_IPV4_NAMES) $(LWIP_NETIF_NAMES) $(LWIP_PORT_NAMES))

# Pattern rules — make uses whichever prerequisite file actually exists.
# -MMD -MP generates lwip_*.d dependency files so that changes to lwipopts.h
# (or any other header) automatically trigger recompilation of affected objects.
lwip_%.o: $(LWIP_DIR)/src/core/%.c
	$(CC) $(LWIP_FLAGS) -MMD -MP -c -o $@ $<

lwip_%.o: $(LWIP_DIR)/src/core/ipv4/%.c
	$(CC) $(LWIP_FLAGS) -MMD -MP -c -o $@ $<

lwip_%.o: $(LWIP_DIR)/src/netif/%.c
	$(CC) $(LWIP_FLAGS) -MMD -MP -c -o $@ $<

lwip_%.o: $(LWIP_PORT)/%.c
	$(CC) $(LWIP_FLAGS) -MMD -MP -c -o $@ $<

lwip_%.o: $(LWIP_DIR)/src/apps/sntp/%.c
	$(CC) $(LWIP_FLAGS) -MMD -MP -c -o $@ $<

lwip_%.o: $(LWIP_DIR)/src/apps/http/%.c
	$(CC) $(LWIP_FLAGS) -MMD -MP -c -o $@ $<

# Include generated dependency files (silently ignore if not yet built)
-include $(wildcard *.d lwip_*.d)

# Ethernet low-level objects (shared by test_eth and lwip_demo)
ETH_OBJS = mdio.o eth.o

mdio.o: $(ROOT)/src/mdio.c
	$(CC) $(CFLAGS) -c -o $@ $<

eth.o: $(ROOT)/src/eth.c
	$(CC) $(CFLAGS) -c -o $@ $<

# eth_test: imported Phase 5 test (standalone — no lwIP, no eth.c dependency)
eth_test.o: programs/eth_test.c
	$(CC) $(CFLAGS) -c -o $@ $<

eth_test.elf: eth_test.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,eth_test.o)

# test_eth: raw hardware bring-up test (no lwIP)
test_eth.o: programs/test_eth.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_eth.elf: test_eth.o $(ETH_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(ETH_OBJS) test_eth.o)

# test_irq_ext: ETH interrupt (source 2) + ICACHE_INV board test (M13)
test_irq_ext.o: programs/test_irq_ext.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_irq_ext.elf: test_irq_ext.o $(ETH_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(ETH_OBJS) test_irq_ext.o)

# test_vga: VGA registers, palette, vsync IRQ (source 3), FB_BASE latch (VGA_PLAN P1)
test_vga.o: programs/test_vga.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_vga.elf: test_vga.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_vga.o)

# test_vga_scan: VGA DDR scanout modes, page flips, underflow under stress (VGA_PLAN P2)
test_vga_scan.o: programs/test_vga_scan.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_vga_scan.elf: test_vga_scan.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_vga_scan.o)

# lwip_demo: full TCP/IP stack (DHCP + ping + TCP echo)
lwip_demo.o: programs/lwip_demo.c
	$(CC) $(LWIP_FLAGS) -c -o $@ $<

lwip_demo.elf: lwip_demo.o $(ETH_OBJS) $(LWIP_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(ETH_OBJS) $(LWIP_OBJS) lwip_demo.o)

ping_demo.o: programs/ping_demo.c
	$(CC) $(LWIP_FLAGS) -c -o $@ $<

ping_demo.elf: ping_demo.o $(ETH_OBJS) $(LWIP_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(ETH_OBJS) $(LWIP_OBJS) ping_demo.o)

tcp_echo.o: programs/tcp_echo.c
	$(CC) $(LWIP_FLAGS) -c -o $@ $<

tcp_echo.elf: tcp_echo.o $(ETH_OBJS) $(LWIP_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(ETH_OBJS) $(LWIP_OBJS) tcp_echo.o)

netboot.o: programs/netboot.c
	$(CC) $(LWIP_FLAGS) -c -o $@ $<

netboot.elf: netboot.o $(ETH_OBJS) $(LWIP_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(ETH_OBJS) $(LWIP_OBJS) netboot.o)

http_server.o: programs/http_server.c
	$(CC) $(LWIP_FLAGS) -c -o $@ $<

http_server.elf: http_server.o $(ETH_OBJS) $(LWIP_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(ETH_OBJS) $(LWIP_OBJS) http_server.o)

net_client.o: programs/net_client.c
	$(CC) $(LWIP_FLAGS) -c -o $@ $<

LWIP_APPS_OBJS = lwip_sntp.o lwip_http_client.o

net_client.elf: net_client.o $(ETH_OBJS) $(LWIP_OBJS) $(LWIP_APPS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(ETH_OBJS) $(LWIP_OBJS) $(LWIP_APPS_OBJS) net_client.o)

# crypto_selftest: NIST/RFC known-answer tests for all 5 hardware crypto blocks.
crypto_selftest.o: programs/crypto_selftest.c
	$(CC) $(CFLAGS) -c -o $@ $<

crypto_selftest.elf: crypto_selftest.o $(CRYPTO_HW_OBJ) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(CRYPTO_HW_OBJ) crypto_selftest.o)

crypto_selftest: crypto_selftest.elf

crypto_test.o: programs/crypto_test.c
	$(CC) $(CFLAGS) -c -o $@ $<

crypto_test.elf: crypto_test.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,crypto_test.o)

crypto_test: crypto_test.elf

# ── Loader: reads .pic binary from SD card and runs it ───────────────────────
# loader.c needs FatFs for SD card access.
loader.o: programs/loader.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

loader.elf: loader.o $(FATFS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(FATFS_OBJS) loader.o)

# ── LLEXT loadable extensions (ELFCLASS32 ET_REL) ────────────────────────────
# Supersedes the PIC loadable model.  An extension is a plain `-c` compile
# against the extension-SDK headers in ext_include/ (declarations only); every
# libc symbol resolves at load time against the kernel's exported libc (see the
# Zephyr SSH app's ssh/llext_exports.c).  Copy the resulting .llext to the SD
# card and load it with the SSH `run` command (e.g. `run adventure.llext`).
#
# Long calls (-klausscpu-short-calls=false): the loader places the extension
# anywhere in the heap, beyond the +-512 KiB reach of the 1-word short CALL,
# and cannot add the thunks lld would, so every call to the kernel is the
# 2-word CALL (R_KLAUSSCPU_ABS32).  Short calls between objects bundled into
# one extension (test_fp.llext's compiler-rt objects) are R_KLAUSSCPU_CALL18,
# which the Zephyr loader applies (arch/klausscpu/core/elf.c).
EXT_CFLAGS = -target $(TARGET) -Os -nostdinc -ffreestanding -fno-builtin \
             -mllvm -klausscpu-short-calls=false -I$(ROOT)/ext_include
EXT_DEMOS  = hello adventure expr bst crypto queens test_64bit test_switch perf_baseline dhrystone

%.llext: programs/%.c
	$(CC) $(EXT_CFLAGS) -c -o $@ $<
	@echo "==> $@ built (ELF32 ET_REL extension)"

# test_fp uses single-precision soft-FP, whose builtins (conversions especially)
# are not in the prebuilt libclang_rt.builtins.a.  Rather than bloat the kernel
# ROM by exporting them, bundle them into the extension: partial-link (ld -r)
# test_fp with the compiler-rt SF objects + the fp_mode stub, leaving only printf
# to resolve against the kernel.
EXT_FP_OBJS = crt-addsf3.o crt-subsf3.o crt-mulsf3.o crt-divsf3.o \
              crt-comparesf2.o crt-fixsfdi.o fp_mode_stub.o

test_fp_ext.o: programs/test_fp.c
	$(CC) $(EXT_CFLAGS) -c -o $@ $<

test_fp.llext: test_fp_ext.o $(EXT_FP_OBJS)
	$(LLD) -r -o $@ $^
	@echo "==> $@ built (ELF32 ET_REL extension, soft-FP bundled)"

# test_atomic: the __sync_* atomics resolve against the kernel's exports.
# <stdatomic.h> is clang's own header (after ext_include/, whose stdint.h and
# stddef.h it picks up).
test_atomic.llext: programs/test_atomic.c
	$(CC) $(EXT_CFLAGS) -isystem $(shell $(CC) -print-resource-dir)/include \
	    -DKLAUSS_LLEXT -c -o $@ $<
	@echo "==> $@ built (ELF32 ET_REL extension)"

EXT_ALL = $(addsuffix .llext, $(EXT_DEMOS)) test_fp.llext test_atomic.llext

.PHONY: ext-demos
ext-demos: $(EXT_ALL)
	@echo "==> extensions built: $(EXT_ALL)"

# ── Phony aliases ─────────────────────────────────────────────────────────────

loader:      loader.elf
hello:       hello.elf
adventure:   adventure.elf
test_64bit:  test_64bit.elf
expr:        expr.elf
bst:         bst.elf
crypto:      crypto.elf
queens:      queens.elf
test_switch: test_switch.elf
test_setjmp: test_setjmp.elf
test_fp:     test_fp.elf
test_asm:    test_asm.elf
test_printf:       test_printf.elf
test_varargs:      test_varargs.elf
test_unaligned:    test_unaligned.elf
test_atomic:       test_atomic.elf
test_divshadow:    test_divshadow.elf
perf_baseline:     perf_baseline.elf
dhrystone:         dhrystone.elf
fs_demo:           fs_demo.elf
test_fatfs_printf: test_fatfs_printf.elf
test_sd:           test_sd.elf
test_big:          test_big.elf
test_eth:          test_eth.elf
test_irq_ext:      test_irq_ext.elf
test_vga:          test_vga.elf
test_vga_scan:     test_vga_scan.elf
eth_test:          eth_test.elf
lwip_demo:         lwip_demo.elf
ping_demo:         ping_demo.elf
tcp_echo:          tcp_echo.elf
netboot:           netboot.elf
http_server:       http_server.elf
net_client:        net_client.elf

test_cache.elf: test_cache.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_cache.o)

test_cache: test_cache.elf

# RTOS objects (linked into test_rtos, test_task_exit and test_sync)
RTOS_OBJS = rtos.o context_switch.o

rtos.o: $(ROOT)/src/rtos.c
	$(CC) $(CFLAGS) -c -o $@ $<

context_switch.o: $(ROOT)/src/context_switch.S
	$(CC) $(CFLAGS) -c -o $@ $<

test_rtos.o: programs/test_rtos.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_rtos.elf: test_rtos.o $(RTOS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(RTOS_OBJS) test_rtos.o)

test_rtos: test_rtos.elf

test_task_exit.o: programs/test_task_exit.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_task_exit.elf: test_task_exit.o $(RTOS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(RTOS_OBJS) test_task_exit.o)

test_task_exit: test_task_exit.elf

test_sync.o: programs/test_sync.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_sync.elf: test_sync.o $(RTOS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(RTOS_OBJS) test_sync.o)

test_sync: test_sync.elf

# ── Inspect helpers ───────────────────────────────────────────────────────────

%.dump: %.elf
	$(LLVM_BIN)/llvm-objdump -d --no-show-raw-insn $< | head -60

clean:
	rm -f *.o crt-*.o lwip_*.o *.d lwip_*.d *.elf *.pic *.sd.elf *.llext \
	      "adventure 2.elf" "adventure 2.o" \
	      "bst 2.elf" "bst 2.o" "crypto 2.elf" "crypto 2.o" \
	      "expr 2.elf" "expr 2.o" "hello 2.elf" "hello 2.o" \
	      "queens 2.elf" "queens 2.o" "test_64bit 2.elf" "test_64bit 2.o" \
	      "test_asm 2.elf" "test_asm 2.o" "test_fp 2.elf" "test_fp 2.o" \
	      "test_printf 2.elf" "test_printf 2.o" "test_switch 2.elf" "test_switch 2.o" \
	      "crt-adddf3 2.o" "crt-addsf3 2.o" "crt-comparedf2 2.o" "crt-comparesf2 2.o" \
	      "crt-divdf3 2.o" "crt-divdi3 2.o" "crt-divsf3 2.o" "crt-divsi3 2.o" \
	      "crt-extendsfdf2 2.o" "crt-fixdfdi 2.o" "crt-fixdfsi 2.o" \
	      "crt-fixsfdi 2.o" "crt-fixsfsi 2.o" "crt-fixunsdfdi 2.o" \
	      "crt-fixunsdfsi 2.o" "crt-fixunssfdi 2.o" "crt-fixunssfsi 2.o" \
	      "crt-floatsidf 2.o" "crt-floatsisf 2.o" "crt-floatunsidf 2.o" \
	      "crt-floatunsisf 2.o" "crt-moddi3 2.o" "crt-muldf3 2.o" "crt-mulsf3 2.o" \
	      "crt-negdf2 2.o" "crt-negsf2 2.o" "crt-subdf3 2.o" "crt-subsf3 2.o" \
	      "crt-truncdfsf2 2.o" "crt-udivdi3 2.o" "crt-udivmoddi4 2.o" \
	      "crt-udivsi3 2.o" "crt-umoddi3 2.o" "crt0 2.o" "fp_mode_stub 2.o" \
		  "eth_test 2.elf" "eth_test 2.o" \
	      "io_stubs 2.o" "syscalls 2.o" "uart_stubs 2.o" 2>/dev/null; true
	rm -rf bench-build
	# Remove ../src and ../fatfs stale objects left by the freertos build so VPATH
	# doesn't confuse make into thinking targets are already up to date.
	rm -f $(ROOT)/src/setjmp.o $(ROOT)/src/syscalls.o $(ROOT)/src/stdio_handles.o $(ROOT)/src/atomic.o \
	      $(ROOT)/src/uart_stubs.o $(ROOT)/src/crt0.o $(ROOT)/src/eth.o \
	      $(ROOT)/src/mdio.o $(ROOT)/src/diskio.o $(ROOT)/src/sd.o \
	      $(ROOT)/fatfs/ff.o $(ROOT)/fatfs/ffsystem.o $(ROOT)/fatfs/ffunicode.o 2>/dev/null; true

# ── AMP core 2 (AMP_CORE2_PLAN.md) ──────────────────────────────────────────
# core2.elf: the second core's image — its own lwipopts (core2/ is FIRST on
# the include path so core2/lwipopts.h shadows lwip_port/lwipopts.h), its own
# linker script (text in the DDR window, data in local BRAM) and crt0.
# core1_amp_host: the core-1 program that embeds core2.bin and boots core 2.
CORE2_DIR   = $(ROOT)/core2
CORE2_FLAGS = $(CFLAGS) -I$(CORE2_DIR) -I$(LWIP_DIR)/src/include -I$(LWIP_PORT)
CORE2_LWIP_OBJS = $(patsubst %, c2lwip_%.o, \
    $(LWIP_CORE_NAMES) $(LWIP_IPV4_NAMES) $(LWIP_NETIF_NAMES) $(LWIP_PORT_NAMES))

c2lwip_%.o: $(LWIP_DIR)/src/core/%.c
	$(CC) $(CORE2_FLAGS) -c -o $@ $<
c2lwip_%.o: $(LWIP_DIR)/src/core/ipv4/%.c
	$(CC) $(CORE2_FLAGS) -c -o $@ $<
c2lwip_%.o: $(LWIP_DIR)/src/netif/%.c
	$(CC) $(CORE2_FLAGS) -c -o $@ $<
c2lwip_%.o: $(LWIP_PORT)/%.c
	$(CC) $(CORE2_FLAGS) -c -o $@ $<
c2_eth.o:  $(ROOT)/src/eth.c
	$(CC) $(CORE2_FLAGS) -c -o $@ $<
c2_mdio.o: $(ROOT)/src/mdio.c
	$(CC) $(CORE2_FLAGS) -c -o $@ $<
c2_crt0.o: $(CORE2_DIR)/crt0_core2.c
	$(CC) $(CFLAGS) -c -o $@ $<
c2_main.o: $(CORE2_DIR)/main.c $(CORE2_DIR)/vnc_c2.h $(ROOT)/amp/amp_proto.h
	$(CC) $(CORE2_FLAGS) -c -o $@ $<
c2_netutil.o: $(CORE2_DIR)/c2_netutil.c $(CORE2_DIR)/c2_prof.h $(CORE2_DIR)/lwipopts.h
	$(CC) $(CORE2_FLAGS) -c -o $@ $<

core2.elf: c2_crt0.o c2_main.o c2_vnc.o c2_netutil.o c2_eth.o c2_mdio.o $(CORE2_LWIP_OBJS) $(LIBC_OBJS) uart_stubs.o
	$(LLD) -T $(CORE2_DIR)/core2.ld --gc-sections $(LTO_LDFLAGS) -o $@ \
	    c2_crt0.o uart_stubs.o $(LIBC_OBJS) c2_main.o c2_vnc.o c2_netutil.o c2_eth.o c2_mdio.o \
	    $(CORE2_LWIP_OBJS) $(LIBC_LINK)
	@echo "==> $@ built"
	$(LLVM_BIN)/llvm-size $@

core2.bin: core2.elf
	$(LLVM_BIN)/llvm-objcopy -O binary $< $@

core2_image.h: core2.bin
	python3 -c "import sys; d=open('core2.bin','rb').read(); \
	  print('/* generated from core2.bin — do not edit */'); \
	  print('static const unsigned char core2_image[] = {' + ','.join(str(b) for b in d) + '};'); \
	  print('static const unsigned core2_image_len = %d;' % len(d))" > $@

core1_amp_host.o: programs/core1_amp_host.c core2_image.h
	$(CC) $(CFLAGS) -I. -c -o $@ $<
core1_amp_host.elf: core1_amp_host.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,core1_amp_host.o)
core1_amp_host: core1_amp_host.elf
core2: core2.elf
.PHONY: core2 core1_amp_host

# P4: VNC server on core 2 + the baremetal demo host.
c2_vnc.o: $(CORE2_DIR)/vnc_c2.c $(CORE2_DIR)/vnc_c2.h $(ROOT)/amp/amp_proto.h
	$(CC) $(CORE2_FLAGS) -c -o $@ $<
core1_amp_vnc.o: programs/core1_amp_vnc.c core2_image.h $(ROOT)/amp/amp_proto.h
	$(CC) $(CFLAGS) -I. -c -o $@ $<
core1_amp_vnc.elf: core1_amp_vnc.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,core1_amp_vnc.o)
core1_amp_vnc: core1_amp_vnc.elf
.PHONY: core1_amp_vnc

# ── Benchmarks: CoreMark + Embench-IoT (benchmarks/README.md) ────────────────
include benchmarks/benchmarks.mk
