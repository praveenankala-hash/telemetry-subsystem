#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>      // Required for registration functions
#include <linux/uaccess.h> // Required for copy_to_user
#include <linux/slab.h>    // Required for kmalloc/kfree
#include <linux/cache.h> // Ensure this header is included
#include <linux/mm.h>  // Required for remap_pfn_range and page manipulation
#include "telemetry.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Praveen Ankala");
MODULE_DESCRIPTION("Subsystem Allocation & VFS Multiplexer");

/* CRITICAL MISSING HEADERS FOR CDEV & SYSFS */
#include <linux/cdev.h>   /* Fixes cdev_init, cdev_add, cdev_del, struct cdev */
#include <linux/device.h> /* Fixes class_create, device_create, device_destroy */

/* Global shared infrastructure pointer accessible by other module blocks */
shared_telemetry_ring *ring_ptr = NULL;
EXPORT_SYMBOL_GPL(ring_ptr); // Crucial for net_filter module cross-linking

static dev_t telemetry_dev_num;
static struct class *telemetry_class = NULL;
static struct cdev telemetry_cdev;

/* Extern operations and synchronization structures from telemetry_core.c */
extern int dev_open(struct inode *inodep, struct file *filep);
extern ssize_t dev_read(struct file *filep, char __user *buffer, size_t len, loff_t *offset);
extern ssize_t dev_write(struct file *filep, const char __user *buffer, size_t len, loff_t *offset);
extern wait_queue_head_t char_wait_queue; // Externed to allow the IOCTL handler to trigger a wake-up call

/* Define our explicit cross-boundary wake-up control command */
#define TELEMETRY_IOC_MAGIC 't'
#define TELEMETRY_IOC_WAKE_WRITER _IO(TELEMETRY_IOC_MAGIC, 1)

extern ssize_t telemetry_net_read(struct file *file, char __user *user_buf, size_t count, loff_t *ppos);
/* ---------------------------------------------------------------------------------- */
/* FILE OPERATIONS FOR MINOR 0: MASTER MMAP NODE                                      */
/* ---------------------------------------------------------------------------------- */
static int telemetry_mmap_open(struct inode *inode, struct file *filp) {
    return 0;
}

static int telemetry_mmap_io(struct file *filep, struct vm_area_struct *vma) {
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn;

    if (size > PAGE_SIZE_4K) {
        return -EINVAL;
    }

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    pfn = virt_to_phys(ring_ptr) >> PAGE_SHIFT;
    
    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
        pr_err("telemetry_base: mmap remap_pfn_range failed\n");
        return -EAGAIN;
    }
    return 0;
}

static int ioctl_count = 0;
/* IOCTL multiplexer to unblock streaming writers from zero-copy contexts safely */
static long telemetry_mmap_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    switch (cmd) {
        case TELEMETRY_IOC_WAKE_WRITER:
            /* Wake up any sleeping producer threads in dev_write */
           // pr_info("telemetry_base: IOCTL wake kick issued, count: %d\n", ioctl_count++);
            wake_up_interruptible(&char_wait_queue);
            return 0;
        default:
            return -EINVAL;
    }
}

static const struct file_operations mmap_fops = {
    .owner          = THIS_MODULE,
    .open           = telemetry_mmap_open,
    .mmap           = telemetry_mmap_io,
    .unlocked_ioctl = telemetry_mmap_ioctl, /* Added to handle zero-copy consumer wake kicks */
};

/* ---------------------------------------------------------------------------------- */
/* TENANT 0 ROUTING WRAPPER                                                           */
/* ---------------------------------------------------------------------------------- */
static const struct file_operations char_fops = {
    .owner = THIS_MODULE,
    .open  = dev_open,
    .read  = dev_read,
    .write = dev_write,
};

/* ---------------------------------------------------------------------------------- */
/* TENANT 0 ROUTING WRAPPER                                                           */
/* ---------------------------------------------------------------------------------- */

static const struct file_operations net_fops = {
    .owner = THIS_MODULE,
    .read  = telemetry_net_read,
};

/* ---------------------------------------------------------------------------------- */
/* CENTRAL INTERCEPT MASTER MULTIPLEXER                                               */
/* ---------------------------------------------------------------------------------- */
static int telemetry_master_open(struct inode *inode, struct file *filp) {
    unsigned int minor = iminor(inode);

    switch (minor) {
        case MINOR_TELEMETRY_MMAP:
            filp->f_op = &mmap_fops;
            break;
        case MINOR_TELEMETRY_CHAR:
            filp->f_op = &char_fops;
            break;
        case MINOR_TELEMETRY_NET:
            filp->f_op = &net_fops;
            break; // Attach specialized net reader operations
        default:
            return -ENXIO; /* Minor node registered, but tracing target not started yet */
    }

    if (filp->f_op->open)
        return filp->f_op->open(inode, filp);
        
    return 0;
}

static const struct file_operations master_telemetry_fops = {
    .owner = THIS_MODULE,
    .open  = telemetry_master_open, 
};

/* ---------------------------------------------------------------------------------- */
/* SUBSYSTEM LIFECYCLE MANAGEMENT                                                     */
/* ---------------------------------------------------------------------------------- */
static int __init telemetry_base_init(void) {
    int ret;

    /* 1. Allocate the 4096-Byte Master Page */
    ring_ptr = (shared_telemetry_ring *)get_zeroed_page(GFP_KERNEL);
    if (!ring_ptr) {
        pr_err("telemetry_base: Failed to allocate physical page boundary\n");
        return -ENOMEM;
    }

    /* 2. Dynamically allocate device Major/Minor block pools */
    ret = alloc_chrdev_region(&telemetry_dev_num, 0, TELEMETRY_MINORS, TELEMETRY_DEVICE_NAME);
    if (ret < 0) {
        free_page((unsigned long)ring_ptr);
        return ret;
    }

    /* 3. Add character execution link to the kernel core VFS mapping table */
    cdev_init(&telemetry_cdev, &master_telemetry_fops);
    telemetry_cdev.owner = THIS_MODULE;
    ret = cdev_add(&telemetry_cdev, telemetry_dev_num, TELEMETRY_MINORS);
    if (ret < 0) {
        unregister_chrdev_region(telemetry_dev_num, TELEMETRY_MINORS);
        free_page((unsigned long)ring_ptr);
        return ret;
    }

    /* 4. Create class entries to trigger sysfs / devnode generation hooks */
    telemetry_class = class_create(TELEMETRY_DEVICE_NAME);
    if (IS_ERR(telemetry_class)) {
        cdev_del(&telemetry_cdev);
        unregister_chrdev_region(telemetry_dev_num, TELEMETRY_MINORS);
        free_page((unsigned long)ring_ptr);
        return PTR_ERR(telemetry_class);
    }

    /* 5. Deploy node files to user space dynamically */
    device_create(telemetry_class, NULL, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_MMAP),  NULL, "telemetry_mmap");
    device_create(telemetry_class, NULL, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_CHAR),  NULL, "telemetry_char");
    device_create(telemetry_class, NULL, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_NET),   NULL, "telemetry_net");
    device_create(telemetry_class, NULL, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_BLOCK), NULL, "telemetry_block");
    device_create(telemetry_class, NULL, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_MEM),   NULL, "telemetry_mem");
    device_create(telemetry_class, NULL, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_CPU),   NULL, "telemetry_cpu");

    pr_info("telemetry_base: Device registered. Major: %d Pool Live.\n", MAJOR(telemetry_dev_num));
    return 0;
}

static void __exit telemetry_base_exit(void) {
    device_destroy(telemetry_class, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_MMAP));
    device_destroy(telemetry_class, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_CHAR));
    device_destroy(telemetry_class, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_NET));
    device_destroy(telemetry_class, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_BLOCK));
    device_destroy(telemetry_class, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_MEM));
    device_destroy(telemetry_class, MKDEV(MAJOR(telemetry_dev_num), MINOR_TELEMETRY_CPU));
    
    class_destroy(telemetry_class);
    cdev_del(&telemetry_cdev);
    unregister_chrdev_region(telemetry_dev_num, TELEMETRY_MINORS);
    
    if (ring_ptr) {
        free_page((unsigned long)ring_ptr);
    }
    pr_info("telemetry_base: Infrastructure unloaded cleanly\n");
}

module_init(telemetry_base_init);
module_exit(telemetry_base_exit);