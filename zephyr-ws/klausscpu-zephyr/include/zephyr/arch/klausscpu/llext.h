/*
 * llext.h — KlaussCPU LLEXT: which extensions this kernel can run.
 *
 * An extension is an ET_REL object compiled by klausscpu-llvm.  The low byte
 * of its e_flags is the C ABI version it was compiled for (ABI.md in
 * klausscpu-llvm); one built for another version passes and returns its
 * arguments differently from the kernel it calls, and one from before the
 * ISA v2 encoding (ABI 0) crashes at its first instruction.  llext_load()
 * refuses both through arch_elf_check_header() (arch/klausscpu/core/elf.c).
 *
 * Mirror of klausscpu-llvm's llvm/include/llvm/BinaryFormat/ELF.h
 * (EM_KLAUSSCPU) and llvm/include/llvm/TargetParser/KlaussCPUABI.h
 * (ABIVersion, EFlagsABIMask); klausscpu-runtime's
 * check-toolchain-constants.py compares them.
 */
#ifndef ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_LLEXT_H_
#define ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_LLEXT_H_

#include <stddef.h>
#include <zephyr/llext/elf.h>

#define EM_KLAUSSCPU          0x4B43u /* 'K' << 8 | 'C' */
#define KLAUSSCPU_EF_ABI_MASK 0xFFu
#define KLAUSSCPU_ABI_VERSION 2u

/**
 * @brief Check that an extension's ELF header is one this kernel can run.
 *
 * @param[in]  hdr     the extension's ELF header
 * @param[out] why     on a refusal, the reason as one line (no newline),
 *                     e.g. "built for C ABI 0, this kernel is ABI 2
 *                     (e_flags 0x0): rebuild it"; may be NULL
 * @param[in]  why_len size of @p why
 * @retval 0        the machine and the C ABI version match
 * @retval -ENOEXEC another machine or another C ABI version
 */
int klausscpu_llext_check_header(const elf_ehdr_t *hdr, char *why,
				 size_t why_len);

#endif /* ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_LLEXT_H_ */
