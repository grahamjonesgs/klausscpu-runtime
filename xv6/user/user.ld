/* klausscpu: user programs (XV6_PLAN.md).  Linked at VA 0, entry ulib.c's
 * start().  Two segments, each starting on a 64 KiB page (exec maps whole
 * PGSIZE pages, read-execute for text and read-write for data). */
OUTPUT_ARCH( "klausscpu" )
ENTRY( start )

PHDRS
{
  text PT_LOAD FLAGS(5);  /* R X */
  data PT_LOAD FLAGS(6);  /* R W */
}

SECTIONS
{
  . = 0x0;

  .text : {
    *(.text .text.*)
  } :text

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*)
    . = ALIGN(16);
    *(.rodata .rodata.*)
  } :text

  . = ALIGN(0x10000);
  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*)
    . = ALIGN(16);
    *(.data .data.*)
  } :data

  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*)
    . = ALIGN(16);
    *(.bss .bss.*)
    *(COMMON)
  } :data

  PROVIDE(end = .);

  /DISCARD/ : {
    *(.eh_frame .eh_frame.*)
  }
}
