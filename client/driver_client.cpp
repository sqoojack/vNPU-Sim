#include <iostream>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <cstring>
#include <sys/mman.h>
#include <iomanip>
#include "vnpu_common.h"

int tenant_id = -1;
int dev_fd = -1;
vnpu_shared_state* global_npu = nullptr;

void send_command(vnpu_command cmd) {
    while (true) {
        int ret = ioctl(dev_fd, VNPU_IOCTL_SUBMIT_CMD, &cmd);
        
        if (ret < 0) {
            if (errno == EBUSY) {
                std::cout << "[Driver Client] Buffer full. Retrying in 1ms..." << std::endl;
                usleep(1000);
            } else {
                perror("[Driver Client] IOCTL failed");
                break;
            }
        } else {
            std::cout << "[Driver Client] Success: Command submitted to Kernel" << std::endl;
            break;
        }
    }
}

void print_vram_ascii() {
    std::cout << "[Driver Client] VRAM 64x48 View (Scaled 10x10):" << std::endl;
    for(int r = 0; r < 48; ++r) {
        for(int c = 0; c < 64; ++c) {
            float val = global_npu->npu_mem[(r * 10) * 640 + (c * 10)];
            std::cout << (val > 0 ? "#" : ".");
        }
        std::cout << std::endl;
    }
}

void print_matrix(int offset, int dim) {
    std::cout << "[Driver Client] Matrix View (Offset: " << offset << "):" << std::endl;
    for (int i = 0; i < dim; ++i) {
        for (int j = 0; j < dim; ++j) {
            std::cout << std::setw(6) << global_npu->npu_mem[offset + i * dim + j] << " ";
        }
        std::cout << std::endl;
    }
}

void print_menu() {
    std::cout << "\n===== vNPU Driver Client (Tenant " << tenant_id << ") =====" << std::endl;
    std::cout << "1. Trigger Matrix Multiply (4x4) & Show Result" << std::endl;
    std::cout << "2. Test ARM64 Assembly (Checksum/Addition)" << std::endl;
    std::cout << "3. Clear VRAM" << std::endl;
    std::cout << "4. Draw Rectangle in VRAM" << std::endl;
    std::cout << "5. Show VRAM ASCII Art" << std::endl;
    std::cout << "6. Test Custom Micro-ISA (Compile & Execute Vector Add)" << std::endl;
    std::cout << "9. SIMULATE HANG (Test Crash Dump & Watchdog)" << std::endl;
    std::cout << "0. Exit" << std::endl;
    std::cout << "Select: ";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ./driver_client <tenant_id 0-1>" << std::endl;
        return 1;
    }
    tenant_id = atoi(argv[1]);

    dev_fd = open("/dev/vnpu0", O_RDWR);
    if (dev_fd < 0) { 
        std::cerr << "Error: Cannot open /dev/vnpu0. Is the Kernel Module loaded?" << std::endl; 
        return 1; 
    }

    global_npu = static_cast<vnpu_shared_state*>(mmap(nullptr, sizeof(vnpu_shared_state), PROT_READ | PROT_WRITE, MAP_SHARED, dev_fd, 0));
    if (global_npu == MAP_FAILED) {
        std::cerr << "Error: mmap failed in client." << std::endl;
        return 1;
    }
    
    int choice;
    while (true) {
        print_menu();
        if (!(std::cin >> choice)) break;
        if (choice == 0) break;

        vnpu_command cmd;
        memset(&cmd, 0, sizeof(cmd)); 

        switch(choice) {
            case 1:
                for(int i=0; i<16; ++i) global_npu->npu_mem[0 + i] = (float)(i + 1);
                for(int i=0; i<16; ++i) global_npu->npu_mem[16 + i] = 2.0f;
                
                cmd.type = CMD_MATRIX_MULTIPLY; 
                cmd.params[0] = 0;   
                cmd.params[1] = 16;  
                cmd.params[2] = 32;  
                cmd.params[3] = 4;   
                
                std::cout << "[Driver Client] Submitting CMD_MATRIX_MULTIPLY..." << std::endl;
                send_command(cmd);
                usleep(500000); 
                print_matrix(32, 4);
                std::cout << "[Driver Client] Hardware Temp increased to: " << global_npu->temperature << " C" << std::endl;
                break;

            case 2:
                cmd.type = CMD_CHECKSUM; 
                cmd.params[0] = 100;
                cmd.params[1] = 250;
                send_command(cmd);
                break;

            case 3:
                cmd.type = CMD_CLEAR;
                cmd.params[0] = 0;
                send_command(cmd);
                std::cout << "[Driver Client] VRAM Cleared." << std::endl;
                break;

            case 4:
                cmd.type = CMD_DRAW_RECT;
                cmd.params[0] = 100; 
                cmd.params[1] = 100; 
                cmd.params[2] = 400; 
                cmd.params[3] = 200; 
                cmd.params[4] = 255; 
                send_command(cmd);
                std::cout << "[Driver Client] Rectangle command sent." << std::endl;
                break;

            case 5:
                print_vram_ascii();
                break;

            case 6: {
                // ==========================================
                // Compiler Simulation: Generate Machine Code
                // ==========================================
                uint32_t data_offset_a = 100;
                uint32_t data_offset_b = 104; // WARP_SIZE is 4
                uint32_t data_offset_c = 108; // Result destination
                
                // Populate dummy vector data
                for(int i = 0; i < 4; ++i) global_npu->npu_mem[data_offset_a + i] = 5.0f;
                for(int i = 0; i < 4; ++i) global_npu->npu_mem[data_offset_b + i] = 10.0f;

                uint32_t prog_offset = 200; // Arbitrary offset for program memory
                
                // Treat npu_mem as a raw byte buffer for writing instructions
                vNPU_Instruction* program_mem = reinterpret_cast<vNPU_Instruction*>(&global_npu->npu_mem[prog_offset]);
                
                // Hack: We need a place to store the immediate memory addresses so OP_LOAD can find them.
                // In a real ISA, we'd have a LOAD_IMM instruction.
                global_npu->npu_mem[300] = (float)data_offset_a;
                global_npu->npu_mem[301] = (float)data_offset_b;
                global_npu->npu_mem[302] = (float)data_offset_c;

                // Instruction 0: OP_LOAD (Load memory address A from memory[300] into register 0)
                program_mem[0].opcode = OP_LOAD;
                program_mem[0].dest_reg = 0;
                program_mem[0].src_reg1 = 0; 
                
                // Instruction 1: OP_LOAD (Load memory address B from memory[301] into register 1)
                program_mem[1].opcode = OP_LOAD;
                program_mem[1].dest_reg = 1;
                program_mem[1].src_reg1 = 1;

                // Instruction 2: OP_ADD (R2 = R0 + R1)
                program_mem[2].opcode = OP_ADD;
                program_mem[2].dest_reg = 2;
                program_mem[2].src_reg1 = 0;
                program_mem[2].src_reg2 = 1;

                // Instruction 3: OP_STORE (Store R2 to memory address C, which is in R3)
                // Note: Simplified for demonstration. Real compilation involves complex register allocation.
                program_mem[3].opcode = OP_HALT;

                std::cout << "[Driver Client] Compiling and writing NPU instructions to memory..." << std::endl;
                
                cmd.type = CMD_EXEC_PROGRAM;
                cmd.params[0] = prog_offset; // Tell NPU where to start fetching
                send_command(cmd);
                
                usleep(500000); // Wait for hardware execution
                
                std::cout << "[Driver Client] Execution complete! Result at offset 108:" << std::endl;
                for(int i = 0; i < 4; ++i) {
                    std::cout << global_npu->npu_mem[data_offset_c + i] << " ";
                }
                std::cout << "\n(Check firmware.log for Cache Miss/Hit statistics!)" << std::endl;
                break;
            }

            case 9:
                cmd.type = CMD_HANG; 
                std::cout << "[Driver Client] Submitting CMD_HANG to trigger SIGSEGV..." << std::endl;
                send_command(cmd);
                break;

            default:
                std::cout << "Invalid choice." << std::endl;
                break;
        }
    }

    close(dev_fd);
    return 0;
}