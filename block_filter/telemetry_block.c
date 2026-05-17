#include <linux/module.h>
#include <linux/kernel.h>
#include "telemetry.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Praveen Ankala");
MODULE_DESCRIPTION("Block Storage Telemetry Hook");

static int __init block_filter_init(void) {
    pr_info("Block Filter: Initializing Block Storage Telemetry Hook...\n");
    return 0;
}

static void __exit block_filter_exit(void) {
    pr_info("Block Filter: Unregistering Block Storage Telemetry Hook...\n");
}

module_init(block_filter_init);
module_exit(block_filter_exit);
