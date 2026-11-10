// SPDX-License-Identifier: GPL-2.0
/*
 * The board's VGA output (MMIO 0xF011_0000; klaussCPU VGA_PLAN.md and
 * MMIO_MAP.md, the runtime's mmio.h and vga.h) as /dev/fb0.
 *
 * The controller shows a fixed 640x480 @ 59.5 Hz picture.  It scans an
 * image out of DDR: HEIGHT lines of 640 pixels, or of 320 pixels shown
 * twice each way (DOUBLE), at STRIDE bytes apart from FB_BASE, either
 * RGB565 or 8-bit indices into a 256-entry RGB444 palette (BPP8).  The
 * image starts at display line VSTART; the rest is the BORDER colour.
 * FB_BASE, STRIDE, HEIGHT, VSTART and the scan-out bits of CTRL latch at
 * the start of vblank, so a pan is a tear-free flip.
 *
 * Modes (FBIOPUT_VSCREENINFO): xres 640, or 320 (doubled); 8 or 16 bpp;
 * any yres up to 480 (240 doubled), centred vertically; yres_virtual as
 * far as the buffer allows, for page flipping with FBIOPAN_DISPLAY.  The
 * default is 640x480 RGB565.  Doom's 320x200 8-bit frames fit as they are.
 *
 * Memory: the buffer is ordinary kernel memory (kseg, physically
 * contiguous), allocated at boot and mapped into user space by PFN.
 *
 * Coherency: the display reads DDR, but the CPU writes through its
 * write-back D-cache, which the hardware flushes only as a whole
 * (CACHE_CTRL FLUSH, a walk of a few thousand cycles).  So:
 *   - while /dev/fb0 is open or mapped, the vsync interrupt (source 3)
 *     flushes the cache at the start of every vblank, so what a program
 *     draws shows within a frame;
 *   - write(), a pan and FBIO_WAITFORVSYNC flush at once, and so does the
 *     last close;
 *   - nothing is flushed while the frame buffer is unused, and the vsync
 *     interrupt is off then.
 *
 * Without the controller (the emulator, bitstreams without VGA) its
 * registers read 0, so BORDER does not read back and nothing is
 * registered.  klausscpu_fb.force=1 registers /dev/fb0 anyway, without
 * touching the display: a way to test programs on the emulator.
 *
 * /sys/module/klausscpu_fb/parameters/status reads the live frame counter,
 * raster line, underflow count, vsync interrupts and flushes.
 */
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/wait.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>

#define VGA_BASE	(MMIO_BASE + 0x00110000UL)
#define VGA_CTRL	0x000
#define VGA_FB_BASE	0x008
#define VGA_STRIDE	0x010
#define VGA_STATUS	0x018
#define VGA_BORDER	0x020
#define VGA_VSTART	0x028
#define VGA_FB_ACTIVE	0x030
#define VGA_HEIGHT	0x038
#define VGA_PALETTE(n)	(0x800 + 8 * (n))

#define CTRL_SCANOUT_EN	0x01
#define CTRL_DOUBLE	0x02
#define CTRL_BPP8	0x04
#define CTRL_VSYNC_IRQ	0x10

#define STATUS_VSYNC		0x2	/* W1C */
#define STATUS_UNDERFLOW_CLR	0x4
#define STATUS_FRAME(s)		(((s) >> 16) & 0xffff)
#define STATUS_LINE(s)		(((s) >> 32) & 0x3ff)
#define STATUS_UNDERFLOW(s)	(((s) >> 48) & 0xffff)

#define CACHE_CTRL	(MMIO_BASE + 0x00050000UL)
#define CACHE_FLUSH	0x2

#define IRQ_VGA		3

#define SCREEN_W	640
#define SCREEN_H	480
/* Two full 640x480 RGB565 pages: room to flip in every mode. */
#define FB_SIZE		(SCREEN_W * SCREEN_H * 2 * 2)

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "register /dev/fb0 without a VGA controller (emulator tests)");

struct kl_fb {
	struct fb_info *info;
	void *buf;
	phys_addr_t phys;
	bool present;		/* a VGA controller answered */
	int irq;		/* 0: no vsync interrupt */
	u32 ctrl;		/* CTRL as last set, scan-out bits */
	bool blanked;
	atomic_t users;		/* open files plus mappings */
	unsigned long vsyncs;	/* vsync interrupts taken */
	unsigned long flushes;
	wait_queue_head_t vsync_wait;
	u32 pseudo_palette[16];
};

static struct kl_fb kl_fb;
static DEFINE_SPINLOCK(kl_fb_ctrl_lock);

static void __iomem *vga(unsigned int off)
{
	return (void __iomem *)(VGA_BASE + off);
}

/* Write every dirty D-cache line back to DDR.  The CPU's next cached
 * access waits for the walk, which takes far less than a vblank. */
static void kl_fb_flush(struct kl_fb *p)
{
	if (!p->present)
		return;
	writel(CACHE_FLUSH, (void __iomem *)CACHE_CTRL);
	p->flushes++;
}

static bool kl_fb_detect(void)
{
	bool found;

	writeq(0xa5c, vga(VGA_BORDER));
	found = (readq(vga(VGA_BORDER)) & 0xfff) == 0xa5c;
	writeq(0, vga(VGA_BORDER));
	return found;
}

static irqreturn_t kl_fb_irq(int irq, void *dev_id)
{
	struct kl_fb *p = dev_id;

	if (!(readq(vga(VGA_STATUS)) & STATUS_VSYNC))
		return IRQ_NONE;
	writeq(STATUS_VSYNC, vga(VGA_STATUS));
	if (atomic_read(&p->users))
		kl_fb_flush(p);
	p->vsyncs++;
	wake_up_all(&p->vsync_wait);
	return IRQ_HANDLED;
}

/* CTRL: the scan-out bits, and the vsync interrupt while the frame buffer
 * is in use. */
static void kl_fb_write_ctrl(struct kl_fb *p)
{
	unsigned long flags;
	u32 v;

	if (!p->present)
		return;
	spin_lock_irqsave(&kl_fb_ctrl_lock, flags);
	v = p->blanked ? 0 : p->ctrl;
	if (p->irq && atomic_read(&p->users)) {
		if (!(readq(vga(VGA_CTRL)) & CTRL_VSYNC_IRQ))
			writeq(STATUS_VSYNC, vga(VGA_STATUS));	/* not a stale one */
		v |= CTRL_VSYNC_IRQ;
	}
	writeq(v, vga(VGA_CTRL));
	spin_unlock_irqrestore(&kl_fb_ctrl_lock, flags);
}

/* Users: open files and mappings. */
static void kl_fb_get(struct kl_fb *p)
{
	if (atomic_inc_return(&p->users) == 1)
		kl_fb_write_ctrl(p);
}

static void kl_fb_put(struct kl_fb *p)
{
	kl_fb_flush(p);
	if (atomic_dec_and_test(&p->users))
		kl_fb_write_ctrl(p);
}

/* Wait for the start of the next vblank, after flushing the cache. */
static int kl_fb_wait_vsync(struct kl_fb *p)
{
	unsigned long seen = READ_ONCE(p->vsyncs);
	long left;

	kl_fb_flush(p);
	if (!p->irq) {
		/* force=1 without a display: pretend to 60 Hz. */
		msleep(16);
		return 0;
	}
	left = wait_event_interruptible_timeout(p->vsync_wait,
						READ_ONCE(p->vsyncs) != seen,
						msecs_to_jiffies(100));
	if (left < 0)
		return left;
	return left ? 0 : -ETIMEDOUT;
}

static u32 kl_fb_stride(const struct fb_var_screeninfo *var)
{
	return var->xres_virtual * (var->bits_per_pixel / 8);
}

/* Program the scan-out for info->var; it latches at the next vblank. */
static void kl_fb_hw_mode(struct kl_fb *p)
{
	struct fb_var_screeninfo *var = &p->info->var;
	bool dbl = var->xres == SCREEN_W / 2;
	u32 shown = dbl ? 2 * var->yres : var->yres;

	p->ctrl = CTRL_SCANOUT_EN | (dbl ? CTRL_DOUBLE : 0) |
		  (var->bits_per_pixel == 8 ? CTRL_BPP8 : 0);
	if (p->present) {
		writeq(p->phys + (u64)var->yoffset * kl_fb_stride(var), vga(VGA_FB_BASE));
		writeq(kl_fb_stride(var), vga(VGA_STRIDE));
		writeq(var->yres, vga(VGA_HEIGHT));
		writeq((SCREEN_H - shown) / 2, vga(VGA_VSTART));
	}
	kl_fb_write_ctrl(p);
}

static void kl_fb_set_bitfields(struct fb_var_screeninfo *var)
{
	var->transp = (struct fb_bitfield){ 0, 0, 0 };
	if (var->bits_per_pixel == 8) {
		var->red = (struct fb_bitfield){ 0, 8, 0 };
		var->green = var->red;
		var->blue = var->red;
	} else {
		var->red = (struct fb_bitfield){ 11, 5, 0 };
		var->green = (struct fb_bitfield){ 5, 6, 0 };
		var->blue = (struct fb_bitfield){ 0, 5, 0 };
	}
}

static int kl_fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	u32 maxh;

	if (var->xres == 0 || var->xres > SCREEN_W || var->yres == 0)
		return -EINVAL;
	var->xres = var->xres <= SCREEN_W / 2 ? SCREEN_W / 2 : SCREEN_W;
	var->xres_virtual = var->xres;
	var->bits_per_pixel = var->bits_per_pixel <= 8 ? 8 : 16;
	var->grayscale = 0;
	var->nonstd = 0;
	maxh = var->xres == SCREEN_W ? SCREEN_H : SCREEN_H / 2;
	if (var->yres > maxh)
		return -EINVAL;
	if (var->yres_virtual < var->yres)
		var->yres_virtual = var->yres;
	if ((u64)var->yres_virtual * kl_fb_stride(var) > info->fix.smem_len)
		return -EINVAL;
	var->xoffset = 0;
	if (var->yoffset > var->yres_virtual - var->yres)
		return -EINVAL;
	kl_fb_set_bitfields(var);
	/* The fixed 640x480 @ 59.5 Hz timing, for information. */
	var->pixclock = 40000;	/* ps: 25 MHz */
	var->left_margin = 48;
	var->right_margin = 16;
	var->upper_margin = 33;
	var->lower_margin = 10;
	var->hsync_len = 96;
	var->vsync_len = 2;
	var->sync = 0;
	var->vmode = FB_VMODE_NONINTERLACED;
	var->height = -1;
	var->width = -1;
	var->accel_flags = 0;
	return 0;
}

static int kl_fb_set_par(struct fb_info *info)
{
	struct kl_fb *p = info->par;
	struct fb_var_screeninfo *var = &info->var;

	info->fix.line_length = kl_fb_stride(var);
	info->fix.visual = var->bits_per_pixel == 8 ? FB_VISUAL_PSEUDOCOLOR
						    : FB_VISUAL_TRUECOLOR;
	kl_fb_flush(p);
	kl_fb_hw_mode(p);
	return 0;
}

/* 16-bit colour components to the hardware's 4 bits, rounded. */
static u32 kl_fb_c4(unsigned int c)
{
	return ((c & 0xffff) * 15 + 32767) / 65535;
}

static int kl_fb_setcolreg(unsigned int regno, unsigned int red, unsigned int green,
			   unsigned int blue, unsigned int transp, struct fb_info *info)
{
	struct kl_fb *p = info->par;

	if (regno >= 256)
		return -EINVAL;
	if (info->var.bits_per_pixel == 8) {
		/* Takes effect at once, mid-frame if need be. */
		if (p->present)
			writeq((kl_fb_c4(red) << 8) | (kl_fb_c4(green) << 4) | kl_fb_c4(blue),
			       vga(VGA_PALETTE(regno)));
		return 0;
	}
	if (regno < 16)
		p->pseudo_palette[regno] = ((red >> 11) << 11) | ((green >> 10) << 5) |
					   (blue >> 11);
	return 0;
}

static int kl_fb_blank(int blank, struct fb_info *info)
{
	struct kl_fb *p = info->par;

	p->blanked = blank != FB_BLANK_UNBLANK;
	kl_fb_hw_mode(p);
	return 0;
}

static int kl_fb_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct kl_fb *p = info->par;

	if (var->xoffset || var->yoffset > info->var.yres_virtual - info->var.yres)
		return -EINVAL;
	kl_fb_flush(p);
	if (p->present)
		writeq(p->phys + (u64)var->yoffset * info->fix.line_length, vga(VGA_FB_BASE));
	if (var->activate & FB_ACTIVATE_VBL)
		kl_fb_wait_vsync(p);
	return 0;
}

static int kl_fb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	struct kl_fb *p = info->par;
	u32 crtc;

	switch (cmd) {
	case FBIO_WAITFORVSYNC:
		if (get_user(crtc, (u32 __user *)arg))
			return -EFAULT;
		if (crtc != 0)
			return -ENODEV;
		return kl_fb_wait_vsync(p);
	}
	return -ENOTTY;
}

static int kl_fb_open(struct fb_info *info, int user)
{
	kl_fb_get(info->par);
	return 0;
}

static int kl_fb_release(struct fb_info *info, int user)
{
	kl_fb_put(info->par);
	return 0;
}

static ssize_t kl_fb_write(struct fb_info *info, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	ssize_t n = fb_sys_write(info, buf, count, ppos);

	kl_fb_flush(info->par);
	return n;
}

static void kl_fb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	sys_fillrect(info, rect);
	kl_fb_flush(info->par);
}

static void kl_fb_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	sys_copyarea(info, area);
	kl_fb_flush(info->par);
}

static void kl_fb_imageblit(struct fb_info *info, const struct fb_image *image)
{
	sys_imageblit(info, image);
	kl_fb_flush(info->par);
}

/* A mapping keeps the vsync flushes going after its file is closed. */
static void kl_fb_vm_open(struct vm_area_struct *vma)
{
	kl_fb_get(&kl_fb);
}

static void kl_fb_vm_close(struct vm_area_struct *vma)
{
	kl_fb_put(&kl_fb);
}

static const struct vm_operations_struct kl_fb_vm_ops = {
	.open	= kl_fb_vm_open,
	.close	= kl_fb_vm_close,
};

static int kl_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct kl_fb *p = info->par;
	int err;

	err = vm_iomap_memory(vma, p->phys, info->fix.smem_len);
	if (err)
		return err;
	vma->vm_ops = &kl_fb_vm_ops;
	kl_fb_vm_open(vma);
	return 0;
}

static const struct fb_ops kl_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_open	= kl_fb_open,
	.fb_release	= kl_fb_release,
	.fb_read	= fb_sys_read,
	.fb_write	= kl_fb_write,
	.fb_check_var	= kl_fb_check_var,
	.fb_set_par	= kl_fb_set_par,
	.fb_setcolreg	= kl_fb_setcolreg,
	.fb_blank	= kl_fb_blank,
	.fb_pan_display	= kl_fb_pan_display,
	.fb_fillrect	= kl_fb_fillrect,
	.fb_copyarea	= kl_fb_copyarea,
	.fb_imageblit	= kl_fb_imageblit,
	.fb_ioctl	= kl_fb_ioctl,
	.fb_mmap	= kl_fb_mmap,
};

static int kl_fb_status_get(char *buf, const struct kernel_param *kp)
{
	struct kl_fb *p = &kl_fb;
	u64 s = p->present ? readq(vga(VGA_STATUS)) : 0;

	return sysfs_emit(buf, "%s frame %llu line %llu underflows %llu vsyncs %lu flushes %lu users %d active %08llx\n",
			  p->info ? (p->present ? "vga" : "forced") : "none",
			  STATUS_FRAME(s), STATUS_LINE(s), STATUS_UNDERFLOW(s),
			  p->vsyncs, p->flushes, atomic_read(&p->users),
			  p->present ? readq(vga(VGA_FB_ACTIVE)) : 0ULL);
}

static const struct kernel_param_ops kl_fb_status_ops = {
	.get	= kl_fb_status_get,
};
module_param_cb(status, &kl_fb_status_ops, NULL, 0444);

static int __init kl_fb_init(void)
{
	struct kl_fb *p = &kl_fb;
	struct fb_info *info;
	int err;

	p->present = kl_fb_detect();
	if (!p->present && !force) {
		pr_info("klausscpu-fb: no VGA controller in this bitstream\n");
		return 0;
	}
	init_waitqueue_head(&p->vsync_wait);
	p->buf = alloc_pages_exact(FB_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!p->buf)
		return -ENOMEM;
	p->phys = __pa(p->buf);

	info = framebuffer_alloc(0, NULL);
	if (!info) {
		err = -ENOMEM;
		goto free_buf;
	}
	p->info = info;
	info->par = p;
	info->fbops = &kl_fb_ops;
	info->flags = FBINFO_VIRTFB | FBINFO_HWACCEL_YPAN;
	info->screen_buffer = p->buf;
	info->screen_size = FB_SIZE;
	info->pseudo_palette = p->pseudo_palette;
	strscpy(info->fix.id, "klausscpu-vga", sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.smem_start = p->phys;
	info->fix.smem_len = FB_SIZE;
	info->fix.ypanstep = 1;
	info->fix.accel = FB_ACCEL_NONE;

	info->var.xres = SCREEN_W;
	info->var.yres = SCREEN_H;
	info->var.bits_per_pixel = 16;
	info->var.activate = FB_ACTIVATE_NOW;
	err = kl_fb_check_var(&info->var, info);
	if (err)
		goto release;
	err = fb_alloc_cmap(&info->cmap, 256, 0);
	if (err)
		goto release;

	if (p->present) {
		writeq(0, vga(VGA_BORDER));
		writeq(STATUS_UNDERFLOW_CLR | STATUS_VSYNC, vga(VGA_STATUS));
		err = request_irq(klausscpu_irq(IRQ_VGA), kl_fb_irq, 0, "klausscpu-vga", p);
		if (err)
			pr_warn("klausscpu-fb: no vsync interrupt (%d)\n", err);
		else
			p->irq = klausscpu_irq(IRQ_VGA);
	}
	kl_fb_set_par(info);

	err = register_framebuffer(info);
	if (err)
		goto free_irq;
	fb_info(info, "%s %ux%u RGB565, %u KiB at %08llx%s\n", info->fix.id,
		info->var.xres, info->var.yres, FB_SIZE / 1024, (u64)p->phys,
		p->present ? (p->irq ? ", vsync IRQ" : ", no vsync IRQ")
			   : " (no VGA controller: force=1)");
	return 0;

free_irq:
	if (p->irq)
		free_irq(p->irq, p);
	p->irq = 0;
	fb_dealloc_cmap(&info->cmap);
release:
	framebuffer_release(info);
	p->info = NULL;
free_buf:
	free_pages_exact(p->buf, FB_SIZE);
	p->buf = NULL;
	if (p->present)
		writeq(0, vga(VGA_CTRL));
	return err;
}
device_initcall(kl_fb_init);
