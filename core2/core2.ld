/*
 * core2.ld — AMP core 2 linker script (AMP_CORE2_PLAN.md §4).
 *
 * Core 2 sees DDR at the same addresses as core 1 (above its first 64 KB)
 * plus a private 64 KB local BRAM at 0x0000_0000.
 *
 *   TEXT  0x07E0_0000 +1 MB  DDR "text window": read-cached on core 2,
 *                            immutable after core 1 loads + flushes it.
 *                            .text/.rodata live here; .data's initial image
 *                            is stored here too (LMA) and copied to BRAM by
 *                            crt0_core2.
 *   LRAM  0x0000_0000 +128 KB local BRAM: .data/.bss (from 0x20 — the first
 *                            32 bytes are the heap header, as on core 1),
 *                            picolibc heap, 16 KB stack at the top.
 *
 * The flat binary (llvm-objcopy -O binary) starts at 0x07E0_0020 and is one
 * contiguous blob: core 1 memcpys it to C2_TEXT_ENTRY, flushes, starts core 2
 * there.  Hardware resets core 2's SP to 0x0002_0000 (SP_RESET parameter).
 */
ENTRY(_start)

MEMORY {
    TEXT : ORIGIN = 0x07E00000, LENGTH = 1M
    LRAM : ORIGIN = 0x00000000, LENGTH = 128K
}

SECTIONS {
    .text 0x07E00020 : {
        KEEP(*(.text._start))
        *(.text*)
        *(.rodata*)
    } > TEXT

    .data 0x00000020 : ALIGN(8) {
        __data_start = .;
        *(.data*)
        __data_end = .;
    } > LRAM AT> TEXT
    __data_load_start = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(8) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > LRAM

    __heap_start = ALIGN(8);
    __heap_end   = ORIGIN(LRAM) + LENGTH(LRAM) - 0x6000;   /* 24 KiB stack (lwIP callback depth + printf) */
    _end         = .;
    _stack_top   = ORIGIN(LRAM) + LENGTH(LRAM);            /* 0x00020000 */
}
