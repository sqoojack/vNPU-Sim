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
#include <sstream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include "vnpu_common.h" 

#include "vnpu_logger.h"
#define LOG_FILE "firmware.log"
#define TAG "Firmware"

vnpu_shared_state* global_npu_ptr = nullptr;

enum class LogLevel { INFO, WARN, ERROR, FATAL };

class Logger {
public:
    static void log(LogLevel level, const std::string& msg) {
        static std::mutex log_mutex;
        std::lock_guard<std::mutex> lock(log_mutex);

        std::ofstream log_file("firmware.log", std::ios::app);
        
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::string label;
        switch (level) {
            case LogLevel::INFO:  label = "[INFO]"; break;
            case LogLevel::WARN:  label = "[WARN]"; break;
            case LogLevel::ERROR: label = "[ERROR]"; break;
            case LogLevel::FATAL: label = "[FATAL]"; break;
        }

        std::stringstream ss;
        ss << std::put_time(std::localtime(&now), "%Y-%m-%d %H:%M:%S") << " " << label << " " << msg << std::endl;
        
        if (log_file.is_open()) log_file << ss.str();
        std::cout << ss.str(); 
    }
};

#define LOG_INFO(msg) Logger::log(LogLevel::INFO, msg)
#define LOG_ERROR(msg) Logger::log(LogLevel::ERROR, msg)
#define LOG_FATAL(msg) Logger::log(LogLevel::FATAL, msg)

void crash_handler(int sig, siginfo_t *info, void *context) {
    if (!global_npu_ptr) _exit(1);
    
    int fd = open("crash_dump.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, &global_npu_ptr->head, sizeof(__u32));
        write(fd, &global_npu_ptr->tail, sizeof(__u32));
        write(fd, global_npu_ptr->ring, sizeof(vnpu_command) * RING_BUFFER_SIZE);
        write(fd, global_npu_ptr->npu_mem, 1024);
        close(fd);
    }
    LOG_FATAL("SIGSEGV detected. Crash dump written to crash_dump.bin");
    _exit(1);
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
    
    LOG_INFO("TCP Server listening on port 8080");

    while (npu->running) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;
        
        uint8_t mode;
        if (read(client, &mode, 1) > 0) {
            if (mode == 0) {
                write(client, npu, sizeof(vnpu_shared_state));
            } else if (mode == 1) {
                uint32_t offset, size_in_bytes;
                read(client, &offset, 4);
                read(client, &size_in_bytes, 4);
                read(client, (uint8_t*)npu->npu_mem + offset, size_in_bytes);
                LOG_INFO("Received AI weights via TCP");
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
            LOG_INFO("Processed CMD_MATRIX_MULTIPLY");
            break;
        }
        case CMD_HANG: {
            LOG_ERROR("Executing CMD_HANG - Triggering intentional SIGSEGV");
            volatile int* bad_ptr = nullptr;
            *bad_ptr = 42; 
            break;
        }
    }
}

int main() {
    LOG_INFO("Firmware system starting...", LOG_FILE, TAG);
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);

    int fd = open("/dev/vnpu0", O_RDWR);
    if (fd < 0) {
        LOG_FATAL("Failed to open /dev/vnpu0. Is the kernel module loaded?");
        return 1;
    }
    
    vnpu_shared_state* npu = static_cast<vnpu_shared_state*>(
        mmap(nullptr, sizeof(vnpu_shared_state), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    global_npu_ptr = npu;

    int irq_fd = eventfd(0, EFD_NONBLOCK);
    ioctl(fd, VNPU_IOCTL_SET_EVENTFD, irq_fd);

    npu->temperature = 37.0f;
    LOG_INFO("Firmware initialized successfully");
    
    std::thread net_thread(tcp_server_thread, npu);
    net_thread.detach();

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