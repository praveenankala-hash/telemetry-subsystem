#ifndef BLOCK_FILTER_UAPI_H
#define BLOCK_FILTER_UAPI_H

#include "telemetry_uapi.h"

#define TELEMETRY_FLAG_BLOCK_OVERFLOW_RESET (1U << 3)
#define TELEMETRY_FLAG_BLOCK_THREAD_ACTIVE  (1U << 4)

struct block_io_metric {
    telemetry_u32 dev_major;
    telemetry_u32 dev_minor;
    telemetry_u32 sector_low;
    telemetry_u32 sector_high;
    telemetry_u32 nr_sectors;
    telemetry_u32 bytes;
    telemetry_u32 op;
    telemetry_u32 flags;
} __attribute__((packed));

typedef struct {
    struct block_io_metric io_metrics;
    volatile telemetry_u32 slot_ticket;
    unsigned char reserved[28];
} block_payload_slot_t;

typedef struct {
    block_payload_slot_t slots[TELEMETRY_BLOCKFILTER_RING_BUFFER_CAPACITY / sizeof(block_payload_slot_t)];
} block_payload_pool_t;

#define BLOCKFILTER_SLOT_SIZE sizeof(block_payload_slot_t)
#define BLOCKFILTER_SLOT_CAPACITY (TELEMETRY_BLOCKFILTER_RING_BUFFER_CAPACITY / BLOCKFILTER_SLOT_SIZE)

#ifndef __KERNEL__
#include "telemetry_client.h"

typedef enum {
    BLOCK_MODE_SCATTER_GATHER = 0,
    BLOCK_MODE_SKIP_AHEAD     = 1
} block_consumer_mode_t;

typedef struct {
    telemetry_ctx_t *ctx;
    block_consumer_mode_t mode;
    volatile int run_signal;
} block_consumer_thread_args_t;

telemetry_ctx_t *block_ctx_create(void);
void block_ctx_destroy(telemetry_ctx_t *ctx);
int blockfilter_main(int argc, char *argv[]);
#endif

#endif /* BLOCK_FILTER_UAPI_H */
