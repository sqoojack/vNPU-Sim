#ifndef VNPU_COMMON_H
#define VNPU_COMMON_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VNPU_MAGIC 'V'
#define VNPU_IOCTL_SUBMIT_CMD _IOW(VNPU_MAGIC, 1, struct vnpu_command)
#define VNPU_IOCTL_SET_EVENTFD _IOW(VNPU_MAGIC, 2, int)

#define NPU_MEM_SIZE (640 * 480) 
#define L1_CACHE_SIZE 4096
#define RING_BUFFER_SIZE 256

// --- Micro-ISA Definitions (Custom NPU Instruction Set) ---
enum vNPU_Opcode {
    OP_LOAD  = 0x01, // Load data from memory to register
    OP_STORE = 0x02, // Store data from register to memory
    OP_ADD   = 0x03, // Add two registers: dest = src1 + src2
    OP_MUL   = 0x04, // Multiply two registers: dest = src1 * src2
    OP_HALT  = 0xFF  // Halt execution
};

// 32-bit Instruction Format
struct vNPU_Instruction {
    __u8 opcode;   
    __u8 dest_reg; 
    __u8 src_reg1; 
    __u8 src_reg2; 
};

enum VNPUCommandType {
    CMD_CLEAR = 1,
    CMD_DRAW_RECT = 2,
    CMD_CHECKSUM = 4,
    CMD_MATRIX_MULTIPLY = 5,
    CMD_VECTOR_ADD = 6,
    CMD_HANG = 9,
    CMD_MEM_TRANSFER = 10,
    CMD_EXEC_PROGRAM = 11 // NEW: Execute custom NPU machine code
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
    __u64 total_cycles;          
    
    float npu_mem[NPU_MEM_SIZE]; 
    float l1_cache[L1_CACHE_SIZE]; 

    __u32 head; 
    __u32 tail; 
    struct vnpu_command ring[RING_BUFFER_SIZE];
};

#endif