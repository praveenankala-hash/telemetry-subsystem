
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/smp.h>
#include <linux/atomic.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/skbuff.h>
#include "net_filter_uapi.h"
#include "telemetry.h"
#include "telemetry_core.h"

/* ---------------------------------------------------------------------------------- */
/* /dev/telemetry_net SPECIALLY FORKED CAT INTERACTIVE READ OPERATION                 */
/* ---------------------------------------------------------------------------------- */
ssize_t telemetry_net_read(struct file *file, char __user *user_buf, 
                                  size_t count, loff_t *ppos)
{
    char *page_buf;
    int bytes_written = 0;
    int i;
    telemetry_payload_pool_t *pool;

    if (!ring_ptr) return -ENODEV;
    pool = (telemetry_payload_pool_t *)&ring_ptr->net_buffer;

    // THE CAT FIX: If ppos > 0, we already dumped the full snapshot for this command.
    // Return 0 (EOF) so 'cat' terminates gracefully instead of looping forever.
    if (*ppos > 0) return 0;

    // Allocate a temporary kernel page to format our raw snapshot string safely
    page_buf = (char *)get_zeroed_page(GFP_KERNEL);
    if (!page_buf) return -ENOMEM;

    // Format header block
    bytes_written += snprintf(page_buf + bytes_written, PAGE_SIZE - bytes_written,
        "=== TELEMETRY RAW MATRIX SNAPSHOT (CAPACITY: %lu) ===\n", NETFILTER_SLOT_CAPACITY);

    // Unconditionally loop through the exact structural layout bounds of the ring
    for (i = 0; i < NETFILTER_SLOT_CAPACITY; i++) {
        net_payload_slot_t *slot = &pool->slots[i];
        struct net_packet_metric *m = &slot->packet_metrics;

        bytes_written += snprintf(page_buf + bytes_written, PAGE_SIZE - bytes_written,
            "SLOT:%02d | TICKET:%u | %pI4:%u -> %pI4:%u | LEN:%u\n",
            i, 
            smp_load_acquire(&slot->slot_ticket),
            &m->src_ip, ntohs(m->src_port),
            &m->dest_ip, ntohs(m->dest_port), 
            ntohs(m->pkt_len));
    }

    bytes_written += snprintf(page_buf + bytes_written, PAGE_SIZE - bytes_written,
        "=== END OF TELEMETRY SNAPSHOT ===\n");

    // Enforce user-provided memory container limits before copying
    if (bytes_written > count) {
        free_page((unsigned long)page_buf);
        return -EINVAL;
    }

    // Unconditionally flush the raw matrix state out to user space
    if (copy_to_user(user_buf, page_buf, bytes_written)) {
        free_page((unsigned long)page_buf);
        return -EFAULT;
    }

    free_page((unsigned long)page_buf);

    // Advance ppos to signal to the next internal loop iteration that data has been sent.
    *ppos += bytes_written;
    // Return the size of the block we just generated. We intentionally 
    // leave *ppos untouched so subsequent read invocations simply fire again.
    return bytes_written;
}