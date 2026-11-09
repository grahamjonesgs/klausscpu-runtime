/* klausscpu: the xv6 kernel (XV6_PLAN.md).
 *
 * The loader puts the image at PA 0x20 (the first 32 bytes are the
 * hardware's heap header) whatever its link address, so the kernel is
 * linked at the kseg alias of that, KERNBASE + 0x20, and boot.S starts
 * there with translation off and jumps into kseg.
 */
OUTPUT_ARCH( "klausscpu" )
ENTRY( _start )

SECTIONS
{
  . = 0x80000020;

  .text : {
    KEEP(*(.text.boot))  /* _start: the first instruction, at PA 0x20 */
    *(.text .text.*)
    . = ALIGN(8);
    PROVIDE(etext = .);
  }

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*)
    . = ALIGN(16);
    *(.rodata .rodata.*)
  }

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*)
    . = ALIGN(16);
    *(.data .data.*)
  }

  .bss : {
    . = ALIGN(16);
    __bss_start = .;
    *(.sbss .sbss.*)
    . = ALIGN(16);
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }

  PROVIDE(end = .);
}
