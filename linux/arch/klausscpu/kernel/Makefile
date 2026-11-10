# SPDX-License-Identifier: GPL-2.0
extra-y := vmlinux.lds

obj-y := head.o entry.o setup.o traps.o irq.o time.o process.o signal.o \
	ptrace.o syscall_table.o early_printk.o hvc_uart.o \
	stacktrace.o
obj-$(CONFIG_KLAUSSCPU_SD) += sdcard.o sd_spi.o
obj-$(CONFIG_KLAUSSCPU_ETH) += liteeth.o
obj-$(CONFIG_KLAUSSCPU_TRNG) += trng.o
obj-$(CONFIG_KLAUSSCPU_QSPI) += qspi_flash.o

CFLAGS_hvc_uart.o := -I$(srctree)/drivers/tty/hvc
