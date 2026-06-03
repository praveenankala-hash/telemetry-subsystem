#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <pthread.h>
#include "telemetry_client.h"
#include "net_filter_uapi.h"

static telemetry_ctx_t *g_nt_ctx = NULL;

/* g_nt_ctx accessor functions */
telemetry_ctx_t *nt_ctx_create(void) {
    if (g_nt_ctx) {
        fprintf(stderr, "Network telemetry context already exists\n");
        return NULL;
    }
    telemetry_ctx_t *ctx = malloc(sizeof(telemetry_ctx_t));
    if (!ctx) {
        perror("Failed to allocate network telemetry context memory");
        return NULL;
    }
    ctx->fd = -1;
    ctx->rng_ptr = NULL;
    g_nt_ctx = ctx;
    return ctx;
}

void nt_ctx_destroy(telemetry_ctx_t *ctx) {
    if (!ctx) return;
    free(ctx);
    g_nt_ctx = NULL;
}

/* Dummy metric processing function */
static inline void process_metric(net_payload_slot_t *local_slot) {
    struct net_packet_metric *metric = &local_slot->packet_metrics;
    // Keep this incredibly lean to ensure the consumer matches line-rate speeds
    printf(" TICKET:%u | %pI4:%u -> %pI4:%u | LEN:%u\n",
            local_slot->slot_ticket,
//            &metric->src_ip, ntohs(metric->src_port),
//            &metric->dest_ip, ntohs(metric->dest_port), 
//             ntohs(metric->pkt_len));
            &metric->src_ip,metric->src_port,
            &metric->dest_ip, metric->dest_port,
             metric->pkt_len);
    (void)metric; 
}

/* =========================================================================
 * WORKER THREAD: Zero-Copy Dedicated Consumer Loop
 * ========================================================================= */
void *zero_copy_consumer_thread(void *arg) {
    consumer_thread_args_t *args = (consumer_thread_args_t *)arg;
    shared_telemetry_ring *rng_ptr = args->ctx->rng_ptr;
    
    // Isolate the Netfilter control lane out of Zone A using your structure cast
    net_tenant_lane_t *net_lane = (net_tenant_lane_t *)&rng_ptr->net_ctl;

    // 1. Claim ownership of the network channel consumer path
    uint32_t flags = smp_load_acquire(&net_lane->ctrl.state_flags);
    smp_store_release(&net_lane->ctrl.state_flags, flags | TELEMETRY_FLAG_NET_THREAD_ACTIVE);    

    // Read the current tracking baseline stored in the shared control plane
    uint32_t expected_ticket = smp_load_acquire(&net_lane->rx.rd_idx);

    printf("[Consumer Thread] New Engine active. Mode: %s run_signal: %d\n", 
           (args->mode == MODE_SCATTER_GATHER) ? "Scatter-Gather" : "Skip-Ahead",
           args->run_signal);


    while (args->run_signal) {
        printf("[Consumer Thread] Starting iteration\n");
        
        // ---------------------------------------------------------------------
        // ENGINE OPTION A: SCATTER-GATHER SNAPSHOT
        // ---------------------------------------------------------------------
        if (args->mode == MODE_SCATTER_GATHER) {
            telemetry_payload_pool_t local_snapshot;
            printf("[Consumer Thread] Starting Scatter-Gather snapshot\n");
            
            // Sequential read burst across Zone B into local memory space
            memcpy(&local_snapshot, &rng_ptr->net_buffer, sizeof(telemetry_payload_pool_t));
            smp_rmb(); // Ensure memory block copy is complete before reading tickets

            while (1) {
                uint32_t slot_index = expected_ticket % NETFILTER_SLOT_CAPACITY;
                net_payload_slot_t *local_slot = &local_snapshot.slots[slot_index];
                process_metric(local_slot);
                printf("[Consumer Thread] Processing slot %u\n", slot_index);
                // If the snapshot has our target ticket, process it out of local cache
                if (local_slot->slot_ticket == expected_ticket) {
                    process_metric(local_slot);
                    expected_ticket++;
                } else {
                    // Hole or old wrap marker hit. Drop out of snapshot to sync with SHM.
                    break; 
                }
            }
        }
        
        // ---------------------------------------------------------------------
        // ENGINE OPTION B: SPECULATIVE SKIP-AHEAD
        // ---------------------------------------------------------------------
        else if (args->mode == MODE_SKIP_AHEAD) {
            uint32_t highest_hint = smp_load_acquire(&net_lane->tx.wr_idx);
            printf("[Consumer Thread] Starting Skip-Ahead with highest hint: %u\n", highest_hint);
            
            // STRICT DESIGN RULE: Use cyclic distance math to handle monotonic overflows
            while (((int32_t)(highest_hint - expected_ticket) >= 0)) {
                uint32_t slot_index = expected_ticket % NETFILTER_SLOT_CAPACITY;
                net_payload_slot_t *slot = &((net_payload_slot_t *)rng_ptr->net_buffer)[slot_index];
            
                uint32_t found_ticket = smp_load_acquire(&slot->slot_ticket);
                process_metric(slot);
                printf("[Consumer Thread] Processing slot %u\n", slot_index);
                if (found_ticket == expected_ticket) {
                    process_metric(slot);
                    expected_ticket++;
                } 
                else {
                    // HOLE INTERCEPTED: A slow producer core is holding this slot.
                    // Scan ahead sequentially up to the known write horizon to find the next valid stream anchor
                    uint32_t scan_ticket = expected_ticket + 1;
                    int hole_healed = 0;

                    while (((int32_t)(highest_hint - scan_ticket) >= 0)) {
                        uint32_t scan_idx = scan_ticket % NETFILTER_SLOT_CAPACITY;
                        net_payload_slot_t *scan_slot = &((net_payload_slot_t *)rng_ptr->net_buffer)[scan_idx];
                        process_metric(scan_slot);  
                        printf("[Consumer Thread] Processing slot %u\n", scan_idx);
                        if (smp_load_acquire(&scan_slot->slot_ticket) == scan_ticket) {
                            // Healing leap: Hop over the stalled slot(s) to align tracking tail
                            expected_ticket = scan_ticket;
                            hole_healed = 1;
                            break; 
                        }
                        scan_ticket++;
                    }

                    // If we couldn't find a forward valid anchor, we've reached the edge of live data
                    if (!hole_healed) {
                        break; 
                    }
                }
            }
        }

        // Push our processed read index back to the kernel control plane
        smp_store_release(&net_lane->rx.rd_idx, expected_ticket);
        
        // Micro-sleep or cpu_relax to prevent pegging the core if traffic is light
        usleep(10); 
    }

    // 2. Relinquish ownership before exiting so 'cat' diagnostics work cleanly again
    flags = smp_load_acquire(&net_lane->ctrl.state_flags);
    smp_store_release(&net_lane->ctrl.state_flags, flags & ~TELEMETRY_FLAG_NET_THREAD_ACTIVE);
    printf("[Consumer Thread] Terminated cleanly.\n");
    return NULL;
} 

int netfilter_main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <0 for Scatter-Gather | 1 for Skip-Ahead>\n", argv[0]);
        return EXIT_FAILURE;
    }

    consumer_mode_t selected_mode = (atoi(argv[1]) == 1) ? MODE_SKIP_AHEAD : MODE_SCATTER_GATHER;

    // TODO: Add netfilter specific initialization here
    telemetry_ctx_t *telemetry_ctx = nt_ctx_create();
    if (!telemetry_ctx) {
        return EXIT_FAILURE;
    }

    // Initialize the unified memory architecture
    if (!telemetry_initialize("/dev/telemetry_mmap", telemetry_ctx)) {
        nt_ctx_destroy(telemetry_ctx);
        return EXIT_FAILURE;
    }

    // Phase 2: Setup Thread Argument Package
    consumer_thread_args_t thread_args = {
        .ctx = telemetry_ctx,
        .mode = selected_mode,
        .run_signal = 1
    };

    pthread_t consumer_worker_id;
    
    // Phase 3: Launch the consumer thread into the background
    if (pthread_create(&consumer_worker_id, NULL, zero_copy_consumer_thread, &thread_args) != 0) {
        perror("Failed to spawn background zero-copy consumer thread");
        telemetry_shutdown(telemetry_ctx);
        nt_ctx_destroy(telemetry_ctx);
        return EXIT_FAILURE;
    }

    // Let the background engine run for profiling
    printf("[Main] System running. Press Enter to terminate profiling test...\n");
    getchar();

    // Phase 4: Graceful Teardown Sequence
    printf("[Main] Signaling thread shutdown...\n");
    thread_args.run_signal = 0;
    pthread_join(consumer_worker_id, NULL);

    telemetry_shutdown(telemetry_ctx);
    nt_ctx_destroy(telemetry_ctx);
    printf("[Main] Shutdown complete.\n");

    return EXIT_SUCCESS;
}