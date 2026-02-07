// g++ driver.cpp -o driver -lpthread
// ./driver 0
// ./driver 1
#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include "common.h"

struct GPUState* gpu;
int tenant_id = -1;

void send_command(Command cmd) {
    TenantContext* ctx = &gpu->tenants[tenant_id];
    pthread_mutex_lock(&ctx->lock);
    int next_tail = (ctx->tail + 1) % RING_BUFFER_SIZE;
    while (next_tail == ctx->head) {
        std::cout << "[Driver] Buffer full..." << std::endl;
        pthread_cond_wait(&ctx->not_full, &ctx->lock);
        next_tail = (ctx->tail + 1) % RING_BUFFER_SIZE;
    }
    ctx->cmd_buffer[ctx->tail] = cmd;
    ctx->tail = next_tail;
    pthread_cond_signal(&ctx->not_empty);
    pthread_mutex_unlock(&ctx->lock);

    std::cout << "[Driver] Success: Command sent" << std::endl;
}

void print_menu() {
    std::cout << "\n===== vGPU Driver (Tenant " << tenant_id << ") =====" << std::endl;
    std::cout << "1. Draw Rect (Test Rendering)" << std::endl;
    std::cout << "2. Clear Screen" << std::endl;
    std::cout << "3. Test ARM64 Assembly (Addition)" << std::endl;
    std::cout << "4. DMA Transfer" << std::endl;
    std::cout << "9. SIMULATE HANG (Test Watchdog)" << std::endl;
    std::cout << "0. Exit" << std::endl;
    std::cout << "Select: ";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ./driver <tenant_id 0-1>" << std::endl;
        return 1;
    }
    tenant_id = atoi(argv[1]);

    int fd = open(SHM_FILENAME, O_RDWR, 0666);
    if (fd == -1) { std::cerr << "Run firmware first!" << std::endl; return 1; }
    
    gpu = (GPUState*)mmap(0, sizeof(GPUState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    gpu->tenants[tenant_id].active = 1;
    gpu->tenants[tenant_id].pid = getpid();
    
    int choice;
    while (true) {
        print_menu();
        if (!(std::cin >> choice)) break;
        if (choice == 0) break;

        Command cmd;
        switch(choice) {
            case 1: // Draw
                cmd.type = CMD_DRAW_RECT;
                cmd.params[0] = rand() % (WIDTH-50); 
                cmd.params[1] = rand() % (HEIGHT-50);
                cmd.params[2] = 50; cmd.params[3] = 50;
                cmd.params[4] = 0xFF00FF00;
                send_command(cmd);
                break;
            case 2: // Clear
                cmd.type = CMD_CLEAR;
                cmd.params[0] = 0xFF222222;
                send_command(cmd);
                break;
            case 3: // ASM Checksum
                cmd.type = CMD_CHECKSUM;
                cmd.params[0] = 100;
                cmd.params[1] = 250;
                send_command(cmd);
                std::cout << "Sent Checksum Command (100 + 250). Check Firmware Output." << std::endl;
                break;
            case 4: // DMA
                {
                    uint32_t* dma = gpu->tenants[tenant_id].dma_staging_area;
                    for(int i=0; i<64*64; i++) dma[i] = 0xFFFF00FF;
                    cmd.type = CMD_DMA_TEXTURE;
                    cmd.params[0] = 100; cmd.params[1] = 100; 
                    cmd.params[2] = 64; cmd.params[3] = 64;
                    send_command(cmd);
                }
                break;
            case 9: // HANG
                cmd.type = CMD_HANG;
                send_command(cmd);
                std::cout << "!!! Sent HANG Command. Watch firmware console for Reset !!!" << std::endl;
                break;
        }
    }

    gpu->tenants[tenant_id].active = 0;
    return 0;
}