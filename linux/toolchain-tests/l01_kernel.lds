/* kernel-style linker script: VMA in kseg, LMA physical, PROVIDE, KEEP, ASSERT, /DISCARD/, ALIGN, AT() */
OUTPUT_ARCH(klausscpu)
ENTRY(_start)
PAGE_SIZE = 0x10000;
KSEG = 0x80000000;
SECTIONS
{
	. = KSEG + 0x100000;
	_text = .;
	.head.text : AT(ADDR(.head.text) - KSEG) { KEEP(*(.head.text)) }
	.text : AT(ADDR(.text) - KSEG) { *(.text .text.*) }
	_etext = .;
	. = ALIGN(PAGE_SIZE);
	.rodata : AT(ADDR(.rodata) - KSEG) { *(.rodata .rodata.*) }
	__ex_table : AT(ADDR(__ex_table) - KSEG) {
		__start___ex_table = .;
		KEEP(*(__ex_table))
		__stop___ex_table = .;
	}
	. = ALIGN(PAGE_SIZE);
	.data : AT(ADDR(.data) - KSEG) { *(.data .data.*) }
	.init.text : AT(ADDR(.init.text) - KSEG) { *(.init.text) }
	. = ALIGN(8);
	__bss_start = .;
	.bss : AT(ADDR(.bss) - KSEG) { *(.bss .bss.* COMMON) }
	__bss_stop = .;
	_end = .;
	PROVIDE(ext_sym = 0x1234);
	/DISCARD/ : { *(.discard) *(.discard.*) *(.note.GNU-stack) *(.comment) }
}
ASSERT(_end - KSEG < 0x08000000, "kernel too big for kseg");
ASSERT(__stop___ex_table - __start___ex_table == 16, "one ex_table entry expected");
