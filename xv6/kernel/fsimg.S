// klausscpu: the file system image, linked into the kernel (ramdisk.c).
    .section .data.fsimg, "aw"
    .p2align 12
    .globl  fs_img_start
fs_img_start:
    .incbin FS_IMG
    .globl  fs_img_end
fs_img_end:
