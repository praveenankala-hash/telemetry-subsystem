#ifndef TELEMETRY_UAPI_H
#define TELEMETRY_UAPI_H

#define TELEMETRY_DEVICE_NAME "telemetry"
#define TELEMETRY_PAGE_SIZE   4096
#define TELEMETRY_MINORS      6

/* Minor Number Allocations */
#define MINOR_TELEMETRY_MMAP  0
#define MINOR_TELEMETRY_CHAR  1
#define MINOR_TELEMETRY_NET   2
#define MINOR_TELEMETRY_BLOCK 3
#define MINOR_TELEMETRY_MEM   4
#define MINOR_TELEMETRY_CPU   5

/* Memory Mapping Offsets */
#define PAGE_SIZE_4K          TELEMETRY_PAGE_SIZE
#define ZONE_A_SIZE           1024
#define ZONE_B_SIZE           3072

/* Memory Partition Capacities */
#define TELEMETRY_RING_BUFFER_CAPACITY              1024 
#define TELEMETRY_NETFILTER_RING_BUFFER_CAPACITY    768 
#define TELEMETRY_BLOCKFILTER_RING_BUFFER_CAPACITY  512 
#define TELEMETRY_MEMORY_RING_BUFFER_CAPACITY       512 
#define TELEMETRY_CPU_RING_BUFFER_CAPACITY          256

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
/* Memory barriers for user space */
#define smp_rmb()           __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_load_acquire(p) ({ __typeof__(*p) ___v = __atomic_load_n(p, __ATOMIC_ACQUIRE); ___v; })
#define smp_store_release(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#endif


/* ---- State Flag Bitmask Helpers ---- */
/* Char Buffer writer asleep flag -- used to signal Producer to kick the writer(Producer) */
#define TELEMETRY_FLAG_WRITER_ASLEEP      (1U << 0)


/* Standard Base for Single-Producer Tenant Layout (Char, NetBlock, Mem, CPU)
Per tenant modifications based on buffer and design requirements  */
typedef struct {
    /* ---- CACHELINE N (Read Lane) ---- */
    struct {
        volatile telemetry_u32 rd_idx;
    } rx ALIGN_TO_CACHELINE;
    /* ---- CACHELINE N+1 (Write Lane) ---- */
    struct {
        volatile telemetry_u32 wr_idx;
    } tx ALIGN_TO_CACHELINE;
    /* ---- CACHELINE N+2 (Asynchronous Control Flags) ---- */
    // Shared or signaled lane (e.g., wake flags, interrupts, metrics)
    struct {
        volatile telemetry_u32 state_flags; // Bitmask for various control flags (e.g., writer asleep)
    } ctrl ALIGN_TO_CACHELINE;
} tenant_lane_t; // Size = exactly 192 bytes (3 clean cache lines)

/* Specialized Multi-Producer Netfilter Tenant Layout */
typedef struct {
    struct {
        volatile telemetry_u32 rd_idx;          /* Read path tracking (Consumer Only) */
    } rx ALIGN_TO_CACHELINE;

    struct {
        volatile telemetry_u32 wr_idx;          /* Highest completed ticket hint */
        volatile telemetry_u32 ticket_dispenser;/* Atomic reservation counter for incoming hooks */
    } tx ALIGN_TO_CACHELINE;

    struct {
        volatile telemetry_u32 state_flags;     /* Asynchronous control/sleep flags */
    } ctrl ALIGN_TO_CACHELINE;
} net_tenant_lane_t;

/* * Unified Multi-Tenant Control Layout - Flat Design
 * Maps cleanly onto a single 4096-Byte Hardware Page
 */
typedef struct {
    tenant_lane_t char_ctl;   /* 192 bytes / Offsets 0..191   */
    tenant_lane_t net_ctl;    /* 192 bytes / Offsets 192..383 */
    tenant_lane_t block_ctl;  /* 192 bytes / Offsets 384..575 */
    tenant_lane_t mem_ctl;    /* 192 bytes / Offsets 576..767 */
    tenant_lane_t cpu_ctl;    /* 192 bytes / Offsets 768..960 */

    /* Strict Padding to hold Zone A exactly to 512 bytes */
    char meta_pad[ZONE_A_SIZE - (5*192)];

    // --- TODO: Add more tenants here as needed. 
    // --- Shrink the meta_pad size accordingly to maintain the 1024 boundary.
    // --- Example: If you add 2 more tenants, meta_pad should be [TELEMETRY_RING_METADATA_SIZE - (8 * 64)]
    // Adjustments must FIX the Isolated Dedicated Buffers size to maintain the 4KB boundary.

    // --- Isolated Dedicated Buffers ---
/* --- ZONE B: ISOLATED DEDICATED RING BUFFERS (3584 BYTES) --- */
    char char_buffer[TELEMETRY_RING_BUFFER_CAPACITY];            /* 1024B */
    char net_buffer[TELEMETRY_NETFILTER_RING_BUFFER_CAPACITY];   /* 768B  */
    char block_buffer[TELEMETRY_BLOCKFILTER_RING_BUFFER_CAPACITY]; /* 512B  */
    char memory_buffer[TELEMETRY_MEMORY_RING_BUFFER_CAPACITY];   /* 512B  */
    char cpu_buffer[TELEMETRY_CPU_RING_BUFFER_CAPACITY];         /* 256B  */
} shared_telemetry_ring;

#endif /* TELEMETRY_UAPI_H */


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
 * | [Tenant 0: Char Stream] -> | volatile telemetry_u32 char_rd_idx |volatile telemetry_u32 char_wr_idx | [64B Cacheline Pad] |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * | [Tenant 1: Netfilter]   -> | volatile telemetry_u32 net_rd_idx  |volatile telemetry_u32 net_wr_idx  | [64B Cacheline Pad] |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * | [Tenant 2: Block I/O]   -> | volatile telemetry_u32 block_rd_idx|volatile telemetry_u32 block_wr_idx| [64B Cacheline Pad] |
 * +------------------------------------------------------------------------------------------------------------------------------------+
 * | [Meta Padding Zone]     ->   char meta_pad[TELEMETRY_RING_METADATA_SIZE - (5 * 192)] (Fills remaining room to strict 1024B offset)  |
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
 * |  BLOCK FILTER RING (Tenant 2) -- [Capacity: 512 Bytes]                                                                             |
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
