#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/eventfd.h>
#include "../include/vnpu_common.h" // 確認路徑是否符合您的目錄結構

#define DEVICE_NAME "vnpu0"

static int major_num;
static struct cdev vnpu_cdev;
static struct vnpu_shared_state *shared_mem;
static struct eventfd_ctx *irq_ctx = NULL;

static int vnpu_mmap(struct file *filp, struct vm_area_struct *vma) {
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn = vmalloc_to_pfn(shared_mem);
    
    if (size > sizeof(struct vnpu_shared_state)) return -EINVAL;
    
    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
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

            next_tail = (shared_mem->tail + 1) % RING_BUFFER_SIZE;
            if (next_tail == shared_mem->head) {
                return -EBUSY; 
            }

            shared_mem->ring[shared_mem->tail] = user_cmd;
            smp_store_release(&shared_mem->tail, next_tail);

            if (irq_ctx) {
                eventfd_signal(irq_ctx, 1);
            }
            break;

        case VNPU_IOCTL_SET_EVENTFD:
            irq_ctx = eventfd_ctx_fdget(arg);
            if (IS_ERR(irq_ctx)) return PTR_ERR(irq_ctx);
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
    
    memset(shared_mem, 0, sizeof(struct vnpu_shared_state));
    shared_mem->magic = 0x564E5055; // "VNPU" in ASCII
    shared_mem->running = 1;

    printk(KERN_INFO "vNPU Driver loaded: /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit vnpu_exit(void) {
    if (irq_ctx) eventfd_ctx_put(irq_ctx);
    vfree(shared_mem);
    cdev_del(&vnpu_cdev);
    unregister_chrdev_region(MKDEV(major_num, 0), 1);
    printk(KERN_INFO "vNPU Driver unloaded\n");
}

module_init(vnpu_init);
module_exit(vnpu_exit);
MODULE_LICENSE("GPL");