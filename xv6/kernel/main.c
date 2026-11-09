#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "klauss.h"
#include "defs.h"

// klausscpu: boot.S jumps here in supervisor mode, in kseg, with
// translation on.  One CPU.
void
main()
{
  consoleinit();
  printkinit();
  printk("\n");
  printk("xv6 kernel is booting\n");
  printk("\n");
  uint32 f = CPU_FEATURES;
  if ((f & (CPU_FEAT_EXC | CPU_FEAT_TLB | CPU_FEAT_LRSC)) !=
      (CPU_FEAT_EXC | CPU_FEAT_TLB | CPU_FEAT_LRSC))
    panic("this core has no exceptions, TLB or LR/SC (CPU_FEATURES)");
  printk("CPU_FEATURES 0x%x: %d TLB entries\n", f, CPU_FEAT_TLB_ENTRIES(f));
  kinit();         // physical page allocator
  procinit();      // process table
  trapinit();      // trap vectors
  trapinithart();  // install the vectors, TLB and timer
  binit();         // buffer cache
  iinit();         // inode table
  fileinit();      // file table
  ramdisk_init();  // the file system image
  userinit();      // first user process

  scheduler();
}
