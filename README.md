# vNPU-Sim: AI-Accelerator System Simulator

vNPU-Sim is a hardware-software co-design project that simulates a Neural Processing Unit (NPU) environment. It features a custom Linux Kernel Module for low-latency Inter-Process Communication (IPC), a C++ firmware for hardware logic emulation, and a Streamlit-based Python application for interactive deep learning workload orchestration.

## System Architecture

The project is architected into three distinct layers to mimic real-world AI acceleration stacks:

1.  **Kernel Space (LKM)**: A Linux Kernel Module (`vnpu_drv.ko`) that manages shared memory regions and provides a command submission interface via `ioctl` and `eventfd`. It implements a high-performance, asynchronous communication path between the driver client and the hardware simulator.
2.  **Hardware Emulation (Firmware)**: A C++ process that simulates NPU hardware behavior, including GEMM (General Matrix Multiply) operations, 2D rendering, and hardware reliability tracking. It utilizes **Zero-copy mmap** to access the shared memory provided by the kernel, eliminating data copy overhead.
3.  **User Space (Dashboard & Driver)**:
    * **Streamlit Dashboard**: A professional web-based interface for loading weights, triggering hardware execution via the driver, and visualizing computed results with real-time hardware state monitoring.
    * **Driver Client**: A C++ implementation that submits hardware commands to the kernel-side ring buffer, used for high-frequency testing and stress tests.

## Key Features

* **Zero-Copy Memory Management**: Leverages `remap_vmalloc_range` in the kernel and `mmap` in user space to provide direct access to simulated device memory.
* **Asynchronous Command Submission**: Uses a lock-protected ring buffer and `eventfd` for non-blocking, interrupt-driven command processing between layers.
* **Real-time Hardware Synchronization**: Implements a synchronization logic in the frontend that monitors ring buffer pointers (`head` and `tail`) to ensure the system waits for hardware idle before retrieving computation results.
* **Automated Numerical Validation**: Integrated GEMM verification pipeline that compares NPU simulated results against CPU-based NumPy calculations to ensure simulation accuracy.
* **Visual Hardware Feedback**: Real-time VRAM buffer rendering for GPU-like memory operations, providing immediate visual proof of successful memory manipulation.
* **Watchdog & Fault Recovery**: Simulated hardware reliability tracking with automated watchdog recovery mechanisms for firmware failures or intentional fault injections.

## Prerequisites

* **OS**: Linux (Tested on Ubuntu 20.04/22.04)
* **Compiler**: GCC/G++ (C++17 support)
* **Build System**: CMake (>= 3.14)
* **Python**: Python 3.x with NumPy and Streamlit
* **Kernel**: Linux Kernel Headers (for LKM compilation)

## Quick Start (Automated)

You can build and launch the entire system using the provided automation script:

```bash
chmod +x start.sh
./start.sh
```

This script performs the following actions:
1. Compiles and inserts the `vnpu_drv` kernel module.
2. Sets appropriate permissions for `/dev/vnpu0`.
3. Builds the C++ Firmware and Driver using CMake.
4. Launches the Firmware in the background.
5. Starts the Streamlit Dashboard.

## Manual Execution Workflow

If you prefer to run components individually, follow these steps in separate terminals:

### 1. Compile and Load the Kernel Module
```bash
cd kernel_module
make
sudo insmod vnpu_drv.ko
sudo chmod 666 /dev/vnpu0
```

### 2. Build C++ Components
```bash
mkdir build && cd build
cmake ..
make
```

### 3. Launch Services
* **Terminal A (Firmware)**: `./build/firmware`
* **Terminal B (Dashboard)**: `streamlit run scripts/app.py`

## Testing & Cleanup

* **Dashboard Verification**: Use the "Run Full NPU GEMM Pipeline & Verify" button in the NPU Sim tab to automatically test end-to-end data flow and numerical correctness.
* **Stress Testing**: Run `python3 scripts/stress_test.py` to verify ring buffer backpressure and synchronization under high load.
* **Cleanup**: To unload the driver and stop background services, use the cleanup commands provided in the `start.sh` exit routine or run `sudo rmmod vnpu_drv` manually.
