#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <csignal>
#include <atomic>
#include <thread>
#include <vector>
#include <cstring>
#include <fstream>
#include <ucontext.h>
#include <chrono>
#include <list>
#include <unordered_map>
#include "vnpu_common.h" 
#include "vnpu_logger.h"

#define LOG_FILE "firmware.log"
#define TAG "Firmware"

const uint64_t LATENCY_ALU = 1;
const uint64_t LATENCY_L1_HIT = 2;
const uint64_t LATENCY_DRAM_ACCESS = 100;

#define WARP_SIZE 4       // Simulate 4 threads per warp (SIMT)
#define NUM_REGISTERS 16  // 16 general purpose registers per thread

vnpu_shared_state* global_npu_ptr = nullptr;

// ==========================================
// 1. Memory Hierarchy: LRU Cache Simulator
// ==========================================
class LRUCache {
private:
    size_t capacity;
    std::list<uint32_t> lru_list; 
    std::unordered_map<uint32_t, std::list<uint32_t>::iterator> cache_map;

public:
    uint64_t hits = 0;
    uint64_t misses = 0;

    LRUCache(size_t cap) : capacity(cap) {}

    // Simulates memory access and returns latency in cycles
    uint64_t access(uint32_t address) {
        uint32_t cache_line = address / 64; // Assume 64-byte cache line
        
        if (cache_map.find(cache_line) != cache_map.end()) {
            // Cache Hit: Move to front of LRU list
            lru_list.erase(cache_map[cache_line]);
            lru_list.push_front(cache_line);
            cache_map[cache_line] = lru_list.begin();
            hits++;
            return LATENCY_L1_HIT;
        } else {
            // Cache Miss: Fetch from DRAM
            if (lru_list.size() >= capacity) {
                uint32_t lru = lru_list.back();
                lru_list.pop_back();
                cache_map.erase(lru);
            }
            lru_list.push_front(cache_line);
            cache_map[cache_line] = lru_list.begin();
            misses++;
            return LATENCY_DRAM_ACCESS;
        }
    }
};

// ==========================================
// 2. Thread Scheduling: SIMT Warp Context
// ==========================================
struct Warp {
    int warp_id;
    uint32_t pc; // Program Counter
    bool is_active;
    
    // Register File: Each thread in the warp has its own registers
    float registers[WARP_SIZE][NUM_REGISTERS]; 

    Warp(int id) : warp_id(id), pc(0), is_active(true) {
        memset(registers, 0, sizeof(registers));
    }
};

// ==========================================
// 3. Instruction Execution Engine
// ==========================================
void execute_npu_program(vnpu_shared_state* npu, uint32_t prog_offset) {
    LRUCache l1_cache(128); // 128 cache lines
    std::vector<Warp> sm_warps;
    
    // Initialize 1 Warp for this test simulation
    Warp w0(0);
    w0.pc = prog_offset; 
    sm_warps.push_back(w0);

    bool sm_running = true;
    
    while (sm_running) {
        sm_running = false;

        for (auto& warp : sm_warps) {
            if (!warp.is_active) continue;
            sm_running = true; 

            // [FETCH] Get instruction from memory using Program Counter (pc)
            vNPU_Instruction* inst = reinterpret_cast<vNPU_Instruction*>(&npu->npu_mem[warp.pc]);

            // [DECODE & EXECUTE]
            switch (inst->opcode) {
                case OP_LOAD: {
                    // Get memory address from the source register of thread 0
                    uint32_t mem_addr = (uint32_t)warp.registers[0][inst->src_reg1]; 
                    uint64_t latency = l1_cache.access(mem_addr);
                    npu->total_cycles += latency;
                    
                    // SIMT: All threads in the warp load contiguous data concurrently
                    for (int t = 0; t < WARP_SIZE; ++t) {
                        warp.registers[t][inst->dest_reg] = npu->npu_mem[mem_addr + t];
                    }
                    warp.pc++; 
                    break;
                }
                case OP_STORE: {
                    uint32_t mem_addr = (uint32_t)warp.registers[0][inst->dest_reg]; 
                    uint64_t latency = l1_cache.access(mem_addr);
                    npu->total_cycles += latency;
                    
                    // SIMT: All threads store data concurrently
                    for (int t = 0; t < WARP_SIZE; ++t) {
                        npu->npu_mem[mem_addr + t] = warp.registers[t][inst->src_reg1];
                    }
                    warp.pc++; 
                    break;
                }
                case OP_ADD: {
                    for (int t = 0; t < WARP_SIZE; ++t) {
                        warp.registers[t][inst->dest_reg] = 
                            warp.registers[t][inst->src_reg1] + warp.registers[t][inst->src_reg2];
                    }
                    npu->total_cycles += LATENCY_ALU; 
                    warp.pc++;
                    break;
                }
                case OP_HALT: {
                    warp.is_active = false; 
                    break;
                }
                default: {
                    LOG_ERROR("Unknown Instruction Opcode", LOG_FILE, TAG);
                    warp.is_active = false;
                    break;
                }
            }
        }
    }
    
    std::string stats = "Execution Finished. Cache Hits: " + std::to_string(l1_cache.hits) + 
                        ", Misses: " + std::to_string(l1_cache.misses);
    LOG_INFO(stats, LOG_FILE, TAG);
}

// ==========================================
// Legacy Systems (Crash handler, Watchdog)
// ==========================================
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
        
        if (npu->temperature > 37.0f) {
            npu->temperature -= 0.5f;
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ==========================================
// Command Router
// ==========================================
void process_command(vnpu_shared_state* npu, vnpu_command& cmd) {
    uint64_t cycles_spent = 0;

    switch (cmd.type) {
        case CMD_EXEC_PROGRAM: {
            uint32_t prog_offset = cmd.params[0];
            LOG_INFO("Processing Custom ISA Program at offset " + std::to_string(prog_offset), LOG_FILE, TAG);
            execute_npu_program(npu, prog_offset);
            break;
        }
        case CMD_CLEAR: {
            uint32_t color = cmd.params[0];
            for (uint32_t i = 0; i < NPU_MEM_SIZE; ++i) {
                npu->npu_mem[i] = (float)color;
            }
            cycles_spent += (NPU_MEM_SIZE / 16) * LATENCY_DRAM_ACCESS;
            LOG_INFO("Processed CMD_CLEAR", LOG_FILE, TAG);
            break;
        }
        case CMD_DRAW_RECT: {
            uint32_t x = cmd.params[0], y = cmd.params[1], w = cmd.params[2], h = cmd.params[3], color = cmd.params[4]; 
            for (uint32_t r = y; r < y + h && r < 480; ++r) {
                for (uint32_t c = x; c < x + w && c < 640; ++c) {
                    npu->npu_mem[r * 640 + c] = (float)color;
                }
            }
            cycles_spent += (w * h / 16) * LATENCY_DRAM_ACCESS;
            LOG_INFO("Processed CMD_DRAW_RECT", LOG_FILE, TAG);
            break;
        }
        case CMD_CHECKSUM: { 
            uint32_t offset = cmd.params[0], size = cmd.params[1], dest_offset = cmd.params[2];
            if (offset + size <= NPU_MEM_SIZE && dest_offset < NPU_MEM_SIZE) {
                uint32_t sum = 0;
                for (uint32_t i = offset; i < offset + size; ++i) {
                    sum += (uint32_t)npu->npu_mem[i];
                }
                npu->npu_mem[dest_offset] = (float)sum;
                cycles_spent += size * LATENCY_DRAM_ACCESS + size * LATENCY_ALU;
                LOG_INFO("Processed CMD_CHECKSUM", LOG_FILE, TAG);
            } else {
                LOG_ERROR("CMD_CHECKSUM bounds error", LOG_FILE, TAG);
            }
            break;
        }
        case CMD_MATRIX_MULTIPLY: {
            uint32_t offA = cmd.params[0], offB = cmd.params[1], offC = cmd.params[2], dim = cmd.params[3]; 
            uint64_t req_space = (uint64_t)dim * dim;
            
            if ((offA + req_space > NPU_MEM_SIZE) || (offB + req_space > NPU_MEM_SIZE) || (offC + req_space > NPU_MEM_SIZE)) {
                LOG_ERROR("CMD_MATRIX_MULTIPLY bounds error", LOG_FILE, TAG);
                break;
            }

            float* A = &npu->npu_mem[offA];
            float* B = &npu->npu_mem[offB];
            float* C = &npu->npu_mem[offC];
            
            uint32_t num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) num_threads = 4;
            std::vector<std::thread> workers;
            
            auto worker_task = [&](uint32_t start_row, uint32_t end_row) {
                for (uint32_t i = start_row; i < end_row; ++i) {
                    for (uint32_t j = 0; j < dim; ++j) {
                        float sum = 0;
                        for (uint32_t k = 0; k < dim; ++k) {
                            sum += A[i * dim + k] * B[k * dim + j];
                        }
                        C[i * dim + j] = sum;
                    }
                }
            };

            uint32_t rows_per_thread = dim / num_threads;
            for (uint32_t t = 0; t < num_threads; ++t) {
                uint32_t start_row = t * rows_per_thread;
                uint32_t end_row = (t == num_threads - 1) ? dim : start_row + rows_per_thread;
                workers.emplace_back(worker_task, start_row, end_row);
            }
            
            for (auto& w : workers) {
                w.join();
            }

            uint64_t total_ops = (uint64_t)dim * dim * dim;
            uint64_t dram_loads = dim * dim * 2;
            uint64_t l1_hits = total_ops * 2 - dram_loads; 
            cycles_spent += (dram_loads * LATENCY_DRAM_ACCESS) + (l1_hits * LATENCY_L1_HIT) + (total_ops * LATENCY_ALU);
            cycles_spent /= num_threads; 
            
            npu->temperature += 1.5f; 
            LOG_INFO("Processed CMD_MATRIX_MULTIPLY", LOG_FILE, TAG);
            break;
        }
        case CMD_MEM_TRANSFER: {
            uint32_t src_offset = cmd.params[0], dest_offset = cmd.params[1], size = cmd.params[2], is_l1_dest = cmd.params[3];
            if (is_l1_dest && dest_offset + size <= L1_CACHE_SIZE && src_offset + size <= NPU_MEM_SIZE) {
                std::memcpy(&npu->l1_cache[dest_offset], &npu->npu_mem[src_offset], size * sizeof(float));
                cycles_spent += size * LATENCY_DRAM_ACCESS;
            }
            LOG_INFO("Processed CMD_MEM_TRANSFER", LOG_FILE, TAG);
            break;
        }
        case CMD_VECTOR_ADD: {
            uint32_t offA = cmd.params[0], offB = cmd.params[1], offC = cmd.params[2], len = cmd.params[3];
            float* A = &npu->npu_mem[offA];
            float* B = &npu->npu_mem[offB];
            float* C = &npu->npu_mem[offC];
            for(uint32_t i = 0; i < len; ++i) C[i] = A[i] + B[i];
            cycles_spent += len * LATENCY_DRAM_ACCESS * 3 + len * LATENCY_ALU;
            LOG_INFO("Processed CMD_VECTOR_ADD", LOG_FILE, TAG);
            break;
        }
        case CMD_HANG: {
            LOG_ERROR("Executing CMD_HANG - Triggering intentional SIGSEGV", LOG_FILE, TAG);
            volatile int* bad_ptr = nullptr;
            *bad_ptr = 42; 
            break;
        }
    }
    npu->total_cycles += cycles_spent;
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

    if (npu->last_heartbeat != 0) {
        npu->watchdog_reset_count += 1;
        LOG_INFO("Recovered from crash.", LOG_FILE, TAG);
    } else {
        npu->temperature = 37.0f;
        npu->watchdog_reset_count = 0;
        npu->total_cycles = 0;
    }

    int irq_fd = eventfd(0, EFD_NONBLOCK);
    ioctl(fd, VNPU_IOCTL_SET_EVENTFD, irq_fd);

    npu->running = 1;

    std::thread wd_thread(watchdog_thread, npu);
    wd_thread.detach();

    std::atomic<uint32_t>* head = reinterpret_cast<std::atomic<uint32_t>*>(&npu->head);
    std::atomic<uint32_t>* tail = reinterpret_cast<std::atomic<uint32_t>*>(&npu->tail);
    uint64_t irq_count;

    LOG_INFO("Firmware is running and waiting for commands...", LOG_FILE, TAG);

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