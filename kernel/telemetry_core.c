#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>      // Required for registration functions
#include <linux/uaccess.h> // Required for copy_to_user
#include <linux/slab.h>    // Required for kmalloc/kfree
#include <linux/cache.h> // Ensure this header is included
#include <linux/mm.h>  // Required for remap_pfn_range and page manipulation
#include "net_filter_uapi.h"
#include "telemetry.h"
#include "telemetry_core.h"

// --- Synchronization Primitives ---
/* these below are synchronization primitives for critical sections */

// --- Wait Queue for Writer full condition ---
// Declare and initialize the wait queue for the writer
DECLARE_WAIT_QUEUE_HEAD(char_wait_queue);
// --- ENDOF: Wait Queue for Writer ---

// --- IRQ-SAFE Lock for Ring Buffer ---
/* DECLARE THE IRQ-SAFE LOCK : this protects basic ring buffer on multiple producers
As a advanced we will update the design to segment & use seperate ring buffers for each producer */
DEFINE_SPINLOCK(telemetry_char_write_lock); 
// --- ENDOF: IRQ-SAFE Lock for Ring Buffer ---

// --- ENDOF: Synchronization Primitives ---

/* ---------------------------------------------------------------------------------- */
/* TENANT 0: STREAM CHANNELS                                                          */
/* ---------------------------------------------------------------------------------- */

// --- Device Open Function ---
// Function called when user-space opens /dev/telemetry
ssize_t dev_open(struct inode *inodep, struct file *filep) {
    printk(KERN_INFO "Telemetry: Char Device opened\n");
    return 0;
}

// --- Device Read Function ---
ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    int bytes_read = 0;
    int data_ready = 0;

    // circular buffer logic.. if read_index == write_index, then buffer is empty
    while ( len > 0 )
    {
        int snapshot_write_index = READ_ONCE(ring_ptr->char_ctl.tx.wr_idx);
        /* * RMB: Read Memory Barrier.
         * This ensures that the read_index is read BEFORE the kernel_char_buffer data.
         */
        rmb();
        if (ring_ptr->char_ctl.rx.rd_idx == snapshot_write_index) 
            break;
        
        if ( put_user(ring_ptr->char_buffer[ring_ptr->char_ctl.rx.rd_idx], buffer++)){
            return -EFAULT;
        }
        wmb();
        ring_ptr->char_ctl.rx.rd_idx = (ring_ptr->char_ctl.rx.rd_idx + 1) % RING_BUFFER_CAPACITY;
        len--;
        bytes_read++;
        data_ready = 1;
    }
    
    if (data_ready) {
        wake_up_interruptible(&char_wait_queue);
    }

    return bytes_read;
}

// --- Device Write Function ---
// --- Device Write Function ---
#if 0
ssize_t dev_write(struct file *filep, const char __user *buffer, size_t len, loff_t *offset) 
{
    int bytes_written = 0;
    unsigned long flags;
    int next_write_index;
    int ret;

    // INTERCEPT THE USER-SPACE KICK (For your base IOCTL/Fallback pathways)
    if (len == 0) {
        wake_up_interruptible(&char_wait_queue);
        return 0; 
    }

    while (len > 0) {
        // Calculate where the next write would land
        next_write_index = (ring_ptr->char_ctl.tx.wr_idx + 1) % RING_BUFFER_CAPACITY;

        /* * LOCKLESS NATIVE EVALUATION:
         * By evaluating the fullness condition natively inside the wait macro,
         * the kernel ensures the thread is registered on the wait queue BEFORE 
         * verifying pointers. This completely eliminates the lost wake-up race.
         */
        ret = wait_event_interruptible(char_wait_queue, ({
            smp_rmb(); // Refresh cross-core cache lines
            next_write_index != READ_ONCE(ring_ptr->char_ctl.rx.rd_idx);
        }));

        if (ret < 0) {
            return ret; // Interrupted system call (-ERESTARTSYS)
        }

        /* LOCK STEP: Acquire spinlock to safely advance memory indices */
        spin_lock_irqsave(&telemetry_char_write_lock, flags);
        
        // Double check fullness state under the spinlock protection
        if (next_write_index == READ_ONCE(ring_ptr->char_ctl.rx.rd_idx)) {
            spin_unlock_irqrestore(&telemetry_char_write_lock, flags);
            continue; // Re-evaluate condition cleanly
        }

        if (get_user(ring_ptr->char_buffer[ring_ptr->char_ctl.tx.wr_idx], buffer++)) {
            spin_unlock_irqrestore(&telemetry_char_write_lock, flags);
            return -EFAULT;
        }

        /* smp_wmb: Forces the data payload to be committed before updating the write index */
        smp_wmb();

        ring_ptr->char_ctl.tx.wr_idx = next_write_index;
        
        /* UNLOCK STEP */
        spin_unlock_irqrestore(&telemetry_char_write_lock, flags);
        
        len--;
        bytes_written++;
    }

    return bytes_written;
}
#endif
#if 1
static int full_count = 0;
ssize_t dev_write(struct file *filep, const char *buffer, size_t len, loff_t *offset) 
{
    int bytes_written = 0;
    unsigned long flags;
    int next_write_index;
    int snapshot_read_index;
    int ret;

    // INTERCEPT THE USER-SPACE KICK
    if (len == 0) {
        // Explicitly wake up the wait queue. The sleeping dev_write instances
        // will wake up, re-evaluate their loop conditions, and see that space has cleared.
        wake_up_interruptible(&char_wait_queue);
        return 0; 
    }

    while ( len > 0 )
    {
        /* LOCK STEP: Acquire the spinlock */
        spin_lock_irqsave(&telemetry_char_write_lock, flags);
        
        next_write_index = (ring_ptr->char_ctl.tx.wr_idx + 1) % RING_BUFFER_CAPACITY;
        snapshot_read_index = READ_ONCE(ring_ptr->char_ctl.rx.rd_idx);
        smp_rmb();
        
        if (next_write_index == snapshot_read_index) {
            /* LOCK STEP: Raise the sleep flag under spinlock before releasing it */
            WRITE_ONCE(ring_ptr->char_ctl.ctrl.state_flags,
                       READ_ONCE(ring_ptr->char_ctl.ctrl.state_flags) | TELEMETRY_FLAG_WRITER_ASLEEP);
            smp_wmb(); // Ensure user-space sees the flag change before we sleep
            /* UNLOCK STEP: Release the spinlock before sleeping */
            spin_unlock_irqrestore(&telemetry_char_write_lock, flags);
            
            //pr_info("telemetry_core: Buffer full, waiting for space, count: %d\n", full_count++);
            // BUFFER IS FULL: Put the producer to sleep until space opens up
            // The condition checks if space HAS BECOME AVAILABLE
            
            ret = wait_event_interruptible(char_wait_queue,  
                          (((ring_ptr->char_ctl.tx.wr_idx + 1) % RING_BUFFER_CAPACITY) != READ_ONCE(ring_ptr->char_ctl.rx.rd_idx))
                 );
            /* * NATIVE EVALUATION: Putting the condition directly inside the macro 
            * ensures the kernel re-evaluates the index dynamically over the cache boundary.
            
            ret = wait_event_interruptible(char_wait_queue, (
                {
                smp_rmb(); // Force an SMP cache-coherency refresh across cores
                ((ring_ptr->char_ctl.tx.wr_idx + 1) % RING_BUFFER_CAPACITY) != READ_ONCE(ring_ptr->char_ctl.rx.rd_idx);
                }));*/
            if (ret < 0) {
                return ret; // Handle signals/interrupted system calls (-ERESTARTSYS)
            }
           // pr_info("telemetry_core: Buffer full, woken up\n");

            /* WOKE UP: Clear the flag immediately so user space stops signaling */
            spin_lock_irqsave(&telemetry_char_write_lock, flags);
            WRITE_ONCE(ring_ptr->char_ctl.ctrl.state_flags,
                       READ_ONCE(ring_ptr->char_ctl.ctrl.state_flags) & ~TELEMETRY_FLAG_WRITER_ASLEEP);
            spin_unlock_irqrestore(&telemetry_char_write_lock, flags);
            // Re-read indices after waking up
            continue;
        }


        if (get_user(ring_ptr->char_buffer[ring_ptr->char_ctl.tx.wr_idx], buffer++)){
            /* UNLOCK STEP: Release the spinlock before returning error */
            spin_unlock_irqrestore(&telemetry_char_write_lock, flags);
            return -EFAULT;
        }
        /* * WMB: Write Memory Barrier.
         * This forces the 'get_user' data to be committed to the 
         * kernel_char_buffer BEFORE the write_index is allowed to change.
         */
        smp_wmb();

        ring_ptr->char_ctl.tx.wr_idx = next_write_index;
        
        /* UNLOCK STEP: Release the spinlock */
        spin_unlock_irqrestore(&telemetry_char_write_lock, flags);
        
        len--;
        bytes_written++;
    }

    return bytes_written;
}
#endif


#if defined(SIMPLE_CHAR_DRIVER)

// --- Memory Mapping Function ---
// Allow user space to map the ring buffer directly into their address space
// This enables zero-copy access to telemetry data
static int dev_mmap(struct file *filep, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn;

    // Check if the size is valid.. User space can only map one page at a time
    if (size > PAGE_SIZE) {
        return -EINVAL;
    }

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    pfn = virt_to_phys(ring_ptr) >> PAGE_SHIFT;
    
    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
        pr_err("Telemetry Driver: mmap remap_pfn_range failed\n");
        return -EAGAIN;
    }

    return 0;
}
// --- END: Memory Mapping Function ---

// Map the system calls to our driver functions
static struct file_operations fops = {
    .open = dev_open,
    .read = dev_read,
    .write = dev_write,
#ifndef SIMPLE_CHAR_DRIVER
    .mmap = dev_mmap,
#endif
};
#endif


// --- END: File Operations ---


/* ---------------------------------------------------------------------------------- */
/* EXTERNAL TRACING ENDPOINTS                                                         */
/* ---------------------------------------------------------------------------------- */

// --- Telemetry Shared Character Device Functions ---
// --- 2 Producer Functions ---
// --- E.g. Network Filter push metric to shared char device
// --- E.g. Application Streams date to shared char device
// -- Spinlock protected
int telemetry_shared_char_dev_push_metric(struct net_packet_metric *metric) {
    int actual_buffer_size = RING_BUFFER_CAPACITY;
    int bytes_to_write = sizeof(struct net_packet_metric);
    char *metric_ptr = (char *)metric;
    int next_write;
    int i;
    unsigned long flags;

    if (!metric || !ring_ptr) {
        return -ENODEV;
    }

    /*ACQUIRE THE SPINLOCK*/
    spin_lock_irqsave(&telemetry_char_write_lock, flags);

    for (i = 0; i < bytes_to_write; i++) {

        next_write = (ring_ptr->char_ctl.tx.wr_idx + 1) % actual_buffer_size;

        if (next_write == READ_ONCE(ring_ptr->char_ctl.rx.rd_idx)) {
            // Ring buffer is full
            spin_unlock_irqrestore(&telemetry_char_write_lock, flags);
            return -ENOSPC;
        }

        ring_ptr->char_buffer[ring_ptr->char_ctl.tx.wr_idx] = metric_ptr[i];
        ring_ptr->char_ctl.tx.wr_idx = next_write;
    }

    smp_wmb(); // Ensure cross-core visibility for user space consumer
    spin_unlock_irqrestore(&telemetry_char_write_lock, flags);
    return 0;
}

EXPORT_SYMBOL(telemetry_shared_char_dev_push_metric);
// --- END: Telemetry Shared Character Device Functions ---

// --- Telemetry Network Device Functions ---
// --- Dedicated to Network Filter --
// ---- Multi-producer safe ----> based on atomic operations
// ---- No spinlock required ----
// ---- hook point: netfilter hook ----> called from network packet processing path 
// from multiple concurrent application threads ---
// TODO: Add atomic operation details
// -- WITHOUT registering Consumer -- Oldest metric is overwritten
int telemetry_net_dev_push_metric(struct net_packet_metric *metric) {
    int actual_buffer_size = TELEMETRY_NETFILTER_RING_BUFFER_CAPACITY;
    int bytes_to_write = sizeof(struct net_packet_metric);
    char *metric_ptr = (char *)metric;
    int next_write;
    int i;

    if (!metric || !ring_ptr) {
        return -ENODEV;
    }

    for (i = 0; i < bytes_to_write; i++) {

        next_write = (ring_ptr->net_ctl.tx.wr_idx + 1) % actual_buffer_size;

        if (next_write == READ_ONCE(ring_ptr->net_ctl.rx.rd_idx)) {
            // Ring buffer is full
            return -ENOSPC;
        }

        ring_ptr->net_buffer[ring_ptr->net_ctl.tx.wr_idx] = metric_ptr[i];
        ring_ptr->net_ctl.tx.wr_idx = next_write;
    }

    smp_wmb(); // Ensure cross-core visibility for user space consumer
    return 0;
}

EXPORT_SYMBOL(telemetry_net_dev_push_metric);

// --- END: Telemetry Network Device Functions ---

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Praveen Ankala");
MODULE_DESCRIPTION("A simple character device for telemetry data");


//==============================================================================

// IGNORE everything below this line, I might integrate it later into the overall
// telemetry subsystem
// --- BEGIN: Simple Character Driver Functions ---
// --- Single Producer, Single Consumer Implementation ---
// --- Not Spinlock-based ---
#ifdef SIMPLE_CHAR_DRIVER

static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    int bytes_read = 0;
    int data_ready = 0;

    // circular buffer logic.. if read_index == write_index, then buffer is empty
    while ( len > 0 )
    {
        int snapshot_write_index = READ_ONCE(char_write_index);
        /* * RMB: Read Memory Barrier.
         * This ensures that the read_index is read BEFORE the kernel_char_buffer data.
         */
        rmb();
        if (char_read_index == snapshot_write_index) 
            break;
        
        if ( put_user(kernel_char_buffer[char_read_index], buffer++)){
            return -EFAULT;
        }
        wmb();
        char_read_index = (char_read_index + 1) % RING_BUFFER_CAPACITY;
        len--;
        bytes_read++;
        data_ready = 1;
    }
    
    if (data_ready) {
        wake_up_interruptible(&char_wait_queue);
    }

    return bytes_read;
}

static ssize_t dev_write(struct file *filep, const char *buffer, size_t len, loff_t *offset) {

    int bytes_written = 0;
    unsigned long flags;
    int next_write_index;
    int snapshot_read_index;
    int ret;

    while ( len > 0 )
    {
        next_write_index = (char_write_index + 1) % RING_BUFFER_CAPACITY;
        snapshot_read_index = READ_ONCE(char_read_index);
        rmb();
        
        if (next_write_index == snapshot_read_index) {
            
            // BUFFER IS FULL: Put the producer to sleep until space opens up
            // The condition checks if space HAS BECOME AVAILABLE
            ret = wait_event_interruptible(char_wait_queue,  
                (((char_write_index + 1) % RING_BUFFER_CAPACITY) != READ_ONCE(char_read_index))
            );
            
            if (ret < 0) {
                return ret; // Handle signals/interrupted system calls (-ERESTARTSYS)
            }
            
            // Re-read indices after waking up
            continue;
        }


        if (get_user(kernel_char_buffer[char_write_index], buffer++)){
            return -EFAULT;
        }
        /* * WMB: Write Memory Barrier.
         * This forces the 'get_user' data to be committed to the 
         * kernel_char_buffer BEFORE the write_index is allowed to change.
         */
        wmb();

        char_write_index = next_write_index;
    
        len--;
        bytes_written++;
    }

    return bytes_written;
}

#endif
// --- END: Simple Character Driver Functions ---

// --- BEGIN: Base Character Driver Functions ---
// --- First Legacy Implementation ---
// simple implementation of character driver
// Single Producer, Single Consumer
#ifdef BASE_CHAR_DRIVER
// Function called when user-space reads from /dev/telemetry
static ssize_t dev_read_old(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    int msg_len = strlen(msg);

    // Check if user has already read the whole message
    if (*offset >= msg_len) {
        return 0; // End of file
    }

    // copy_to_user returns the number of bytes that could NOT be copied
    if (copy_to_user(buffer, msg, msg_len)) {
        return -EFAULT;
    }

    // Move the offset so the next read knows we are done
    *offset += msg_len;
    
    printk(KERN_INFO "Telemetry: Sent data to user space\n");
    return msg_len;
}
#endif

#if 0 
// cleanly remove this code for now
// This is the old base character driver implementation 
// - I have changed the high level design to have multiple minors numbers
// TODO: I AM KEEPING THIS FOR NOW TO RESOLVE ANY BUILD ISSUES
static int __init telemetry_init(void) {
    // 0 tells the kernel to dynamically assign a major number
    major_number = register_chrdev(0, DEVICE_NAME, &fops);

    if (major_number < 0) {
        printk(KERN_ALERT "Telemetry: Failed to register a major number\n");
        return major_number;
    }

#ifdef SIMPLE_CHAR_DRIVER
    kernel_char_buffer = kmalloc(RING_BUFFER_CAPACITY, GFP_KERNEL);
    if (!kernel_char_buffer) {
#else   
    //ring_ptr = kzalloc(sizeof(shared_telemetry_ring), GFP_KERNEL);
    // DO THIS instead of kzalloc(Raw Page allocation - hardware aligned & mmap safe):
    ring_ptr = (shared_telemetry_ring *)get_zeroed_page(GFP_KERNEL);
    if (!ring_ptr) {
#endif
        unregister_chrdev(major_number, DEVICE_NAME);
        return -ENOMEM;
    }
    // THIS IS THE CRITICAL LINE FOR YOUR LOGS:
    printk(KERN_INFO "Telemetry: Module loaded with major number %d\n", major_number);
    return 0;
}

static void __exit telemetry_exit(void) {

    unregister_chrdev(major_number, DEVICE_NAME);
#ifdef SIMPLE_CHAR_DRIVER
    kfree(kernel_char_buffer);
#else
    free_page((unsigned long)ring_ptr);
#endif
printk(KERN_INFO "Telemetry: Module removed\n");
}

module_init(telemetry_init);
module_exit(telemetry_exit);
#endif
