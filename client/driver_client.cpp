#include <iostream>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <cstring>
#include "vnpu_common.h"

int tenant_id = -1;
int dev_fd = -1;

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

void print_menu() {
    std::cout << "\n===== vNPU Driver Client (Tenant " << tenant_id << ") =====" << std::endl;
    std::cout << "1. Trigger Matrix Multiply (4x4, A=0, B=16, C=32)" << std::endl;
    std::cout << "2. Test ARM64 Assembly (Checksum/Addition)" << std::endl;
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
    
    int choice;
    while (true) {
        print_menu();
        if (!(std::cin >> choice)) break;
        if (choice == 0) break;

        vnpu_command cmd;
        memset(&cmd, 0, sizeof(cmd)); 

        switch(choice) {
            case 1:
                cmd.type = CMD_MATRIX_MULTIPLY; 
                cmd.params[0] = 0;   
                cmd.params[1] = 16;  
                cmd.params[2] = 32;  
                cmd.params[3] = 4;   
                
                std::cout << "[Driver Client] Submitting CMD_MATRIX_MULTIPLY..." << std::endl;
                send_command(cmd);
                break;
            case 2:
                cmd.type = CMD_CHECKSUM; 
                cmd.params[0] = 100;
                cmd.params[1] = 250;
                send_command(cmd);
                break;
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