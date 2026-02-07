// g++ firmware.cpp -o firmware -lpthread
// ./firmware
#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <pthread.h>
#include <time.h>
#include "common.h"

struct GPUState* gpu;

// --- 1. ARM64 Assembly Optimization ---

uint32_t asm_add(uint32_t a, uint32_t b) {
    uint32_t res;
    // volatile 告訴編譯器不要優化這段 Code，必須執行
    __asm__ volatile (
        "add %w0, %w1, %w2"  // 組合語言指令: add res, a, b
        : "=r" (res)         // 輸出: res
        : "r" (a), "r" (b)   // 輸入: a, b
    );
    return res;
}

// --- 2. Watchdog Mechanism ---
// 模擬硬體看門狗計時器 (Hardware Watchdog Timer)
void* watchdog_thread(void* arg) {
    std::cout << "[Watchdog] Monitoring thread started..." << std::endl;
    
    while (gpu->running) {
        uint64_t current_time = (uint64_t)time(NULL);
        
        // 檢查 Heartbeat 是否超時 (例如超過 3 秒沒更新)
        if (current_time - gpu->last_heartbeat > 3) {
            std::cerr << "\n[!!! CRITICAL !!!] WATCHDOG TIMEOUT DETECTED!" << std::endl;
            std::cerr << "[Watchdog] Firmware hung. Performing Hard Reset..." << std::endl;
            
            // 執行重置邏輯
            gpu->watchdog_reset_count++;
            gpu->temperature = 37.0f; // 重置溫度
            
            // 在真實硬體會重啟，這裡我們強制跳出死迴圈或重啟主迴圈
            // 為了演示，我們簡單地更新 heartbeat 讓系統「復活」
            // 並印出重置訊息
            gpu->last_heartbeat = current_time; 
            
            // 注意：如果主執行緒卡在 while(true)，這裡通常需要 kill process
            // 但為了讓 Streamlit 繼續顯示，我們這裡只做標記
            // 真實情況：exit(1); 
        }
        sleep(1);
    }
    return NULL;
}

// 初始化同步物件
void init_sync_obj(pthread_mutex_t* m, pthread_cond_t* c_full, pthread_cond_t* c_empty) {
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(m, &mattr);
    pthread_mutexattr_destroy(&mattr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(c_full, &cattr);
    pthread_cond_init(c_empty, &cattr);
    pthread_condattr_destroy(&cattr);
}

void put_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
        gpu->vram[y * WIDTH + x] = color;
    }
}

void process_command(int tenant_id, Command& cmd) {
    switch (cmd.type) {
        case CMD_CLEAR: {
            uint32_t color = cmd.params[0];
            for (int i = 0; i < WIDTH * HEIGHT; ++i) gpu->vram[i] = color;
            break;
        }
        case CMD_DRAW_RECT: {
            int x = cmd.params[0], y = cmd.params[1];
            int w = cmd.params[2], h = cmd.params[3];
            uint32_t color = cmd.params[4];
            for (int dy = 0; dy < h; ++dy) {
                for (int dx = 0; dx < w; ++dx) {
                    put_pixel(x + dx, y + dy, color);
                }
            }
            gpu->temperature += 0.8f;
            usleep(500);
            break;
        }
        case CMD_DMA_TEXTURE: {
            int x = cmd.params[0], y = cmd.params[1];
            int w = cmd.params[2], h = cmd.params[3];
            uint32_t* src = gpu->tenants[tenant_id].dma_staging_area;
            for (int dy = 0; dy < h; ++dy) {
                for (int dx = 0; dx < w; ++dx) {
                    put_pixel(x + dx, y + dy, src[dy * w + dx]);
                }
            }
            break;
        }
        case CMD_CHECKSUM: {
            // 使用 ARM64 Assembly 進行運算
            uint32_t val1 = cmd.params[0];
            uint32_t val2 = cmd.params[1];
            uint32_t result = asm_add(val1, val2);
            std::cout << "[ASM Accelerator] " << val1 << " + " << val2 << " = " << result << std::endl;
            // 視覺化回饋：加法執行時，畫面閃一下白色
            gpu->vram[0] = 0xFFFFFFFF; 
            break;
        }
        case CMD_HANG: {
            std::cout << "[Firmware] Received HANG command. Simulating infinite loop..." << std::endl;
            // 故意進入死迴圈，不更新 Heartbeat
            // 這會導致 Watchdog Thread 在 3 秒後報警
            while(true) {
                // Do nothing, just freeze
                // 為了讓 OS 不殺掉我們，sleep 極短時間但不出讓邏輯控制
                usleep(100); 
            }
            break;
        }
    }
}

int main() {
    int fd = open(SHM_FILENAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { perror("open"); return 1; }
    if (ftruncate(fd, sizeof(GPUState)) == -1) { perror("ftruncate"); return 1; }

    gpu = (GPUState*)mmap(0, sizeof(GPUState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memset(gpu, 0, sizeof(GPUState));
    gpu->magic = 0x56475055;
    gpu->running = 1;
	gpu->temperature = 37.0f;
    gpu->last_heartbeat = (uint64_t)time(NULL);
    
    for(int i=0; i<MAX_TENANTS; i++) {
        init_sync_obj(&gpu->tenants[i].lock, &gpu->tenants[i].not_full, &gpu->tenants[i].not_empty);
    }

    // 啟動 Watchdog 執行緒
    pthread_t wd_thread;
    pthread_create(&wd_thread, NULL, watchdog_thread, NULL);

    std::cout << "[vGPU Firmware] Booted (Features: ARM64 ASM, Watchdog, Throttling)" << std::endl;

    while (gpu->running) {
        // 1. 餵狗 (Feed the Dog)
        // 只要這行有執行，Watchdog 就不會叫
        gpu->last_heartbeat = (uint64_t)time(NULL);

        // 2. 溫度過高機制 (Thermal Throttling)
        if (gpu->temperature > 85.0f) {
            std::cout << "[Thermal] Overheating! Throttling performance..." << std::endl;
            usleep(50000); // 強制降頻 (Sleep 50ms)
            gpu->temperature -= 0.5f; // 模擬降溫
        }

        bool idle = true;
        for (int i = 0; i < MAX_TENANTS; ++i) {
            TenantContext& ctx = gpu->tenants[i];
            if (!ctx.active) continue;

            pthread_mutex_lock(&ctx.lock);
            if (ctx.head != ctx.tail) {
                idle = false;
                Command cmd = ctx.cmd_buffer[ctx.head];
                ctx.head = (ctx.head + 1) % RING_BUFFER_SIZE;
                pthread_cond_signal(&ctx.not_full);
                pthread_mutex_unlock(&ctx.lock);

                process_command(i, cmd);
                gpu->temperature += 0.5f; // 操作導致升溫
            } else {
                pthread_mutex_unlock(&ctx.lock);
            }
        }

        if (idle) {
            if (gpu->temperature > 30.0f) gpu->temperature -= 0.1f; // 自然冷卻
            usleep(2000);
        } else {
            usleep(100);
        }
        gpu->frame_counter++;
    }

    return 0;
}