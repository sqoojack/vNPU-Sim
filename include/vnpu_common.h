#ifndef VNPU_COMMON_H
#define VNPU_COMMON_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VNPU_MAGIC 'V'
#define VNPU_IOCTL_SUBMIT_CMD _IOW(VNPU_MAGIC, 1, struct vnpu_command)
#define VNPU_IOCTL_SET_EVENTFD _IOW(VNPU_MAGIC, 2, int)

#define NPU_MEM_SIZE (640 * 480) 
#define RING_BUFFER_SIZE 256

enum VNPUCommandType {
    CMD_CLEAR = 1,
    CMD_DRAW_RECT = 2,
    CMD_CHECKSUM = 4,
    CMD_MATRIX_MULTIPLY = 5,
    CMD_HANG = 9
};

struct vnpu_command {
    __u32 type;
    __u32 params[5]; 
};

struct vnpu_shared_state {
    __u32 magic;                 
    __u32 running;
    __u32 frame_counter;
    float temperature;
    __u64 last_heartbeat;
    __u32 watchdog_reset_count;

    float npu_mem[NPU_MEM_SIZE]; 

    __u32 head; 
    __u32 tail; 
    struct vnpu_command ring[RING_BUFFER_SIZE];
};

#endif