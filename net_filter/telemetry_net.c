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

// Registration handle for our Netfilter hook
static struct nf_hook_ops nfho;

// The Core Packet Interception Callback
static unsigned int packet_telemetry_hook(void *priv, 
                                          struct sk_buff *skb, 
                                          const struct nf_hook_state *state) {
    struct iphdr *network_header;
    struct tcphdr *transport_header;
    struct net_packet_metric metric;
    int ret;

    if (!skb) return NF_ACCEPT; // Pass through if the buffer is empty

    // Extract the Network Layer (IPv4) Header safely
    network_header = ip_hdr(skb);
    if (!network_header) return NF_ACCEPT;

    // Isolate TCP Traffic for our telemetry target
    if (network_header->protocol == IPPROTO_TCP) {
        // Extract the Transport Layer (TCP) Header using skb offset helpers
        transport_header = tcp_hdr(skb);
        if (transport_header) {
            // Convert ports from Network Byte Order (Big Endian) to Host Order (Little Endian)
            unsigned int src_port = ntohs(transport_header->source);
            unsigned int dest_port = ntohs(transport_header->dest);
            unsigned int pkt_len   = ntohs(network_header->tot_len);

            pr_info("Net Filter: Intercepted TCP | Src Port: %u -> Dest Port: %u | Length: %u bytes\n", 
                    src_port, dest_port, pkt_len);
            
            // NEXT STEP ARCHITECTURE:
            // Create a net_packet_metric struct and push it to the ring buffer
            metric.src_ip = network_header->saddr; // Keep in network byte order for user space mapping
            metric.dest_ip = network_header->daddr;
            metric.src_port = transport_header->source;
            metric.dest_port = transport_header->dest;
            metric.pkt_len = network_header->tot_len; // Endian-safe raw data transfers
            
            // This is where we will write packet headers straight into shared char ring buffer
            // to send telemetry data up to your zero-copy consumer app!
            // Call the exported symbol to push data down the high-speed pipe
            ret = telemetry_shared_char_dev_push_metric(&metric);
            if (ret == -ENOSPC) {
                // Buffer is full, telemetry frame dropped silently to maintain network throughput
                pr_warn("Net Filter: Telemetry buffer full, packet dropped\n");
            }

            // TODO: Alternative path to inject into the net_buffer tenant ring buffer 
            // instead of the shared char ring buffer
        }
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