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
#include "telemetry_uapi.h"

#define DEV_PATH "/dev/" TELEMETRY_DEVICE_NAME
#define ITERATIONS 100000
static int writer_count = 0;
static int reader_count = 0;
static int total_bytes = 0;
static int writer_is_finished_flag = 0;

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
    int fd = open(DEV_PATH, O_WRONLY);
    char msg[32];
    
    for (int i = 0; i < ITERATIONS; i++) {
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
    int fd = open(DEV_PATH, O_RDONLY);
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
    int fd = open(DEV_PATH, O_RDWR);
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
        
        telemetry_u32 current_write_index = ring->char_write_index;
        __sync_synchronize(); // CPU/memory barrier to prevent reordering
        
        if (ring->char_read_index != current_write_index) {

            //  Was buffer full? 
            if ((current_write_index+1) % actual_buffer_size == ring->char_read_index){
                printf("Buffer was full\n");
                write(fd, NULL, 0);
            }

            // There's data to read, consume it
            char data = ring->char_buffer[ring->char_read_index];
            // printf("%c", data); // Uncomment if you want to see the stream text flying by
            // Move read index
            int next_read_index = (ring->char_read_index + 1) % actual_buffer_size;
            __sync_synchronize();

            ring->char_read_index = next_read_index; // Update read index
            reader_count++; // Increment reader count
            
        } else { 
            
            // Buffer is empty
            if (writer_is_finished_flag) {
                break;
            }
            usleep(1); // Yield to let producer catch up
        }
    }
    
    // Unmap the shared page
    munmap(ring, TELEMETRY_PAGE_SIZE);
    close(fd);
    return NULL;
}


int main() {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, producer, NULL);
   // pthread_create(&t2, NULL, consumer, NULL);
   pthread_create(&t2, NULL, zero_copy_consumer, NULL);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    

    printf("Stress test complete. Write Attempted %d Wrote %d and Read %d have finished.\n", total_bytes, writer_count, reader_count);
    return 0;
}