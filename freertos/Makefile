# Makefile — KlaussCPU FreeRTOS port build
#
# Targets:
#   make demo         — FreeRTOS task demo (no network)
#   make net_demo     — FreeRTOS + lwIP: DHCP + NTP + HTTP in parallel tasks
#
# Prerequisites:
#   bash get-freertos.sh          (clones FreeRTOS-Kernel once)
#   cd .. && bash build-picolibc.sh   (builds picolibc with -fPIC once)

RUNTIME   := ..
# Toolchain lives in the separate klausscpu-llvm fork; point KLAUSSCPU_LLVM_BIN
# at <klausscpu-llvm>/build/bin.  See ../README.md.
ifeq ($(strip $(KLAUSSCPU_LLVM_BIN)),)
$(error KLAUSSCPU_LLVM_BIN is not set. Export it to the built toolchain's bin dir, e.g. `export KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin`. See ../README.md)
endif
LLVM_BIN  := $(KLAUSSCPU_LLVM_BIN)
LWIP_DIR  := $(RUNTIME)/lwip

CC      := $(LLVM_BIN)/clang
AS      := $(LLVM_BIN)/clang
LD      := $(LLVM_BIN)/ld.lld
OBJCOPY := $(LLVM_BIN)/llvm-objcopy

TARGET  := klausscpu-unknown-elf
PICOLIBC := $(RUNTIME)/picolibc-install
KERNEL   := FreeRTOS-Kernel

# Local build dir for objects compiled from the SHARED sources at the repo root
# (../src, ../fatfs, ../lwip_port) and the fork's compiler-rt.  Writing them here
# instead of next to the shared sources keeps the repo root, ../src, ../fatfs and
# ../lwip_port free of this build's .o/.lo/.d artifacts.
OBJ := obj

# compiler-rt builtins (soft-FP conversions + i128) for console_demo's
# CRT_CONV_OBJS, built here from the fork's compiler-rt — so the FreeRTOS build
# no longer relies on a prior bare-metal build having left crt-*.o at the root.
BUILTINS  := $(abspath $(LLVM_BIN)/../../compiler-rt/lib/builtins)
CRT_FLAGS := -target $(TARGET) -O1 -fPIC -nostdlibinc -I$(BUILTINS) -D__SOFTFP__

# ── Common compiler flags ───────────────────────────────────────────────────

BASE_CFLAGS := \
    -target $(TARGET) \
    -O1 \
    -ffunction-sections \
    -fdata-sections \
    -fPIC \
    -ffreestanding \
    -MMD -MP \
    -D__IEEE_LITTLE_ENDIAN \
    -D_LDBL_EQ_DBL \
    -isystem $(PICOLIBC)/include \
    -I$(KERNEL)/include \
    -Iportable/KlaussCPU \
    -I$(RUNTIME) \
    -nostdlib

# FreeRTOS-only flags (lwipopts from freertos/ directory, no lwIP sources)
CFLAGS := $(BASE_CFLAGS) -I.

# FreeRTOS + lwIP flags:
#   -I. must come BEFORE -I$(RUNTIME)/lwip_port so that freertos/lwipopts.h
#   and freertos/arch/sys_arch.h shadow the bare-metal versions.
NET_CFLAGS := $(BASE_CFLAGS) \
    -I. \
    -I$(LWIP_DIR)/src/include \
    -I$(RUNTIME)/lwip_port \
    -Wno-macro-redefined

ASFLAGS    := $(BASE_CFLAGS) -I.
LDFLAGS    := -T $(RUNTIME)/klausscpu.ld --gc-sections -L$(PICOLIBC)/lib

# ── wolfSSL + wolfSSH paths ─────────────────────────────────────────────────
WOLFSSL_DIR    := wolfssl/wolfssl-install
WOLFSSH_DIR    := wolfssl/wolfssh-install
WOLFSSL_LIB    := $(WOLFSSL_DIR)/lib/libwolfssl.a
WOLFSSH_LIB    := $(WOLFSSH_DIR)/lib/libwolfssh.a

# SSH / wolfSSL compiler flags (extra includes + user_settings.h).
SSH_CFLAGS := $(NET_CFLAGS) \
    -DWOLFSSL_USER_SETTINGS \
    -DWOLFSSH_NO_SFTP \
    -DWOLFSSH_NO_SCP \
    -DWOLFSSH_USER_IO \
    -Iwolfssl \
    -I$(WOLFSSL_DIR)/include \
    -I$(WOLFSSH_DIR)/include

# ── FreeRTOS kernel objects (shared by all targets) ─────────────────────────

KERNEL_SRCS := \
    $(KERNEL)/tasks.c \
    $(KERNEL)/queue.c \
    $(KERNEL)/list.c \
    $(KERNEL)/timers.c \
    $(KERNEL)/event_groups.c \
    $(KERNEL)/stream_buffer.c \
    $(KERNEL)/portable/MemMang/heap_4.c

KERNEL_OBJS := $(KERNEL_SRCS:.c=.o)

# ── Port objects ─────────────────────────────────────────────────────────────

PORT_OBJS := portable/KlaussCPU/port.o portable/KlaussCPU/portasm.o

# ── lwIP objects (compiled with NET_CFLAGS) ──────────────────────────────────

LWIP_CORE := \
    $(LWIP_DIR)/src/core/init.c \
    $(LWIP_DIR)/src/core/def.c \
    $(LWIP_DIR)/src/core/dns.c \
    $(LWIP_DIR)/src/core/inet_chksum.c \
    $(LWIP_DIR)/src/core/ip.c \
    $(LWIP_DIR)/src/core/mem.c \
    $(LWIP_DIR)/src/core/memp.c \
    $(LWIP_DIR)/src/core/netif.c \
    $(LWIP_DIR)/src/core/pbuf.c \
    $(LWIP_DIR)/src/core/raw.c \
    $(LWIP_DIR)/src/core/stats.c \
    $(LWIP_DIR)/src/core/sys.c \
    $(LWIP_DIR)/src/core/tcp.c \
    $(LWIP_DIR)/src/core/tcp_in.c \
    $(LWIP_DIR)/src/core/tcp_out.c \
    $(LWIP_DIR)/src/core/timeouts.c \
    $(LWIP_DIR)/src/core/udp.c

LWIP_IPV4 := \
    $(LWIP_DIR)/src/core/ipv4/acd.c \
    $(LWIP_DIR)/src/core/ipv4/autoip.c \
    $(LWIP_DIR)/src/core/ipv4/dhcp.c \
    $(LWIP_DIR)/src/core/ipv4/etharp.c \
    $(LWIP_DIR)/src/core/ipv4/icmp.c \
    $(LWIP_DIR)/src/core/ipv4/igmp.c \
    $(LWIP_DIR)/src/core/ipv4/ip4.c \
    $(LWIP_DIR)/src/core/ipv4/ip4_addr.c \
    $(LWIP_DIR)/src/core/ipv4/ip4_frag.c

LWIP_NETIF := \
    $(LWIP_DIR)/src/netif/ethernet.c

LWIP_APIS := \
    $(LWIP_DIR)/src/api/tcpip.c \
    $(LWIP_DIR)/src/api/api_lib.c \
    $(LWIP_DIR)/src/api/api_msg.c \
    $(LWIP_DIR)/src/api/err.c \
    $(LWIP_DIR)/src/api/netbuf.c \
    $(LWIP_DIR)/src/api/netdb.c \
    $(LWIP_DIR)/src/api/netifapi.c \
    $(LWIP_DIR)/src/api/sockets.c \
    $(LWIP_DIR)/src/apps/sntp/sntp.c

LWIP_SRCS  := $(LWIP_CORE) $(LWIP_IPV4) $(LWIP_NETIF) $(LWIP_APIS)
LWIP_OBJS  := $(LWIP_SRCS:.c=.lo)   # .lo = compiled with NET_CFLAGS

LWIP_PORT_SRCS := \
    $(RUNTIME)/lwip_port/ethernetif.c \
    sys_arch.c                          # FreeRTOS sys_arch

LWIP_PORT_OBJS := \
    $(OBJ)/ethernetif.lo \
    sys_arch.lo

# ── Runtime support ──────────────────────────────────────────────────────────

RUNTIME_OBJS := \
    $(OBJ)/crt0.o \
    $(OBJ)/uart_stubs.o \
    $(OBJ)/syscalls.o \
    $(OBJ)/stdio_handles.o \
    $(OBJ)/setjmp.o \
    $(OBJ)/mdio.o \
    $(OBJ)/eth.o

# ── FatFs + SD card (console_demo only) ──────────────────────────────────────

FATFS_DIR := $(RUNTIME)/fatfs

FATFS_OBJS := \
    $(OBJ)/ff.o \
    $(OBJ)/ffsystem.o \
    $(OBJ)/ffunicode.o

SD_OBJS := \
    $(OBJ)/sd.o \
    $(OBJ)/diskio.o

# ── Hardware crypto driver ────────────────────────────────────────────────────

CRYPTO_HW_OBJ := $(OBJ)/crypto_hw.o

# ── compiler-rt runtime support objects ──────────────────────────────────────
# picolibc pulls in float/double conversion symbols even when %f is not used.
# wolfSSL with 64-bit SP math uses __udivti3/__umodti3/__multi3 (i128 arithmetic).
CRT_CONV_OBJS := \
    $(OBJ)/crt-extendsfdf2.o \
    $(OBJ)/crt-truncdfsf2.o \
    $(OBJ)/fp_mode_stub.o \
    $(OBJ)/crt-udivti3.o \
    $(OBJ)/crt-umodti3.o \
    $(OBJ)/crt-udivmodti4.o \
    $(OBJ)/crt-multi3.o

# ── SSH server objects ────────────────────────────────────────────────────────

SSH_OBJS := \
    wolfssl_hw.o \
    ssh/ssh_server.o \
    ssh/sshkeys.o

# ── Rules ─────────────────────────────────────────────────────────────────────

.PHONY: all demo net_demo console_demo clean check-kernel

all: demo net_demo console_demo

demo: demo.elf demo.bin
	@echo "==> demo.elf built"

net_demo: net_demo.elf net_demo.bin
	@echo "==> net_demo.elf built"

console_demo: console_demo.elf console_demo.bin
	@echo "==> console_demo.elf built"

check-kernel:
	@test -d $(KERNEL) || { \
	    echo "ERROR: FreeRTOS kernel not found."; \
	    echo "Run:  bash get-freertos.sh"; \
	    exit 1; }

# Kernel + port — compiled with CFLAGS (no lwIP)
$(KERNEL_OBJS): %.o: %.c | check-kernel
	$(CC) $(CFLAGS) -c -o $@ $<

portable/KlaussCPU/portasm.o: portable/KlaussCPU/port.S | check-kernel
	$(AS) $(ASFLAGS) -c -o $@ $<

portable/KlaussCPU/port.o: portable/KlaussCPU/port.c | check-kernel
	$(CC) $(CFLAGS) -c -o $@ $<

# Demo objects — compiled with CFLAGS
demo/demo.o: demo/demo.c | check-kernel
	$(CC) $(CFLAGS) -c -o $@ $<

# Network demo objects — compiled with NET_CFLAGS
demo/net_demo.o: demo/net_demo.c | check-kernel
	$(CC) $(NET_CFLAGS) -c -o $@ $<

demo/console_demo.o: demo/console_demo.c | check-kernel
	$(CC) $(SSH_CFLAGS) -Itelnet -I$(FATFS_DIR) -c -o $@ $<

telnet/telnet.o: telnet/telnet.c | check-kernel
	$(CC) $(NET_CFLAGS) -Itelnet -c -o $@ $<

telnet/pic_loader.o: telnet/pic_loader.c | check-kernel
	$(CC) $(NET_CFLAGS) -Itelnet -I$(FATFS_DIR) -c -o $@ $<

# lwIP sources — use .lo extension to distinguish from CFLAGS-compiled .o
%.lo: %.c | check-kernel
	$(CC) $(NET_CFLAGS) -c -o $@ $<

$(OBJ)/ethernetif.lo: $(RUNTIME)/lwip_port/ethernetif.c | $(OBJ) check-kernel
	$(CC) $(NET_CFLAGS) -c -o $@ $<

sys_arch.lo: sys_arch.c | check-kernel
	$(CC) $(NET_CFLAGS) -c -o $@ $<

# Local object directory for shared-source / compiler-rt objects.
$(OBJ):
	mkdir -p $(OBJ)

# Runtime objects (compiled from the shared ../src sources into $(OBJ)/)
$(OBJ)/crt0.o: $(RUNTIME)/src/crt0.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ)/uart_stubs.o: $(RUNTIME)/src/uart_stubs.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ)/syscalls.o: $(RUNTIME)/src/syscalls.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ)/stdio_handles.o: $(RUNTIME)/src/stdio_handles.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ)/setjmp.o: $(RUNTIME)/src/setjmp.S | $(OBJ)
	$(AS) $(ASFLAGS) -c -o $@ $<

$(OBJ)/mdio.o: $(RUNTIME)/src/mdio.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ)/eth.o: $(RUNTIME)/src/eth.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ)/crypto_hw.o: $(RUNTIME)/src/crypto_hw.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

# compiler-rt builtins + fp_mode stub (console_demo soft-FP/i128 support)
$(OBJ)/crt-%.o: $(BUILTINS)/%.c | $(OBJ)
	$(CC) $(CRT_FLAGS) -c -o $@ $<

$(OBJ)/fp_mode_stub.o: $(RUNTIME)/src/fp_mode_stub.c | $(OBJ)
	$(CC) $(CRT_FLAGS) -c -o $@ $<

# SSH / wolfSSL objects (compiled with SSH_CFLAGS = NET_CFLAGS + wolfSSL includes)
wolfssl_hw.o: wolfssl_hw.c
	$(CC) $(SSH_CFLAGS) -c -o $@ $<

ssh/ssh_server.o: ssh/ssh_server.c
	$(CC) $(SSH_CFLAGS) -Itelnet -I$(FATFS_DIR) -c -o $@ $<

ssh/sshkeys.o: ssh/sshkeys.c
	$(CC) $(SSH_CFLAGS) -I$(FATFS_DIR) -c -o $@ $<

# FatFs objects (no lwIP needed; FF_FS_REENTRANT=0 so no OS calls)
$(OBJ)/ff.o: $(FATFS_DIR)/ff.c | $(OBJ)
	$(CC) $(CFLAGS) -I$(FATFS_DIR) -c -o $@ $<

$(OBJ)/ffsystem.o: $(FATFS_DIR)/ffsystem.c | $(OBJ)
	$(CC) $(CFLAGS) -I$(FATFS_DIR) -c -o $@ $<

$(OBJ)/ffunicode.o: $(FATFS_DIR)/ffunicode.c | $(OBJ)
	$(CC) $(CFLAGS) -I$(FATFS_DIR) -c -o $@ $<

# SD card driver
$(OBJ)/sd.o: $(RUNTIME)/src/sd.c | $(OBJ)
	$(CC) $(CFLAGS) -I$(FATFS_DIR) -c -o $@ $<

$(OBJ)/diskio.o: $(RUNTIME)/src/diskio.c | $(OBJ)
	$(CC) $(CFLAGS) -I$(FATFS_DIR) -c -o $@ $<

# Link
demo.elf: $(KERNEL_OBJS) $(PORT_OBJS) demo/demo.o $(RUNTIME_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^ $(PICOLIBC)/lib/libc.a
	@file $@

demo.bin: demo.elf
	$(OBJCOPY) -O binary $< $@

net_demo.elf: $(KERNEL_OBJS) $(PORT_OBJS) $(LWIP_OBJS) $(LWIP_PORT_OBJS) \
              demo/net_demo.o $(RUNTIME_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^ $(PICOLIBC)/lib/libc.a
	@file $@

net_demo.bin: net_demo.elf
	$(OBJCOPY) -O binary $< $@

console_demo.elf: $(KERNEL_OBJS) $(PORT_OBJS) $(LWIP_OBJS) $(LWIP_PORT_OBJS) \
                  $(FATFS_OBJS) $(SD_OBJS) \
                  telnet/telnet.o telnet/pic_loader.o \
                  $(SSH_OBJS) $(CRYPTO_HW_OBJ) \
                  demo/console_demo.o $(RUNTIME_OBJS) $(CRT_CONV_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^ \
	    $(WOLFSSH_LIB) $(WOLFSSL_LIB) $(PICOLIBC)/lib/libc.a
	@file $@

console_demo.bin: console_demo.elf
	$(OBJCOPY) -O binary $< $@

# Include compiler-generated header dependency files.
# -MMD -MP in BASE_CFLAGS writes a .d next to each .o/.lo.
# The wildcards cover every subdirectory that receives output files.
-include $(wildcard \
    *.d \
    $(OBJ)/*.d \
    ssh/*.d telnet/*.d demo/*.d \
    portable/KlaussCPU/*.d \
    $(KERNEL)/*.d $(KERNEL)/portable/MemMang/*.d)

clean:
	rm -rf $(OBJ)
	rm -f $(KERNEL_OBJS) $(PORT_OBJS) \
	      $(LWIP_OBJS) \
	      demo/demo.o demo/net_demo.o demo/console_demo.o \
	      telnet/telnet.o telnet/pic_loader.o \
	      wolfssl_hw.o ssh/ssh_server.o ssh/sshkeys.o \
	      sys_arch.lo \
	      demo.elf demo.bin net_demo.elf net_demo.bin \
	      console_demo.elf console_demo.bin \
	      *.d ssh/*.d telnet/*.d demo/*.d \
	      portable/KlaussCPU/*.d \
	      $(KERNEL)/*.d $(KERNEL)/portable/MemMang/*.d
	# Sweep any stale shared-source objects a PRE-obj/ build left at the repo
	# root / ../src / ../fatfs / ../lwip_port (harmless if already absent).
	rm -f $(RUNTIME)/crt0.o $(RUNTIME)/uart_stubs.o $(RUNTIME)/stdio_handles.o \
	      $(RUNTIME)/crt-*.o $(RUNTIME)/fp_mode_stub.o \
	      $(RUNTIME)/src/syscalls.o $(RUNTIME)/src/setjmp.o $(RUNTIME)/src/mdio.o \
	      $(RUNTIME)/src/eth.o $(RUNTIME)/src/sd.o $(RUNTIME)/src/diskio.o \
	      $(RUNTIME)/src/crypto_hw.o $(RUNTIME)/src/*.d \
	      $(RUNTIME)/fatfs/ff.o $(RUNTIME)/fatfs/ffsystem.o $(RUNTIME)/fatfs/ffunicode.o \
	      $(RUNTIME)/lwip_port/ethernetif.lo $(RUNTIME)/lwip_port/*.d 2>/dev/null; true
