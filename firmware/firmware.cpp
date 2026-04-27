#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <csignal>
#include <atomic>
#include <thread>
#include <cstring>
#include <fstream>
#include <ucontext.h>
#include <chrono>
#include "vnpu_common.h" 
#include "vnpu_logger.h"

#define LOG_FILE "firmware.log"
#define TAG "Firmware"

vnpu_shared_state* global_npu_ptr = nullptr;

void crash_handler(int sig, siginfo_t *info, void *context) {
    if (!global_npu_ptr) _exit(1);
    
    ucontext_t *uc = (ucontext_t *)context;
    std::ofstream report("crash_report.txt");
    
    report << "=== FATAL: Hardware Fault Detected ===\n";
    report << "Signal Number: " << sig << "\n";
    report << "Faulting Memory Address: " << info->si_addr << "\n";
    
#ifdef __x86_64__
    report << "RIP: 0x" << std::hex << uc->uc_mcontext.gregs[REG_RIP] << "\n";
    report << "RSP: 0x" << std::hex << uc->uc_mcontext.gregs[REG_RSP] << "\n";
    report << "RBP: 0x" << std::hex << uc->uc_mcontext.gregs[REG_RBP] << "\n";
#elif defined(__aarch64__)
    report << "PC: 0x" << std::hex << uc->uc_mcontext.pc << "\n";
    report << "SP: 0x" << std::hex << uc->uc_mcontext.sp << "\n";
    report << "X0: 0x" << std::hex << uc->uc_mcontext.regs[0] << "\n";
#endif

    report << "\n[Ring Buffer Snapshot]\n";
    report << "Head: " << global_npu_ptr->head << " | Tail: " << global_npu_ptr->tail << "\n";
    report.close();

    int fd = open("crash_dump.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, global_npu_ptr, sizeof(vnpu_shared_state));
        close(fd);
    }
    
    LOG_FATAL("SIGSEGV captured. Wrote crash_report.txt and crash_dump.bin", LOG_FILE, TAG);
    _exit(1);
}

void watchdog_thread(vnpu_shared_state* npu) {
    while (npu->running) {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        npu->last_heartbeat = (uint64_t)now;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

bool recv_all(int socket, uint8_t* buffer, size_t length) {
    size_t bytes_received = 0;
    while (bytes_received < length) {
        ssize_t result = read(socket, buffer + bytes_received, length - bytes_received);
        if (result <= 0) return false;
        bytes_received += result;
    }
    return true;
}

bool send_all(int socket, const uint8_t* buffer, size_t length) {
    size_t bytes_sent = 0;
    while (bytes_sent < length) {
        ssize_t result = write(socket, buffer + bytes_sent, length - bytes_sent);
        if (result <= 0) return false;
        bytes_sent += result;
    }
    return true;
}

void tcp_server_thread(vnpu_shared_state* npu) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8080);
    
    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 3);
    
    LOG_INFO("TCP Server listening on port 8080", LOG_FILE, TAG);

    while (npu->running) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;
        
        uint8_t mode;
        if (recv_all(client, &mode, 1)) {
            if (mode == 0) {
                send_all(client, (const uint8_t*)npu, sizeof(vnpu_shared_state));
            } else if (mode == 1) {
                uint32_t offset, size_in_bytes;
                if (recv_all(client, (uint8_t*)&offset, 4) && recv_all(client, (uint8_t*)&size_in_bytes, 4)) {
                    uint32_t max_bytes = NPU_MEM_SIZE * sizeof(float);
                    if (size_in_bytes <= max_bytes && offset <= (max_bytes - size_in_bytes)) {
                        recv_all(client, (uint8_t*)npu->npu_mem + offset, size_in_bytes);
                    } else {
                        LOG_ERROR("TCP Write blocked: Out of bounds or Integer Overflow", LOG_FILE, TAG);
                    }
                }
            }
        }
        close(client);
    }
}

void process_command(vnpu_shared_state* npu, vnpu_command& cmd) {
    switch (cmd.type) {
        case CMD_MATRIX_MULTIPLY: {
            uint32_t offA = cmd.params[0], offB = cmd.params[1], offC = cmd.params[2];
            uint32_t dim = cmd.params[3]; 
            
            uint64_t req_space = (uint64_t)dim * dim;
            if ((offA + req_space > NPU_MEM_SIZE) || (offB + req_space > NPU_MEM_SIZE) || (offC + req_space > NPU_MEM_SIZE)) {
                LOG_ERROR("CMD_MATRIX_MULTIPLY bounds check failed", LOG_FILE, TAG);
                break;
            }

            float* A = &npu->npu_mem[offA];
            float* B = &npu->npu_mem[offB];
            float* C = &npu->npu_mem[offC];
            
            for (uint32_t i = 0; i < dim; ++i) {
                for (uint32_t j = 0; j < dim; ++j) {
                    float sum = 0;
                    for (uint32_t k = 0; k < dim; ++k) {
                        sum += A[i * dim + k] * B[k * dim + j];
                    }
                    C[i * dim + j] = sum;
                }
            }
            npu->temperature += 1.5f; 
            break;
        }
        case CMD_CHECKSUM: { 
            LOG_INFO("Processing CMD_CHECKSUM (Hardware Simulation)", LOG_FILE, TAG);
            break;
        }
        case CMD_HANG: {
            LOG_ERROR("Executing CMD_HANG - Triggering intentional SIGSEGV", LOG_FILE, TAG);
            volatile int* bad_ptr = nullptr;
            *bad_ptr = 42; 
            break;
        }
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);

    int fd = open("/dev/vnpu0", O_RDWR);
    if (fd < 0) {
        LOG_FATAL("Failed to open /dev/vnpu0", LOG_FILE, TAG);
        return 1;
    }
    
    vnpu_shared_state* npu = static_cast<vnpu_shared_state*>(
        mmap(nullptr, sizeof(vnpu_shared_state), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    
    if (npu == MAP_FAILED) {
        LOG_FATAL("mmap failed", LOG_FILE, TAG);
        close(fd);
        return 1;
    }
    
    global_npu_ptr = npu;

    int irq_fd = eventfd(0, EFD_NONBLOCK);
    ioctl(fd, VNPU_IOCTL_SET_EVENTFD, irq_fd);

    npu->temperature = 37.0f;
    npu->running = 1;
    
    std::thread net_thread(tcp_server_thread, npu);
    net_thread.detach();

    std::thread wd_thread(watchdog_thread, npu);
    wd_thread.detach();

    std::atomic<uint32_t>* head = reinterpret_cast<std::atomic<uint32_t>*>(&npu->head);
    std::atomic<uint32_t>* tail = reinterpret_cast<std::atomic<uint32_t>*>(&npu->tail);
    uint64_t irq_count;

    while (npu->running) {
        if (read(irq_fd, &irq_count, sizeof(irq_count)) > 0) {
            uint32_t current_tail = tail->load(std::memory_order_acquire);
            uint32_t current_head = head->load(std::memory_order_relaxed);

            while (current_head != current_tail) {
                process_command(npu, npu->ring[current_head]);
                current_head = (current_head + 1) % RING_BUFFER_SIZE;
            }
            head->store(current_head, std::memory_order_release);
        } else {
            usleep(1000); 
        }
    }
    return 0;
}