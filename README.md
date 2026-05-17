# Telemetry Subsystem

This project contains a multi-tenant Linux telemetry subsystem built around one shared 4096-byte page exposed to user space through the core character device.

## Layout

- **`bin/`**: Final copied build outputs for modules and the user executable.
- **`include/`**: Shared UAPI header and memory-map definitions.
- **`kernel/`**: Core telemetry module that allocates the shared page, registers the character device, and exports telemetry push APIs.
- **`net_filter/`**: Netfilter module that records packet telemetry through the exported core APIs.
- **`block_filter/`**: Placeholder block telemetry module for future block-layer tracing.
- **`user/`**: User-space telemetry consumer/producer test program.

## Build

From this directory:

```bash
make
```

Or from the repository root:

```bash
make
```

## Clean

```bash
make clean
```

## Device Node

The core module dynamically registers a major number. After loading `kernel/telemetry.ko`, create `/dev/telemetry` with the major number shown in `/proc/devices`.
