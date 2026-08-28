# AMP core 2 — software side

Core 2 is a second KlaussCPU `pipeline_core` at effective 50 MHz with a
private 64 KB BRAM and shared access to DDR (uncached, plus an 8 KB read
cache over the 1 MB *text window* at `0x07E0_0000`).  Design + hardware
status: `AMP_CORE2_PLAN.md` in the FPGA repo; register map: `mmio.h`
(`REG_C2_*`).

Programming model = **AMP, two programs, one shared address map**:

- `core2/main.c` (+ `crt0_core2.c`, `core2.ld`, `lwipopts.h`) is core 2's
  own image: ordinary C, same toolchain, `printf` goes to a UART-compatible
  console (a log FIFO core 1 drains).  Text/rodata in the DDR window, data/
  bss/heap/stack in local BRAM.
- `baremetal/programs/core1_amp_host.c` is core 1's side: embeds `core2.bin`,
  memcpys it to `C2_TEXT_ENTRY`, cache-FLUSHes (the coherency contract),
  hands core 2 the LiteEth (`REG_C2_ETH_OWNER=1`), starts it, forwards its
  console.

Build (from `baremetal/`):

    make core1_amp_host        # -> core2.elf -> core2.bin -> core2_image.h -> core1_amp_host.elf

Run: load `core1_amp_host.elf` on the board (klausscc serial); core 2 does
DHCP (static 192.168.68.60 fallback) and answers ping.

Coherency rules for anything shared through DDR: core 1 FLUSHes after
writing / INVALIDATEs before reading data core 2 wrote; core 2 needs no
maintenance (uncached window).  Never write into the text window while
core 2 runs.
