#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/kprobes.h>
#include "block_filter.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Praveen Ankala");
MODULE_DESCRIPTION("Block Storage Telemetry Hook");

extern shared_telemetry_ring *ring_ptr;

static struct kprobe block_rq_issue_kprobe;

static void block_telemetry_commit_metric(const struct block_io_metric *metric)
{
    tenant_lane_t *block_lane;
    block_payload_pool_t *block_pool;
    block_payload_slot_t *slot;
    telemetry_u32 my_ticket;
    telemetry_u32 slot_idx;

    if (!metric || !ring_ptr || BLOCKFILTER_SLOT_CAPACITY == 0) {
        return;
    }

    block_lane = &ring_ptr->block_ctl;
    block_pool = (block_payload_pool_t *)ring_ptr->block_buffer;

    my_ticket = arch_atomic_fetch_add(1, (atomic_t *)&block_lane->tx.wr_idx);
    slot_idx = my_ticket % BLOCKFILTER_SLOT_CAPACITY;
    slot = &block_pool->slots[slot_idx];

    slot->io_metrics = *metric;
    smp_wmb();
    slot->slot_ticket = my_ticket;

}

static void block_telemetry_bio_submit(struct bio *bio)
{
    struct block_io_metric metric;
    sector_t sector;
    unsigned int bytes;
    dev_t dev;

    if (!bio || !bio->bi_bdev) {
        return;
    }

    sector = bio->bi_iter.bi_sector;
    bytes = bio->bi_iter.bi_size;
    dev = bio_dev(bio);

    metric.dev_major = MAJOR(dev);
    metric.dev_minor = MINOR(dev);
    metric.sector_low = (telemetry_u32)sector;
    metric.sector_high = (telemetry_u32)(sector >> 32);
    metric.nr_sectors = bytes >> 9;
    metric.bytes = bytes;
    metric.op = bio_op(bio);
    metric.flags = bio->bi_opf;

    block_telemetry_commit_metric(&metric);
}

static int block_rq_issue_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
    struct bio *bio;

#if defined(CONFIG_X86_64)
    bio = (struct bio *)regs->di;
#elif defined(CONFIG_ARM64)
    bio = (struct bio *)regs->regs[0];
#else
    bio = NULL;
#endif

    block_telemetry_bio_submit(bio);
    return 0;
}

static int __init block_filter_init(void)
{
    int ret;

    pr_info("Block Filter: Initializing Block Storage Telemetry Hook...\n");

    block_rq_issue_kprobe.symbol_name = "submit_bio";
    block_rq_issue_kprobe.pre_handler = block_rq_issue_pre_handler;

    ret = register_kprobe(&block_rq_issue_kprobe);
    if (ret) {
        pr_err("Block Filter: Failed to register submit_bio kprobe: %d\n", ret);
        return ret;
    }

    pr_info("Block Filter: Successfully registered submit_bio kprobe\n");
    return 0;
}

static void __exit block_filter_exit(void)
{
    unregister_kprobe(&block_rq_issue_kprobe);
    pr_info("Block Filter: Unregistering Block Storage Telemetry Hook...\n");
}

module_init(block_filter_init);
module_exit(block_filter_exit);
