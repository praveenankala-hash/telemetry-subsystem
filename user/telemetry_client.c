#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "telemetry_uapi.h"
#include "telemetry_client.h"

#define DEV_PATH_MMAP "/dev/telemetry_mmap"

telemetry_ctx_t *telemetry_initialize(const char *device_path, telemetry_ctx_t *ctx) {
    //telemetry_ctx_t *ctx = malloc(sizeof(telemetry_ctx_t));
    //telemetry_ctx_t *ctx = ctx_ptr;
    if (!ctx) {
        perror("No telemetry context memory provided");
        return NULL;
    }

    // Open the single, unified telemetry char device
    ctx->fd = open(DEV_PATH_MMAP, O_RDWR);
    if (ctx->fd < 0) {
        perror("Failed to open unified telemetry device");
        return NULL;
    }

    // Map the exact 4KB hardware page (Zone A + Zone B combined)
    ctx->rng_ptr = (shared_telemetry_ring *)mmap(
        NULL,
        sizeof(shared_telemetry_ring),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        ctx->fd,
        0
    );

    if (ctx->rng_ptr == MAP_FAILED) {
        perror("Failed to mmap unified telemetry ring buffer");
        close(ctx->fd);
        ctx->fd = -1;
        ctx->rng_ptr = NULL;
        return NULL;
    }
    return ctx;
}

void telemetry_shutdown(telemetry_ctx_t *ctx) {
    if (!ctx) return;
    
    if (ctx->rng_ptr && ctx->rng_ptr != MAP_FAILED) {
        munmap(ctx->rng_ptr, sizeof(shared_telemetry_ring));
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    //free(ctx);
}