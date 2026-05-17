#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <telemetry_uapi.h>

// --- Netfilter telemetry functions ---

/*
 * Pushes a net_packet_metric to the shared character device buffer.
 * This function is called from the network stack's softirq context.
 * 
 * Plays "nice" with spinlocks by using non-blocking operations for multiple producers.
 */
int telemetry_shared_char_dev_push_metric(struct net_packet_metric *metric);

/*
 * Pushes a net_packet_metric to the net device buffer.
 * This function is called from the network stack's hardirq or softirq context.
 * Dedicated ring of packet metrics, multiple producers can write to it.
 * hook: called from netfilter hooks.
 */
int telemetry_net_dev_push_metric(struct net_packet_metric *metric);

// --- Block device telemetry functions ---

// --- TODO: Memory Management telemetry functions ---

#endif
