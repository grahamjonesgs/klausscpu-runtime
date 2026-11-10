// SPDX-License-Identifier: GPL-2.0
/*
 * The board's SD card as a block device, mmcblk0 with partitions
 * mmcblk0p1...; the card protocol is in sd_spi.c.  Polled: queue_rq runs
 * the transfer itself (BLK_MQ_F_BLOCKING).  A request's sectors are
 * contiguous, so each goes to the card as one multi-block command, through
 * a bounce buffer that holds the largest request (max_hw_sectors).
 * Without a card, or on an emulator without the controller (its registers
 * read 0), the probe fails and no disk is registered.
 *
 * Switches for A/B measurements, on by default (kernel command line
 * klausscpu_sd.<name>=0, or /sys/module/klausscpu_sd/parameters/<name>):
 *   multiblock  CMD18/CMD25 for a request's run of sectors (else CMD17/24
 *               per sector);
 *   pipeline    the pipelined byte loops (else one byte at a time).
 * Counters there too: late_reads, multi_fallbacks, retries.
 */
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/bvec.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "sd_spi.h"

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "klausscpu_sd."

#define SD_MAX_SECTORS	128		/* 64 KiB per request */

static bool sd_multiblock = true;
module_param_named(multiblock, sd_multiblock, bool, 0644);
MODULE_PARM_DESC(multiblock, "One multi-block command per request");
static bool sd_pipeline = true;
module_param_named(pipeline, sd_pipeline, bool, 0644);
MODULE_PARM_DESC(pipeline, "Pipelined data-block byte loops");
module_param_named(late_reads, sd_spi_stats.late, ulong, 0444);
module_param_named(multi_fallbacks, sd_spi_stats.multi_fallbacks, ulong, 0444);
module_param_named(retries, sd_spi_stats.retries, ulong, 0444);

static DEFINE_MUTEX(sd_lock);		/* the card, and sd_bounce */
static u8 *sd_bounce;
static int sd_major;
static struct gendisk *sd_disk;
static struct blk_mq_tag_set sd_tags;

static blk_status_t sd_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	bool write = req_op(rq) == REQ_OP_WRITE;
	unsigned int n = blk_rq_sectors(rq), off;
	blk_status_t st = BLK_STS_OK;
	struct req_iterator iter;
	struct bio_vec bv;
	int rc;

	blk_mq_start_request(rq);
	if (req_op(rq) != REQ_OP_READ && !write) {
		/* Writes complete on the card before queue_rq returns. */
		st = req_op(rq) == REQ_OP_FLUSH ? BLK_STS_OK : BLK_STS_NOTSUPP;
		goto out;
	}
	if (WARN_ON_ONCE(n > SD_MAX_SECTORS)) {
		st = BLK_STS_IOERR;
		goto out;
	}
	mutex_lock(&sd_lock);
	if (write) {
		off = 0;
		rq_for_each_segment(bv, rq, iter) {
			memcpy_from_bvec(sd_bounce + off, &bv);
			off += bv.bv_len;
		}
		rc = sd_spi_write(blk_rq_pos(rq), sd_bounce, n,
				  READ_ONCE(sd_multiblock), READ_ONCE(sd_pipeline));
	} else {
		rc = sd_spi_read(blk_rq_pos(rq), sd_bounce, n,
				 READ_ONCE(sd_multiblock), READ_ONCE(sd_pipeline));
		if (!rc) {
			off = 0;
			rq_for_each_segment(bv, rq, iter) {
				memcpy_to_bvec(&bv, sd_bounce + off);
				off += bv.bv_len;
			}
		}
	}
	mutex_unlock(&sd_lock);
	if (rc)
		st = BLK_STS_IOERR;
out:
	blk_mq_end_request(rq, st);
	return BLK_STS_OK;
}

static const struct blk_mq_ops sd_mq_ops = {
	.queue_rq = sd_queue_rq,
};

static const struct block_device_operations sd_fops = {
	.owner = THIS_MODULE,
};

static int __init klausscpu_sd_init(void)
{
	struct queue_limits lim = {
		.logical_block_size	= 512,
		.max_hw_sectors		= SD_MAX_SECTORS,
		/* The bounce buffer takes any layout: up to a segment a sector. */
		.max_segments		= SD_MAX_SECTORS,
	};
	sector_t sectors;
	int err;

	if (sd_spi_init()) {
		pr_info("klausscpu-sd: no card\n");
		return 0;
	}
	sectors = sd_spi_capacity();
	if (!sectors) {
		pr_warn("klausscpu-sd: card does not report its size\n");
		return 0;
	}
	sd_bounce = kmalloc(SD_MAX_SECTORS * 512, GFP_KERNEL);
	if (!sd_bounce)
		return -ENOMEM;

	sd_major = register_blkdev(0, "klausscpu-sd");
	if (sd_major < 0) {
		err = sd_major;
		goto out_bounce;
	}

	sd_tags.ops = &sd_mq_ops;
	sd_tags.nr_hw_queues = 1;
	sd_tags.queue_depth = 4;
	sd_tags.numa_node = NUMA_NO_NODE;
	sd_tags.flags = BLK_MQ_F_BLOCKING;
	err = blk_mq_alloc_tag_set(&sd_tags);
	if (err)
		goto out_blkdev;

	sd_disk = blk_mq_alloc_disk(&sd_tags, &lim, NULL);
	if (IS_ERR(sd_disk)) {
		err = PTR_ERR(sd_disk);
		goto out_tags;
	}
	sd_disk->major = sd_major;
	sd_disk->first_minor = 0;
	sd_disk->minors = 16;
	sd_disk->fops = &sd_fops;
	strscpy(sd_disk->disk_name, "mmcblk0", DISK_NAME_LEN);
	set_capacity(sd_disk, sectors);
	err = add_disk(sd_disk);
	if (err)
		goto out_disk;
	pr_info("klausscpu-sd: mmcblk0, %llu MiB (%s), multiblock %d, pipeline %d\n",
		(u64)sectors >> 11, sd_spi_blockaddr() ? "SDHC/SDXC" : "SDSC",
		sd_multiblock, sd_pipeline);
	return 0;

out_disk:
	put_disk(sd_disk);
out_tags:
	blk_mq_free_tag_set(&sd_tags);
out_blkdev:
	unregister_blkdev(sd_major, "klausscpu-sd");
out_bounce:
	kfree(sd_bounce);
	return err;
}
device_initcall(klausscpu_sd_init);
