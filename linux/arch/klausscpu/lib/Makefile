# SPDX-License-Identifier: GPL-2.0
lib-y := uaccess.o delay.o
obj-y := string.o
# -O2 for the copy loops; -ffreestanding so no loop becomes a memset or
# memcpy call (these are memset and memcpy).
CFLAGS_string.o := -O2 -ffreestanding
