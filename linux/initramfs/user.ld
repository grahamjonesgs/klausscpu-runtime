/* Static user programs: text at 64 KiB, each segment type on its own 64 KiB page. */
ENTRY(_start)
SECTIONS
{
	. = 0x10000;
	.text : { *(.text .text.*) }
	. = ALIGN(0x10000);
	.rodata : { *(.rodata .rodata.*) }
	. = ALIGN(0x10000);
	.data : { *(.data .data.*) *(.sdata .sdata.*) }
	.bss : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }
	/DISCARD/ : { *(.comment) *(.note*) }
}
