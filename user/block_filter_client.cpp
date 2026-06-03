#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

extern "C" {
#include "telemetry_client.h"
#include "block_filter_uapi.h"
}

static telemetry_ctx_t *g_block_ctx = nullptr;

telemetry_ctx_t *block_ctx_create(void) {
    if (g_block_ctx) {
        std::fprintf(stderr, "Block telemetry context already exists\n");
        return nullptr;
    }

    auto *ctx = static_cast<telemetry_ctx_t *>(std::calloc(1, sizeof(telemetry_ctx_t)));
    if (!ctx) {
        std::perror("Failed to allocate block telemetry context memory");
        return nullptr;
    }

    ctx->fd = -1;
    ctx->rng_ptr = nullptr;
    g_block_ctx = ctx;
    return ctx;
}

void block_ctx_destroy(telemetry_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    std::free(ctx);
    g_block_ctx = nullptr;
}

static inline void process_block_metric(const block_payload_slot_t *slot) {
    const block_io_metric *metric = &slot->io_metrics;
    unsigned long long sector =
        (static_cast<unsigned long long>(metric->sector_high) << 32) | metric->sector_low;

    std::printf("BLOCK TICKET:%u | DEV:%u:%u | SECTOR:%llu | NSECT:%u | BYTES:%u | OP:%u | FLAGS:%u\n",
                slot->slot_ticket,
                metric->dev_major,
                metric->dev_minor,
                sector,
                metric->nr_sectors,
                metric->bytes,
                metric->op,
                metric->flags);
}

static void *block_zero_copy_consumer_thread(void *arg) {
    auto *args = static_cast<block_consumer_thread_args_t *>(arg);
    shared_telemetry_ring *rng_ptr = args->ctx->rng_ptr;
    tenant_lane_t *block_lane = &rng_ptr->block_ctl;

    telemetry_u32 flags = smp_load_acquire(&block_lane->ctrl.state_flags);
    smp_store_release(&block_lane->ctrl.state_flags, flags | TELEMETRY_FLAG_BLOCK_THREAD_ACTIVE);

    telemetry_u32 expected_ticket = smp_load_acquire(&block_lane->rx.rd_idx);

    std::printf("[Block Consumer] Engine active. Mode: %s\n",
                args->mode == BLOCK_MODE_SCATTER_GATHER ? "Scatter-Gather" : "Skip-Ahead");

    while (args->run_signal) {
        if (args->mode == BLOCK_MODE_SCATTER_GATHER) {
            block_payload_pool_t local_snapshot;
            std::memcpy(&local_snapshot, rng_ptr->block_buffer, sizeof(local_snapshot));
            smp_rmb();

            while (true) {
                telemetry_u32 slot_index = expected_ticket % BLOCKFILTER_SLOT_CAPACITY;
                block_payload_slot_t *slot = &local_snapshot.slots[slot_index];

                if (slot->slot_ticket != expected_ticket) {
                    break;
                }

                process_block_metric(slot);
                expected_ticket++;
            }
        } else {
            telemetry_u32 highest_hint = smp_load_acquire(&block_lane->tx.wr_idx);

            while (static_cast<telemetry_s32>(highest_hint - expected_ticket) >= 0) {
                telemetry_u32 slot_index = expected_ticket % BLOCKFILTER_SLOT_CAPACITY;
                auto *slot = &reinterpret_cast<block_payload_slot_t *>(rng_ptr->block_buffer)[slot_index];
                telemetry_u32 found_ticket = smp_load_acquire(&slot->slot_ticket);

                if (found_ticket == expected_ticket) {
                    process_block_metric(slot);
                    expected_ticket++;
                    continue;
                }

                telemetry_u32 scan_ticket = expected_ticket + 1;
                bool hole_healed = false;

                while (static_cast<telemetry_s32>(highest_hint - scan_ticket) >= 0) {
                    telemetry_u32 scan_index = scan_ticket % BLOCKFILTER_SLOT_CAPACITY;
                    auto *scan_slot = &reinterpret_cast<block_payload_slot_t *>(rng_ptr->block_buffer)[scan_index];

                    if (smp_load_acquire(&scan_slot->slot_ticket) == scan_ticket) {
                        expected_ticket = scan_ticket;
                        hole_healed = true;
                        break;
                    }
                    scan_ticket++;
                }

                if (!hole_healed) {
                    break;
                }
            }
        }

        smp_store_release(&block_lane->rx.rd_idx, expected_ticket);
        usleep(10);
    }

    flags = smp_load_acquire(&block_lane->ctrl.state_flags);
    smp_store_release(&block_lane->ctrl.state_flags, flags & ~TELEMETRY_FLAG_BLOCK_THREAD_ACTIVE);
    std::printf("[Block Consumer] Terminated cleanly.\n");
    return nullptr;
}

int blockfilter_main(int argc, char *argv[]) {
    block_consumer_mode_t selected_mode = BLOCK_MODE_SCATTER_GATHER;

    if (argc > 1) {
        selected_mode = (std::atoi(argv[1]) == 1) ? BLOCK_MODE_SKIP_AHEAD : BLOCK_MODE_SCATTER_GATHER;
    }

    telemetry_ctx_t *ctx = block_ctx_create();
    if (!ctx) {
        return EXIT_FAILURE;
    }

    if (!telemetry_initialize("/dev/telemetry_mmap", ctx)) {
        block_ctx_destroy(ctx);
        return EXIT_FAILURE;
    }

    block_consumer_thread_args_t thread_args = {
        ctx,
        selected_mode,
        1
    };

    pthread_t worker_id;
    if (pthread_create(&worker_id, nullptr, block_zero_copy_consumer_thread, &thread_args) != 0) {
        std::perror("Failed to spawn block zero-copy consumer thread");
        telemetry_shutdown(ctx);
        block_ctx_destroy(ctx);
        return EXIT_FAILURE;
    }

    std::printf("[Block Main] System running. Press Enter to terminate profiling test...\n");
    getchar();

    thread_args.run_signal = 0;
    pthread_join(worker_id, nullptr);

    telemetry_shutdown(ctx);
    block_ctx_destroy(ctx);
    std::printf("[Block Main] Shutdown complete.\n");

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
    return blockfilter_main(argc, argv);
}
