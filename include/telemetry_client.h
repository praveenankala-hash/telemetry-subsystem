#ifndef _TELEMETRY_CLIENT_H
#define _TELEMETRY_CLIENT_H

#include "telemetry_uapi.h"
#ifndef __KERNEL__
typedef struct {
    int fd;
    shared_telemetry_ring *rng_ptr;
} telemetry_ctx_t;

/* Base lifecycle mappings */
telemetry_ctx_t *telemetry_initialize(const char *device_path, telemetry_ctx_t *ctx);
void telemetry_shutdown(telemetry_ctx_t *ctx);
#endif

#endif /* _TELEMETRY_CLIENT_H */