# SPDX-License-Identifier: GPL-2.0
# KlaussCPU arch Makefile (included by the top-level Makefile).

KBUILD_DEFCONFIG := defconfig

# klausscpu-llvm: freestanding kernel code, absolute addresses (the kernel
# runs at its kseg link address), no small-data or PIC tricks.
KBUILD_CFLAGS	+= -fno-pic -fno-PIE
KBUILD_AFLAGS	+= -fno-pic -fno-PIE
CHECKFLAGS	+= -D__klausscpu__

libs-y		+= arch/klausscpu/lib/

all: vmlinux

define archhelp
  echo  '* vmlinux       - Kernel ELF for the klausscc loader / emulator'
endef
