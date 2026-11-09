# klausscpu: build xv6 for KlaussCPU with klausscpu-llvm (XV6_PLAN.md).
# The upstream RISC-V Makefile is kept as Makefile.upstream.
#
#   make                  kernel/kernel (with fs.img linked in)
#   make clean
#
# KLAUSSCPU_LLVM_BIN: the klausscpu-llvm build's bin directory (branch mmu).

K=kernel
U=user

OBJS = \
  $K/boot.o \
  $K/console.o \
  $K/printk.o \
  $K/uart.o \
  $K/kalloc.o \
  $K/spinlock.o \
  $K/string.o \
  $K/main.o \
  $K/vm.o \
  $K/proc.o \
  $K/swtch.o \
  $K/trapvec.o \
  $K/trap.o \
  $K/syscall.o \
  $K/sysproc.o \
  $K/bio.o \
  $K/fs.o \
  $K/log.o \
  $K/sleeplock.o \
  $K/file.o \
  $K/pipe.o \
  $K/exec.o \
  $K/sysfile.o \
  $K/katomic.o \
  $K/ramdisk.o \
  $K/fsimg.o

ifndef KLAUSSCPU_LLVM_BIN
$(error KLAUSSCPU_LLVM_BIN is not set: export it to the klausscpu-llvm build's bin directory)
endif
LLVM_BIN := $(patsubst ~/%,$(HOME)/%,$(KLAUSSCPU_LLVM_BIN))

CC = $(LLVM_BIN)/clang
LD = $(LLVM_BIN)/ld.lld
OBJDUMP = $(LLVM_BIN)/llvm-objdump
TARGET = klausscpu-unknown-elf

OPT ?= -O2
CFLAGS = -target $(TARGET) $(OPT) -Wall -Werror -fno-omit-frame-pointer -g
CFLAGS += -std=gnu99 -MD
CFLAGS += -ffreestanding -fno-builtin -fno-common -nostdlib
CFLAGS += -fno-delete-null-pointer-checks -Wno-main
CFLAGS += -I.
ASFLAGS = -target $(TARGET) -g -I.

LDFLAGS = -z noexecstack

$K/kernel: $(OBJS) $K/kernel.ld
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o $K/kernel $(OBJS)
	$(OBJDUMP) -S $K/kernel > $K/kernel.asm
	$(OBJDUMP) -t $K/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $K/kernel.sym

$K/%.o: $K/%.S
	$(CC) $(ASFLAGS) -c -o $@ $<

$K/fsimg.o: $K/fsimg.S fs.img
	$(CC) $(ASFLAGS) -DFS_IMG='"fs.img"' -c -o $@ $<

ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o

# User programs: linked at VA 0, text and data in separate 64 KiB-aligned
# segments (exec maps whole pages); --nmagic keeps the file offsets packed.
_%: %.o $(ULIB) $U/user.ld
	$(LD) $(LDFLAGS) --nmagic -T $U/user.ld -o $@ $< $(ULIB)
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o : $U/usys.S
	$(CC) $(ASFLAGS) -c -o $U/usys.o $U/usys.S

$U/_forktest: $U/forktest.o $(ULIB) $U/user.ld
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) --nmagic -T $U/user.ld -e main -o $U/_forktest $U/forktest.o $U/ulib.o $U/usys.o
	$(OBJDUMP) -S $U/_forktest > $U/forktest.asm

mkfs/mkfs: mkfs/mkfs.c $K/fs.h $K/param.h
	cc -Wno-unknown-attributes -I. -o mkfs/mkfs mkfs/mkfs.c

.PRECIOUS: %.o

UPROGS=\
	$U/_cat\
	$U/_echo\
	$U/_forktest\
	$U/_grep\
	$U/_init\
	$U/_kill\
	$U/_ln\
	$U/_ls\
	$U/_mkdir\
	$U/_rm\
	$U/_sh\
	$U/_stressfs\
	$U/_usertests\
	$U/_grind\
	$U/_wc\
	$U/_zombie\
	$U/_logstress\
	$U/_forphan\
	$U/_dorphan\
	$U/_sync\

fs.img: mkfs/mkfs README $(UPROGS)
	mkfs/mkfs fs.img README $(UPROGS)

-include kernel/*.d user/*.d

clean:
	rm -f */*.o */*.d */*.asm */*.sym \
	$K/kernel fs.img \
	mkfs/mkfs \
	$U/usys.S \
	$(UPROGS)

.PHONY: clean
