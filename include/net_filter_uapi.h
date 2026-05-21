#ifndef NET_FILTER_UAPI_H
#define NET_FILTER_UAPI_H

#include "telemetry_uapi.h"


/* ---- Zone B Netfilter Payload Slot (Exactly 32 Bytes / 8-Byte Aligned) ---- */
/* Netbuffer overflow reset flag - signals consumer to roll forward past an overflow */
#define TELEMETRY_FLAG_NET_OVERFLOW_RESET (1U << 1)
#define TELEMETRY_FLAG_NET_THREAD_ACTIVE (1U << 2) /* Background consumer thread is draining */ 


// Core 16-Byte Network Struct
struct net_packet_metric {
    telemetry_u32 src_ip;
    telemetry_u32 dest_ip;
    telemetry_u16 src_port;
    telemetry_u16 dest_port;
    telemetry_u32 pkt_len;
} __attribute__((packed));

typedef struct {
    struct net_packet_metric packet_metrics; /* Offsets 0..15  (16 Bytes Core Payload) */
    volatile telemetry_u32  slot_ticket;    /* Offsets 16..19 (4 Bytes Validation Ticket) */
    unsigned char           reserved[12];    /* Offsets 20..31 (12 Bytes Alignment Padding) */
} net_payload_slot_t;

typedef struct {
    net_payload_slot_t slots[TELEMETRY_NETFILTER_RING_BUFFER_CAPACITY / sizeof(net_payload_slot_t)];
} telemetry_payload_pool_t;

#define NETFILTER_SLOT_SIZE sizeof(net_payload_slot_t)
#define NETFILTER_SLOT_CAPACITY (TELEMETRY_NETFILTER_RING_BUFFER_CAPACITY / NETFILTER_SLOT_SIZE)

#ifndef __KERNEL__

/*
 * Global network telemetry context
 */

/* g_nt_ctx accessor functions */
telemetry_ctx_t *nt_ctx_create(void);
void nt_ctx_destroy(telemetry_ctx_t *ctx);


typedef enum {
    MODE_SCATTER_GATHER = 0,
    MODE_SKIP_AHEAD     = 1
} consumer_mode_t;

typedef struct {
    telemetry_ctx_t *ctx;
    consumer_mode_t mode;
    volatile int run_signal;
} consumer_thread_args_t;

#endif /* __KERNEL__ */

#endif /* NET_FILTER_UAPI_H */