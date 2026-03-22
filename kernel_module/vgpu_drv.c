#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/eventfd.h>
#include "vgpu_common.h"

#define DEVICE_NAME "vgpu0"

static int major_num;
static struct cdev vgpu_cdev;
static struct vgpu_shared_state *shared_mem;
static struct eventfd_ctx *irq_ctx = NULL; // 用於觸發中斷通知 Firmware

// mmap 實作：將核心配置的記憶體映射給使用者空間 (Driver Client 與 Firmware)
static int vgpu_mmap(struct file *filp, struct vm_area_struct *vma) {
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn = vmalloc_to_pfn(shared_mem);
    
    if (size > sizeof(struct vgpu_shared_state)) return -EINVAL;
    
    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
        return -EAGAIN;
    }
    return 0;
}

// IOCTL 實作：取代原先直接寫入陣列的做法
static long vgpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    struct vgpu_command user_cmd;
    __u32 next_tail;

    switch (cmd) {
        case VGPU_IOCTL_SUBMIT_CMD:
            if (copy_from_user(&user_cmd, (struct vgpu_command __user *)arg, sizeof(user_cmd)))
                return -EFAULT;

            next_tail = (shared_mem->tail + 1) % RING_BUFFER_SIZE;
            if (next_tail == shared_mem->head) {
                return -EBUSY; // Buffer Full (背壓)
            }

            // 1. 寫入指令
            shared_mem->ring[shared_mem->tail] = user_cmd;

            // 2. 記憶體屏障：確保指令寫入完成後，tail 才會更新
            // 這避免了 CPU 重新排序導致 Firmware 讀到舊的資料
            smp_store_release(&shared_mem->tail, next_tail);

            // 3. 觸發 eventfd 中斷通知 Firmware 處理
            if (irq_ctx) {
                eventfd_signal(irq_ctx, 1);
            }
            break;

        case VGPU_IOCTL_SET_EVENTFD:
            // Firmware 註冊 eventfd 以接收中斷
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
    .mmap = vgpu_mmap,
    .unlocked_ioctl = vgpu_ioctl,
};

static int __init vgpu_init(void) {
    dev_t dev;
    if (alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME) < 0) {
        printk(KERN_ERR "vNPU: Failed to allocate char device region\n"); // 使用 KERN_ERR
        return -1;
    }
    major_num = MAJOR(dev);
    cdev_init(&vgpu_cdev, &fops);
    if (cdev_add(&vgpu_cdev, dev, 1) < 0) {
        printk(KERN_ERR "vNPU: Failed to add cdev\n");
        unregister_chrdev_region(dev, 1);
        return -1;
    }

    shared_mem = vmalloc_user(sizeof(struct vgpu_shared_state));
    if (!shared_mem) {
        printk(KERN_ERR "vNPU: Failed to allocate shared memory\n");
        return -ENOMEM;
    }
    
    memset(shared_mem, 0, sizeof(struct vgpu_shared_state));
    shared_mem->magic = 0x56475055;
    shared_mem->running = 1;

    printk(KERN_INFO "vNPU Driver loaded: /dev/%s\n", DEVICE_NAME); // 使用 KERN_INFO
    return 0;
}

static void __exit vgpu_exit(void) {
    if (irq_ctx) eventfd_ctx_put(irq_ctx);
    vfree(shared_mem);
    cdev_del(&vgpu_cdev);
    unregister_chrdev_region(MKDEV(major_num, 0), 1);
    printk(KERN_INFO "vGPU Driver unloaded\n");
}

module_init(vgpu_init);
module_exit(vgpu_exit);
MODULE_LICENSE("GPL");