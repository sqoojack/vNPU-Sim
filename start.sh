#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

echo "=== 0. Force Cleanup Previous Session ==="
# Kill any running firmware or driver instances
pkill -f "./build/firmware" || true
pkill -f "./build/driver" || true

# If the device is still busy, force close the file descriptor
if [ -e /dev/vnpu0 ]; then
    sudo fuser -k /dev/vnpu0 || true
fi

# Remove old module if it exists
if lsmod | grep -q "vnpu_drv"; then
    echo "Unloading existing vnpu_drv..."
    sudo rmmod vnpu_drv
fi

echo "=== 1. Compile and Load Kernel Module ==="
cd kernel_module
make
sudo insmod vnpu_drv.ko
# Node should be auto-created if you updated the C code with device_create()
# If not, you might still need: sudo chmod 666 /dev/vnpu0
cd ..

echo "=== 2. Build C++ Firmware and Driver ==="
mkdir -p build
cd build
cmake ..
make
cd ..

echo "=== 3. Launch NPU Firmware ==="
# Run firmware in the background
./build/firmware &
FW_PID=$!

# Wait for TCP Server initialization
sleep 2

echo "=== 4. Launch Streamlit App ==="
# Run the Streamlit application
streamlit run scripts/app.py

# Final Cleanup when Streamlit exits
kill $FW_PID || true
sudo rmmod vnpu_drv || true
echo "System shutdown complete."