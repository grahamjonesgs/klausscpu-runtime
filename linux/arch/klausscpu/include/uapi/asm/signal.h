/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_KLAUSSCPU_SIGNAL_H
#define _UAPI_ASM_KLAUSSCPU_SIGNAL_H

/*
 * Handlers return through sa_restorer (the C library's rt_sigreturn stub):
 * there is no vDSO yet and stacks are not executable.
 */
#define SA_RESTORER	0x04000000

#include <asm-generic/signal.h>

#endif
