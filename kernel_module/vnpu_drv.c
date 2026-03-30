#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/eventfd.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h> 
#include "../include/vnpu_common.h"

#define DEVICE_NAME "vnpu0"

static struct class *vnpu_class = NULL;
static struct device *vnpu_device = NULL;

static int major_num;
static struct cdev vnpu_cdev;
static struct vnpu_shared_state *shared_mem;
static struct eventfd_ctx *irq_ctx = NULL;

static DEFINE_SPINLOCK(ring_lock);

static int vnpu_mmap(struct file *filp, struct vm_area_struct *vma) {
    unsigned long size = vma->vm_end - vma->vm_start;
    
    // 修復：vma 大小會對齊至分頁，因此檢查條件必須使用 PAGE_ALIGN
    if (size > PAGE_ALIGN(sizeof(struct vnpu_shared_state))) return -EINVAL;
    
    if (remap_vmalloc_range(vma, shared_mem, 0)) {
        return -EAGAIN;
    }
    return 0;
}

static long vnpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    struct vnpu_command user_cmd;
    __u32 next_tail;

    switch (cmd) {
        case VNPU_IOCTL_SUBMIT_CMD:
            if (copy_from_user(&user_cmd, (struct vnpu_command __user *)arg, sizeof(user_cmd)))
                return -EFAULT;

            spin_lock(&ring_lock);
            
            next_tail = (shared_mem->tail + 1) % RING_BUFFER_SIZE;
            if (next_tail == shared_mem->head) {
                spin_unlock(&ring_lock);
                return -EBUSY; 
            }

            shared_mem->ring[shared_mem->tail] = user_cmd;
            smp_store_release(&shared_mem->tail, next_tail);
            
            spin_unlock(&ring_lock);

            if (irq_ctx) {
                eventfd_signal(irq_ctx);
            }
            break;

        case VNPU_IOCTL_SET_EVENTFD:
            if (irq_ctx) {
                eventfd_ctx_put(irq_ctx);
            }
            
            irq_ctx = eventfd_ctx_fdget(arg);
            if (IS_ERR(irq_ctx)) {
                struct eventfd_ctx *err_ctx = irq_ctx;
                irq_ctx = NULL;
                return PTR_ERR(err_ctx);
            }
            break;

        default:
            return -ENOTTY;
    }
    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .mmap = vnpu_mmap,
    .unlocked_ioctl = vnpu_ioctl,
};

static int __init vnpu_init(void) {
    dev_t dev;
    if (alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME) < 0) {
        printk(KERN_ERR "vNPU: Failed to allocate char device region\n");
        return -1;
    }
    major_num = MAJOR(dev);
    cdev_init(&vnpu_cdev, &fops);
    if (cdev_add(&vnpu_cdev, dev, 1) < 0) {
        printk(KERN_ERR "vNPU: Failed to add cdev\n");
        unregister_chrdev_region(dev, 1);
        return -1;
    }

    shared_mem = vmalloc_user(sizeof(struct vnpu_shared_state));
    if (!shared_mem) {
        printk(KERN_ERR "vNPU: Failed to allocate shared memory\n");
        return -ENOMEM;
    }

    vnpu_class = class_create(DEVICE_NAME);
    if (IS_ERR(vnpu_class)) {
        cdev_del(&vnpu_cdev);
        unregister_chrdev_region(dev, 1);
        return PTR_ERR(vnpu_class);
    }

    vnpu_device = device_create(vnpu_class, NULL, dev, NULL, DEVICE_NAME);
    if (IS_ERR(vnpu_device)) {
        class_destroy(vnpu_class);
        cdev_del(&vnpu_cdev);
        unregister_chrdev_region(dev, 1);
        return PTR_ERR(vnpu_device);
    }
    
    memset(shared_mem, 0, sizeof(struct vnpu_shared_state));
    shared_mem->magic = 0x564E5055; 
    shared_mem->running = 1;

    printk(KERN_INFO "vNPU Driver loaded: /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit vnpu_exit(void) {
    if (vnpu_device) device_destroy(vnpu_class, MKDEV(major_num, 0));
    if (vnpu_class) class_destroy(vnpu_class);
    if (irq_ctx) {
        eventfd_ctx_put(irq_ctx);
        irq_ctx = NULL;
    }
    if (shared_mem) vfree(shared_mem);
    cdev_del(&vnpu_cdev);
    unregister_chrdev_region(MKDEV(major_num, 0), 1);
    printk(KERN_INFO "vNPU Driver unloaded\n");
}

module_init(vnpu_init);
module_exit(vnpu_exit);
MODULE_LICENSE("GPL");