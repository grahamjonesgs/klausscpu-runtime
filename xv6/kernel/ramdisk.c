//
// klausscpu: a RAM disk in place of virtio_disk.c (XV6_PLAN.md).
//
// The file system image built by mkfs (fs.img) is linked into the kernel
// (fsimg.S) and the "disk" is that memory, so writes last until reset.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "klauss.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "defs.h"

extern char fs_img_start[], fs_img_end[];

void
ramdisk_init(void)
{
  if (fs_img_end - fs_img_start != FSSIZE * BSIZE)
    panic("ramdisk: fs.img size");
}

// read (write == 0) or write block b->blockno.
void
ramdisk_rw(struct buf *b, int write)
{
  if (b->blockno >= FSSIZE)
    panic("ramdisk: blockno");
  char *addr = fs_img_start + (uint64)b->blockno * BSIZE;
  if (write)
    memmove(addr, b->data, BSIZE);
  else
    memmove(b->data, addr, BSIZE);
}
