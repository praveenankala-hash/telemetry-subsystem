#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/ioctl.h> // REQUIRED FOR ioctl()
#include "telemetry_uapi.h"
#include "telemetry_client.h"
#include "net_filter_uapi.h"


#define DEV_PATH_CHAR "/dev/telemetry_char"
#define DEV_PATH_MMAP "/dev/telemetry_mmap"

/* Ensure matching IOCTL definition matches kernel layout exactly */
#define TELEMETRY_IOC_MAGIC 't'
#define TELEMETRY_IOC_WAKE_WRITER _IO(TELEMETRY_IOC_MAGIC, 1)

#define ITERATIONS 100
static int writer_count = 0;
static int reader_count = 0;
static int total_bytes = 0;
static int writer_is_finished_flag = 0;
static int iterations = ITERATIONS;

void pin_to_core(int core_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#else
    (void)core_id;
#endif
}

void* producer(void* arg) {
    pin_to_core(0); // Writer on Core 0
    int fd = open(DEV_PATH_CHAR, O_WRONLY);
    char msg[32];
    
    for (int i = 0; i < iterations; i++) {
        sprintf(msg, "VAL_%d", i);
        int bytes_written = write(fd, msg, strlen(msg));
        if (bytes_written > 0) {
            writer_count += bytes_written;
        }
        total_bytes += strlen(msg);
    }
    close(fd);
    writer_is_finished_flag = 1;
    return NULL;
}

void* consumer(void* arg) {
    pin_to_core(1); // Reader on Core 1
    int fd = open(DEV_PATH_CHAR, O_RDONLY);
    char buf[32];
    
  while (1) {
        memset(buf, 0, sizeof(buf));
        int bytes_read = read(fd, buf, sizeof(buf) - 1);
        
        if (bytes_read > 0) {
            reader_count += bytes_read;
            // printf("Read: %s\n", buf);
        } else if (bytes_read == 0) {
            // Buffer is temporarily empty. 
            // Check if writer is done. If main sets a 'writer_done' flag, break here.
            // For now, if you just want to clear out the tail after the writer thread joins:
            if (writer_is_finished_flag) { 
                break; 
            }
            usleep(1); // Yield to let producer catch up
        }
    }
    close(fd);
    return NULL;
}
#if 0
// Define the classic Linux kernel "READ_ONCE" macro for user space
#define READ_ONCE(x) (*(volatile typeof(x) *)&(x))

void* zero_copy_consumer(void* arg) {
    
    pin_to_core(1); // Reader on Core 1
    long runtime_page_size  = sysconf(_SC_PAGESIZE);
    long actual_buffer_size = TELEMETRY_RING_BUFFER_CAPACITY;

    if (runtime_page_size != TELEMETRY_PAGE_SIZE) {
        fprintf(stderr, "FATAL: Compiled PAGE_SIZE (%d) does not match runtime system page size (%ld)!\n", 
                TELEMETRY_PAGE_SIZE, runtime_page_size);
        return NULL;
    }

    // mmap
    int fd = open(DEV_PATH_MMAP, O_RDWR);
    if (fd < 0) {
        perror("Consumer: Failed to open device");
        return NULL;
    }
    
    // Map the shared page directly into our process memory space
    shared_telemetry_ring *ring = mmap(NULL, TELEMETRY_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) {
        perror("Consumer: Failed to mmap");
        close(fd);
        return NULL;
    }
    
    while (1) {
        
        telemetry_u32 current_read_index = READ_ONCE(ring->char_ctl.rx.rd_idx);
        telemetry_u32 current_write_index = READ_ONCE(ring->char_ctl.tx.wr_idx);
        //  Was buffer full? 
        int was_full = ((READ_ONCE(ring->char_ctl.tx.wr_idx) + 1) % actual_buffer_size == READ_ONCE(ring->char_ctl.rx.rd_idx));
        __sync_synchronize(); // CPU/memory barrier to prevent reordering
        
        if (current_read_index != current_write_index) {
         
            // There's data to read, consume it
            char data = ring->char_buffer[current_read_index];
            ring->char_ctl.rx.rd_idx = (current_read_index + 1) % actual_buffer_size;
            __sync_synchronize();
            reader_count++; // Increment reader count
            if (was_full){
                printf("Buffer was full,%d\n", reader_count);
                // write(fd, NULL, 0);  
               if (ioctl(fd, TELEMETRY_IOC_WAKE_WRITER) < 0) {
                    perror("IOCTL wake kick failed");
                }
            }
            else if ((ring->char_ctl.tx.wr_idx + 1) % actual_buffer_size == ring->char_ctl.rx.rd_idx) {
                // Buffer is full, but we just consumed data
                // This shouldn't happen if we're the only reader, but handle it
                printf("Buffer is full but we consumed data, reader_count=%d\n", reader_count);
            }
        } else { 
            // Buffer is empty
            if (writer_is_finished_flag) {
                break;
            }
            usleep(1); // Yield to let producer catch up
#if 0
            if (ioctl(fd, TELEMETRY_IOC_WAKE_WRITER) < 0) {
                perror("IOCTL wake kick failed");
            }
#endif
        }
    }
    
    // Unmap the shared page
    munmap(ring, TELEMETRY_PAGE_SIZE);
    close(fd);
    return NULL;
}
#endif

long modulo_divisor = TELEMETRY_RING_BUFFER_CAPACITY;

#define READ_ONCE(x) (*(volatile typeof(x) *)&(x))
static int ioctl_count = 0;
void* zero_copy_consumer(void* arg) {
    pin_to_core(1); // Reader on Core 1
    long runtime_page_size  = sysconf(_SC_PAGESIZE);
    long actual_buffer_size = TELEMETRY_RING_BUFFER_CAPACITY;
    int read_any = 0;
    int kick_count = 0;

    if (runtime_page_size != TELEMETRY_PAGE_SIZE) {
        fprintf(stderr, "FATAL: Compiled PAGE_SIZE (%d) does not match runtime system page size (%ld)!\n", 
                TELEMETRY_PAGE_SIZE, runtime_page_size);
        return NULL;
    }

    int fd = open(DEV_PATH_MMAP, O_RDWR);
    if (fd < 0) {
        perror("Consumer: Failed to open device");
        return NULL;
    }
    
    shared_telemetry_ring *ring = mmap(NULL, TELEMETRY_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) {
        perror("Consumer: Failed to mmap");
        close(fd);
        return NULL;
    }
    
    while (1) {
        // 1. Snapshot indices with volatile tracking to ensure real hardware loads
        telemetry_u32 current_read_index  = READ_ONCE(ring->char_ctl.rx.rd_idx);
        telemetry_u32 current_write_index = READ_ONCE(ring->char_ctl.tx.wr_idx);
        __sync_synchronize(); // Prevent compiler/CPU reordering of index snapshots
        
        if (current_read_index != current_write_index) {
            // There's data to read, consume it
            char data = ring->char_buffer[current_read_index];
            
            /* * THE CRITICAL RATIONALE:
             * Evaluate if the buffer was full BEFORE updating the read pointer,
             * but use a live read of the write pointer to confirm if the writer
             * is actively blocked right behind this slot.
             */
            //int was_full = ((READ_ONCE(ring->char_ctl.tx.wr_idx) + 1) % actual_buffer_size == current_read_index);
            
            // Commit the new read index to memory
            ring->char_ctl.rx.rd_idx = (current_read_index + 1) % actual_buffer_size;
            
            /* * FORCE CACHE LINE INVALDATION:
             * Flushes the new read pointer out to the shared cache line immediately
             * BEFORE we evaluate the 'was_full' condition logic and trigger the IOCTL.
             */
            __sync_synchronize();
            
            reader_count++; 
            
            if (READ_ONCE(ring->char_ctl.ctrl.state_flags) & TELEMETRY_FLAG_WRITER_ASLEEP) {
                
                if (kick_count== 0) {
                  // fprintf(stderr, "IOCTL wake full, count: %d\n", ioctl_count++);
                    //fprintf(stderr, "IOCTL wake full, count: %d\n", kick_count);
                    if (ioctl(fd, TELEMETRY_IOC_WAKE_WRITER) < 0) {
                        perror("IOCTL wake kick failed");
                    }
                    kick_count = 1;
                }
                else {
                   // fprintf(stderr, "IOCTL wake full , count: %d\n", kick_count);
                    kick_count= (kick_count + 1)%modulo_divisor;
                }

            }
            read_any = 1;
        } else { 
            // Buffer is empty
            if (READ_ONCE(ring->char_ctl.ctrl.state_flags) & TELEMETRY_FLAG_WRITER_ASLEEP) {
               // fprintf(stderr, "IOCTL wake empty, count: %d\n", ioctl_count++);
                ioctl(fd, TELEMETRY_IOC_WAKE_WRITER);
                read_any = 0; // Reset flag
            }
            if (writer_is_finished_flag) {
                break;
            }
            usleep(1); // Yield execution cleanly
        }
    }
    
    munmap(ring, TELEMETRY_PAGE_SIZE);
    close(fd);
    return NULL;
}

extern int netfilter_main(int argc, char *argv[]);

int main(int argc, char *argv[]) {
 #if 1
    int result = netfilter_main(argc, argv);
    if (result != 0) {
        return result;
    }
#else
    pthread_t t1, t2;

    if (argc > 1) {
        iterations = atoi(argv[1]);
    }
    pthread_create(&t1, NULL, producer, NULL);

    if (argc > 2) {
        modulo_divisor = modulo_divisor / atoi(argv[2]);
    }

    if (argc > 3) {
        pthread_create(&t2, NULL, consumer, NULL);
    }
    else {
        pthread_create(&t2, NULL, zero_copy_consumer, NULL);
    }
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    

    printf("Stress test complete. Write Attempted %d Wrote %d and Read %d have finished.\n", total_bytes, writer_count, reader_count);
    return 0;
#endif
}
