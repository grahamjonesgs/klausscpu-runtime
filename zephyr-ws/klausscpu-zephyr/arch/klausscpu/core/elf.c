/*
 * elf.c — KlaussCPU architecture-specific LLEXT relocations.
 *
 * PROPOSAL / NOT YET WIRED INTO THE BUILD.
 * This implements the arch hook that Zephyr's LLEXT subsystem
 * (subsys/llext/) calls to relocate a partially-linked (ET_REL) extension
 * after it has been copied into RAM.  It is the runtime-loader counterpart
 * of lld/ELF/Arch/KlaussCPU.cpp::relocate() — the formulas here MUST match
 * lld so a loaded extension behaves identically to a statically linked one.
 *
 * Targets Zephyr 3.7 LTS, whose hook signature is:
 *     int arch_elf_relocate(elf_rela_t *rel, uintptr_t loc,
 *                           uintptr_t sym_base_addr, const char *sym_name,
 *                           uintptr_t load_bias);
 * (Newer Zephyr changed this to (ldr, ext, rel, shdr) and does symbol
 *  lookup inside the arch hook — see arch/riscv/core/elf.c on main.)
 *
 * KlaussCPU relocations are whole-field (no HI20/LO12 splitting) except the
 * short call's word displacement, so unlike RISC-V/ARM this handler is tiny:
 * four real cases.
 *
 * Endianness: KlaussCPU data memory is little-endian, and lld writes every
 * relocated field (including the instruction word-1 immediate slot) with
 * write32le/write64le.  We therefore use sys_put_le32/64 so the bytes match
 * lld exactly and unaligned targets are handled safely.
 */

#include <errno.h>

#include <zephyr/llext/elf.h>
#include <zephyr/llext/llext.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(elf, CONFIG_LLEXT_LOG_LEVEL);

/*
 * KlaussCPU relocation types — mirror of
 * llvm/include/llvm/BinaryFormat/ELFRelocs/KlaussCPU.def (and of the
 * runtime's klausscpu_elf.h, which this arch does not include to stay
 * self-contained).  klausscpu-runtime/check-toolchain-constants.py checks
 * that they agree.
 */
#define R_KLAUSSCPU_NONE    0
#define R_KLAUSSCPU_ABS32   1
#define R_KLAUSSCPU_ABS64   2
#define R_KLAUSSCPU_PCREL32 3
#define R_KLAUSSCPU_PC32    4
#define R_KLAUSSCPU_PC64    5
#define R_KLAUSSCPU_CALL18  6

/*
 * PC-relative fields are stored relative to the START of the instruction,
 * while the relocation r_offset (and therefore `loc`) points at the 4-byte
 * immediate slot, which lives at offset 4 within the 8-byte instruction.
 * lld encodes this as: stored = S + A - P + 4  (see KlaussCPU.cpp relocate()).
 */
#define KLAUSSCPU_PCREL_INSN_BIAS 4

/**
 * @brief Relocate one entry of a partially-linked KlaussCPU extension.
 *
 * Symbolic names follow the usual ELF convention:
 *   S = sym_base_addr   (resolved address of the referenced symbol)
 *   A = rel->r_addend   (RELA addend; KlaussCPU emits .rela sections)
 *   P = loc             (address of the field being patched)
 */
int arch_elf_relocate(elf_rela_t *rel, uintptr_t loc, uintptr_t sym_base_addr,
		      const char *sym_name, uintptr_t load_bias)
{
	uint32_t reloc_type = ELF32_R_TYPE(rel->r_info);

	ARG_UNUSED(load_bias); /* RELA gives absolute S+A / self-relative P; no bias needed */

	LOG_DBG("reloc type %u  loc %#lx  S %#lx  A %ld  sym '%s'", reloc_type,
		(unsigned long)loc, (unsigned long)sym_base_addr,
		(long)rel->r_addend, sym_name ? sym_name : "?");

	switch (reloc_type) {
	case R_KLAUSSCPU_NONE:
		break;

	case R_KLAUSSCPU_ABS32:
		/* 32-bit absolute (e.g. SETR sym immediate).  S + A. */
		sys_put_le32((uint32_t)(sym_base_addr + rel->r_addend),
			     (uint8_t *)loc);
		break;

	case R_KLAUSSCPU_ABS64:
		/* 64-bit absolute (8-byte data pointer field).  S + A. */
		sys_put_le64((uint64_t)(sym_base_addr + rel->r_addend),
			     (uint8_t *)loc);
		break;

	case R_KLAUSSCPU_PCREL32:
		/* PC-relative call/branch target.  S + A - P, biased +4 so the
		 * field is relative to the instruction start (hardware
		 * convention; identical to lld's relocate()). */
		sys_put_le32((uint32_t)(sym_base_addr + rel->r_addend - loc +
					KLAUSSCPU_PCREL_INSN_BIAS),
			     (uint8_t *)loc);
		break;

	case R_KLAUSSCPU_PC32:
		/* 32-bit PC-relative data, a symbol difference such as the
		 * -fPIC relative lookup tables (`.long str - table`).  S + A - P
		 * exactly: the field itself is the origin, no instruction bias. */
		sys_put_le32((uint32_t)(sym_base_addr + rel->r_addend - loc),
			     (uint8_t *)loc);
		break;

	case R_KLAUSSCPU_PC64:
		/* 64-bit PC-relative data (`.quad a - b`).  S + A - P. */
		sys_put_le64((uint64_t)(sym_base_addr + rel->r_addend - loc),
			     (uint8_t *)loc);
		break;

	case R_KLAUSSCPU_CALL18: {
		/* ISA v3 short CALL (word 0 0x63000000) or a tail call's short
		 * jump (0x61000000): simm18 word displacement
		 * (S + A - P) / 4 in word0[17:0], P = the instruction itself (the
		 * relocation is at its start).  It reaches +-512 KiB; lld calls a
		 * further callee through a thunk, which this loader cannot add.
		 * Extensions are therefore built with long calls (-mllvm
		 * -klausscpu-short-calls=false, LLEXT_APPEND_FLAGS), and a short
		 * call here is one between objects bundled into the extension. */
		intptr_t disp = (intptr_t)(sym_base_addr + rel->r_addend) -
				(intptr_t)loc;

		if ((disp & 3) != 0 || disp < -(1 << 19) || disp >= (1 << 19)) {
			LOG_ERR("short call to '%s' out of reach (%ld bytes): build "
				"with -mllvm -klausscpu-short-calls=false",
				sym_name ? sym_name : "?", (long)disp);
			return -ENOEXEC;
		}
		sys_put_le32((sys_get_le32((uint8_t *)loc) & ~0x3FFFFu) |
				     ((uint32_t)(disp >> 2) & 0x3FFFFu),
			     (uint8_t *)loc);
		break;
	}

	default:
		LOG_ERR("unsupported relocation type %u for symbol '%s'",
			reloc_type, sym_name ? sym_name : "?");
		return -ENOTSUP;
	}

	return 0;
}
