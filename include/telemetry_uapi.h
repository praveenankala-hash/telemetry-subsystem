#ifndef TELEMETRY_UAPI_H
#define TELEMETRY_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __u32 telemetry_u32;
typedef __u16 telemetry_u16;
typedef __s32 telemetry_s32;
#define ALIGN_TO_CACHELINE ____cacheline_aligned
#else
#include <stdint.h>
typedef uint32_t telemetry_u32;
typedef uint16_t telemetry_u16;
typedef int32_t telemetry_s32;
// Cross-platform user-space cache alignment specifier
#define ALIGN_TO_CACHELINE __attribute__((aligned(64))) 
#endif

#define TELEMETRY_DEVICE_NAME "telemetry"
#define TELEMETRY_PAGE_SIZE 4096

// --- Memory Partition Calculations ---
#define TELEMETRY_RING_METADATA_SIZE               1024 
#define TELEMETRY_RING_BUFFER_CAPACITY             1536 // Character Device Buffer
#define TELEMETRY_NETFILTER_RING_BUFFER_CAPACITY    768 // Network Metrics Buffer
#define TELEMETRY_BLOCKFILTER_RING_BUFFER_CAPACITY  768 // Block Storage Buffer

// Core 16-Byte Network Struct
struct net_packet_metric {
    telemetry_u32 src_ip;
    telemetry_u32 dest_ip;
    telemetry_u16 src_port;
    telemetry_u16 dest_port;
    telemetry_u32 pkt_len;
} __attribute__((packed));

// Unified Multi-Tenant Control Layout
typedef struct {
    // --- Tenant 0: Character Stream Control ---
    volatile telemetry_u32 char_read_index  ALIGN_TO_CACHELINE;
    volatile telemetry_u32 char_write_index ALIGN_TO_CACHELINE;

    // --- Tenant 1: Netfilter Stream Control ---
    volatile telemetry_u32 net_read_index   ALIGN_TO_CACHELINE;
    volatile telemetry_u32 net_write_index  ALIGN_TO_CACHELINE;

    // --- Tenant 2: Block Filter Stream Control ---
    volatile telemetry_u32 block_read_index ALIGN_TO_CACHELINE;
    volatile telemetry_u32 block_write_index ALIGN_TO_CACHELINE;

    // Fixed Metadata Area Padding (Enforces the 1024 boundary strictly)
    char meta_pad[TELEMETRY_RING_METADATA_SIZE - (6 * 64)]; 

    // --- Isolated Dedicated Buffers ---
    char char_buffer[TELEMETRY_RING_BUFFER_CAPACITY];
    char net_buffer[TELEMETRY_NETFILTER_RING_BUFFER_CAPACITY];
    char block_buffer[TELEMETRY_BLOCKFILTER_RING_BUFFER_CAPACITY];

} shared_telemetry_ring;

#endif


/*
 * ++==================================================================================================================================++
 * ||                                           TECHNICAL DESIGN & HARDWARE MEMORY MAP                                                 ||
 * ++==================================================================================================================================++
 *
 * This design maps out a unified 4096-Byte physical page allocated via get_zeroed_page() in the kernel and shared directly with
 * user-space via mmap(). The layout targets a modern standard 120-character editor viewport to allow full structural visualization
 * in a single, un-wrapped layout.
 *
 * Each tracing tenant operates in its own isolated memory segment. Indices are explicitly aligned to 64-byte boundaries (L1 Cache)
 * to prevent cross-core cacheline invalidations (false sharing) when different execution contexts hammer the ring buffers simultaneously.
 *
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * |                                           SINGLE 4096-BYTE PHYSICAL MEMORY PAGE REGION                                             |
 * +====================================================================================================================================+
 * | ZONE A: SHARED PAGE METADATA & RING CONTROL INDICES (Total Allocation: 1024 Bytes)                                                 |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * | [Tenant 0: Char Stream] -> | volatile telemetry_u32 char_read_index |volatile telemetry_u32 char_write_index | [64B Cacheline Pad] |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * | [Tenant 1: Netfilter]   -> | volatile telemetry_u32 net_read_index  |volatile telemetry_u32 net_write_index  | [64B Cacheline Pad] |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * | [Tenant 2: Block I/O]   -> | volatile telemetry_u32 block_read_index|volatile telemetry_u32 block_write_index| [64B Cacheline Pad] |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * | [Meta Padding Zone]     ->   char meta_pad[TELEMETRY_RING_METADATA_SIZE - (6 * 64)] (Fills remaining room to strict 1024B offset)  |
 * +====================================================================================================================================+
 * | ZONE B: ISOLATED MULTI-TENANT RING BUFFER PAYLOADS (Total Allocation: 3072 Bytes)                                                  |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * |                                                                                                                                    |
 * |  SHARED CHAR RING (Tenant 0) -- [Capacity: 1536 Bytes]                                                                             |
 * |  - Execution Context: Process Context / User System Calls via dev_write() & dev_read()                                             |
 * |  - Concurrency Sync:  IRQ-Safe Spinlock Protection (telemetry_char_write_lock)                                                     |
 * |  - Stream Semantics:  Raw, unaligned, chaotic byte-by-byte pipeline. Interleaved by multiple concurrent writers.                   |
 * |                                                                                                                                    |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * |                                                                                                                                    |
 * |  NETFILTER RING (Tenant 1) -- [Capacity: 768 Bytes]                                                                                |
 * |  - Execution Context: SoftIRQ Context / Network Stack via Registered Hook (NF_INET_LOCAL_OUT)                                      |
 * |  - Concurrency Sync:  LOCKLESS Target. Single-Producer, Single-Consumer (SPSC) layout or Multi-Producer Atomic Reservation Loop.   |
 * |  - Stream Semantics:  Strictly framed 16-Byte aligned 'struct net_packet_metric' blocks.Supports lossy Flight Recorder overwrite mode. |
 * |                                                                                                                                    |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * |                                                                                                                                    |
 * |  BLOCK FILTER RING (Tenant 2) -- [Capacity: 768 Bytes]                                                                             |
 * |  - Execution Context: HardIRQ / Kernel Thread Context / Block Storage Layer via bio_submit() requests                              |
 * |  - Concurrency Sync:  [TBD - Under Iteration] Highly concurrent Multi-Producer Lockless / Compare-And-Swap (cmpxchg) layout        |
 * |  - Stream Semantics:  [TBD - Under Iteration] Aligned block storage tracing structures                                             |
 * |                                                                                                                                    |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * |                                             BOTTOM OF 4096-BYTE HARDWARE PAGE BOUNDARY                                             |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 *
 *
 * ++==================================================================================================================================++
 * ||                                        CONCURRENCY PIPELINE & DATA FLOW ARCHITECTURE                                             ||
 * ++==================================================================================================================================++
 *
 *                         PRODUCER CONTEXTS (Kernel Engines)                                 CONSUMER INTERFACE (User Space)
 *
 *                         +-----------------------------------------+                         +-----------------------------------+
 *                         |             PROCESS CONTEXT             |                         |     USER-SPACE DAEMON LOOP        |
 *                         |     User Application / Thread Flow      |                         |  - Pulls metrics continuously     |
 *                         |          Calls: dev_write()             |                         |  - 0% CPU utilization when idle   |
 *                         +-------------------+---------------------+                         +-----------------+-----------------+
 *                                             |                                                             ^
 *                                             V                                                             |
 *                          [ IRQ-SAFE SPINLOCK GATE ]                                                       |
 *                         Masks local CPU interrupts to                                                     | Maps shared page via
 *                         prevent single-core preemption deadlocks.                                         | zero-copy mmap() or
 *                                             |                                                             | drains sequentially
 *                                             V                                                             | via dev_read()
 *                         +-------------------+---------------------+                                       |
 *                         |       SHARED CHAR BUFFER SECTOR         |<======================================+
 *                         |  - Pushes dynamic string tokens/bytes   |                                       |
 *                         +-----------------------------------------+                                       |
 *                                             |                                                             |
 *                                             |                                                             |
 *                         +-----------------------------------------+                                       |
 *                         |             SOFTIRQ CONTEXT             |                                       |
 *                         |      Network Stack Processing Path      |                                       |
 *                         |       Calls: netfilter_hook()           |                                       |
 *                         +-------------------+---------------------+                                       |
 *                                             |                                                             |
 *                                             V                                                             |
 *                          [ ATOMIC SPACE RESERVATION ]                                                     |
 *                          Uses lockless math or cmpxchg loops                                              |
 *                          to claim exact 16-Byte boundaries.                                               |
 *                                             |                                                             |
 *                                             V                                                             |
 *                         +-------------------+---------------------+                                       |
 *                         |          NETFILTER METRIC SECTOR        |<======================================+
 *                         |  - Commits whole net_packet_metric structs  |                                       |
 *                         +-----------------------------------------+                                       |
 *                                             |                                                             |
 *                                             |                                                             |
 *                         +-----------------------------------------+                                       |
 *                         |         BLOCK / HARDIRQ CONTEXT         |                                       |
 *                         |   Asynchronous File System Writebacks   |                                       |
 *                         |          Calls: submit_bio()            |                                       |
 *                         +-------------------+---------------------+                                       |
 *                                             |                                                             |
 *                                             V                                                             |
 *                          [ UNDER DESIGN ITERATION ]                                                       |
 *                          Multi-core file system threads require                                           |
 *                          lockless concurrent layout.                                                      |
 *                                             |                                                             |
 *                                             V                                                             |
 *                         +-------------------+---------------------+                                       |
 *                         |          BLOCK FILTER SECTOR            |<======================================+
 *                         |  - [TBD] Storage metrics tracking area  |
 *                         +-----------------------------------------+
 *
 * ++==================================================================================================================================++
 */
