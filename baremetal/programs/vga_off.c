// vga_off.c — put the VGA controller back to its reset state (test pattern, no
// DRAM scan-out).  The tools/validate harness runs it on the board before the
// board suite: the controller keeps its state when the CPU loads a new
// program, so after a Zephyr GUI app (or doom, test_vga) the scan-out stays
// on, and its DRAM reads double the CPU's cache-miss penalty (measured on
// klaussCPU 3508f87: avg_miss_pen 29 -> 56 cycles, perf_baseline mem_stream
// +31%, ptr_chase +50%).  CTRL takes effect at the next vblank.
#include <stdint.h>
#include <stdio.h>
#include "../../mmio.h"

int main(void) {
  printf("vga_off: CTRL was 0x%x\n", (unsigned)REG_VGA_CTRL);
  REG_VGA_CTRL = VGA_CTRL_TEST_PATTERN;
  for (volatile int i = 0; i < 1000000; i++) {}   // > 2 frames at 100 MHz
  printf("vga_off: CTRL now 0x%x\n", (unsigned)REG_VGA_CTRL);
  return 0;
}
