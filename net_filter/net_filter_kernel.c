#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/skbuff.h>
#include "net_filter.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Praveen Ankala");
MODULE_DESCRIPTION("Network Stack Telemetry Hook");

// Imported directly via Symbol Table cross-linking
extern shared_telemetry_ring *ring_ptr; 
// Registration handle for our Netfilter hook
static struct nf_hook_ops nfho;

// The Core Packet Interception Callback
static unsigned int packet_telemetry_hook(void *priv, 
                                          struct sk_buff *skb, 
                                          const struct nf_hook_state *state) {
    struct iphdr *network_header;
    struct tcphdr *transport_header;

    /*Metric structures*/
    net_tenant_lane_t *net_lane;
    struct net_packet_metric metric;
    net_payload_slot_t *slot;
    telemetry_payload_pool_t *net_buffer;
    
    uint32_t my_ticket, current_rd, current_wr, slot_idx;

    int ret;

    // Pass through if the buffer is empty
    if (!skb || !ring_ptr) return NF_ACCEPT;
    
    net_buffer = (telemetry_payload_pool_t *)&ring_ptr->net_buffer;

    // Extract the Network Layer (IPv4) Header safely
    network_header = ip_hdr(skb);
    if (!network_header || network_header->protocol != IPPROTO_TCP) return NF_ACCEPT;

    // Isolate TCP Traffic for our telemetry target
    // Extract the Transport Layer (TCP) Header using skb offset helpers
    transport_header = tcp_hdr(skb);
    if (!transport_header) return NF_ACCEPT;

    net_lane = (net_tenant_lane_t *)&ring_ptr->net_ctl;

    // 1. Atomically resolve and claim a slot transaction tracking number 
    my_ticket = arch_atomic_fetch_add(1, (atomic_t *)&net_lane->tx.ticket_dispenser);

    // 2. Validate tracking boundary states (lap detection)
    current_rd = smp_load_acquire(&net_lane->rx.rd_idx);

    // Convert ports from Network Byte Order (Big Endian) to Host Order (Little Endian)
    unsigned int src_port = ntohs(transport_header->source);
    unsigned int dest_port = ntohs(transport_header->dest);
    unsigned int pkt_len   = ntohs(network_header->tot_len);

 //   pr_info("Net Filter: Intercepted TCP | Src Port: %u -> Dest Port: %u | Length: %u bytes\n", 
 //           src_port, dest_port, pkt_len);
  
#if 0
    if ((int32_t)(my_ticket - current_rd) >= NETFILTER_SLOT_CAPACITY) {
        pr_info ("Net Filter: Overflow detected, going past rd_idx\n");
  //      smp_store_release(&net_lane->ctrl.state_flags, net_lane->ctrl.state_flags | TELEMETRY_FLAG_NET_OVERFLOW_RESET);
  //      return NF_ACCEPT; // Overrun safety drop
    }
#endif


    // 3. Project payload safely into Zone B
    slot_idx = my_ticket % NETFILTER_SLOT_CAPACITY;
    slot = &net_buffer->slots[slot_idx];

    slot->packet_metrics.src_ip   = network_header->saddr;
    slot->packet_metrics.dest_ip  = network_header->daddr;
    slot->packet_metrics.src_port = transport_header->source;
    slot->packet_metrics.dest_port = transport_header->dest;
    slot->packet_metrics.pkt_len  = network_header->tot_len;

    // 4. Memory Fence: Commit structural payload before changing validation transaction identifier
    smp_wmb();

    // 5. Release validation ticket stamp
    slot->slot_ticket = my_ticket;

    // 6. Push optimistic index hint forward to update visibility bounds
    current_wr = smp_load_acquire(&net_lane->tx.wr_idx);
    while ((((int32_t)(slot_idx - current_wr + NETFILTER_SLOT_CAPACITY)) % NETFILTER_SLOT_CAPACITY) >= 0) {
        
        // Save what we EXPECT to find in memory right now
        uint32_t expected_val = current_wr;
        
        // Try the atomic swap
        current_wr = cmpxchg(&net_lane->tx.wr_idx, expected_val, slot_idx+1);
        
        
        // SUCCESS: If the old value matches what we expected, the swap happened!
        if (current_wr == expected_val) {
           // pr_info("Net Filter: wr_idx update successful from %u to %u to %u\n", expected_val, current_wr, slot_idx+1);
            break; 
        }
        
        // FAILURE: Someone else changed it. current_wr now holds that new value.
        // The while loop will naturally re-evaluate the distance on the next pass.
        pr_info("Net Filter: wr_idx update failed, collision detected from old: %u to new: %u to target: %u\n", expected_val, current_wr, slot_idx+1);
    }

    return NF_ACCEPT; // Tell the kernel to let the packet continue down the stack normally
}


static int __init net_filter_init(void) {
    pr_info("Net Filter: Initializing Stack Telemetry Hook...\n");

    // Configure the Netfilter operation parameters
    nfho.hook     = packet_telemetry_hook;   // Our intercept function
    nfho.hooknum  = NF_INET_PRE_ROUTING;     // First gate after packet ingestion
    nfho.pf       = PF_INET;                 // IPv4 protocol family
    nfho.priority = NF_IP_PRI_FIRST;         // Highest priority execution

    // Register the hook with the global network namespace
    if (nf_register_net_hook(&init_net, &nfho) != 0) {
        pr_err("Net Filter: Failed to register hook structural layout\n");
        return -EFAULT;
    }

    return 0;
}

static void __exit net_filter_exit(void) {
    pr_info("Net Filter: Unregistering Stack Telemetry Hook...\n");
    nf_unregister_net_hook(&init_net, &nfho);
}

module_init(net_filter_init);
module_exit(net_filter_exit);