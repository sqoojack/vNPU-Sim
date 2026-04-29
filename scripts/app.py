import struct
import numpy as np
import streamlit as st
import fcntl
import os
import time
import subprocess
import mmap

NPU_MEM_SIZE = 640 * 480
L1_CACHE_SIZE = 4096
RING_BUFFER_SIZE = 256
NPU_MEM_BYTES = NPU_MEM_SIZE * 4
L1_CACHE_BYTES = L1_CACHE_SIZE * 4

HEADER_SIZE = 40
TOTAL_STATE_SIZE = HEADER_SIZE + NPU_MEM_BYTES + L1_CACHE_BYTES + 8 + (RING_BUFFER_SIZE * 24)

PAGE_SIZE = 4096
MMAP_SIZE = ((TOTAL_STATE_SIZE + PAGE_SIZE - 1) // PAGE_SIZE) * PAGE_SIZE

VNPU_MAGIC = ord('V')
CMD_CLEAR = 1
CMD_DRAW_RECT = 2
CMD_CHECKSUM = 4
CMD_MATRIX_MULTIPLY = 5
CMD_VECTOR_ADD = 6
CMD_HANG = 9
VNPU_IOCTL_SUBMIT_CMD = (1 << 30) | (24 << 16) | (VNPU_MAGIC << 8) | 1

def get_shared_mmap():
    if not os.path.exists("/dev/vnpu0"):
        return None
    fd = os.open("/dev/vnpu0", os.O_RDWR | os.O_SYNC)
    try:
        mm = mmap.mmap(fd, MMAP_SIZE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        return mm
    except Exception as e:
        st.error(f"Failed to mmap: {e}")
        return None
    finally:
        os.close(fd)

def check_firmware_alive():
    mm = get_shared_mmap()
    if mm is None:
        return False
    
    mm.seek(4)
    running = struct.unpack('<I', mm.read(4))[0]
    if running == 0:
        mm.close()
        return False
        
    mm.seek(16)
    last_hb = struct.unpack('<Q', mm.read(8))[0]
    mm.close()
    
    current_time = int(time.time())
    if current_time - last_hb > 3:
        return False
    return True

def perform_watchdog_recovery():
    subprocess.run(["pkill", "-f", "./build/firmware"])
    time.sleep(1)
    subprocess.Popen(["./build/firmware"])
    time.sleep(2)

def send_weights(offset_in_floats, matrix):
    mm = get_shared_mmap()
    if mm is None:
        return
    data_bytes = matrix.astype(np.float32).tobytes()
    byte_offset = HEADER_SIZE + (offset_in_floats * 4)
    mm.seek(byte_offset)
    mm.write(data_bytes)
    mm.close()

def submit_command(cmd_type, p0=0, p1=0, p2=0, p3=0, p4=0):
    cmd_bytes = struct.pack('<I 5I', cmd_type, p0, p1, p2, p3, p4)
    if not os.path.exists("/dev/vnpu0"):
        return "Error: /dev/vnpu0 not found"
    try:
        with open("/dev/vnpu0", "wb") as f:
            fcntl.ioctl(f.fileno(), VNPU_IOCTL_SUBMIT_CMD, cmd_bytes)
        return f"Success: Submit CMD {cmd_type}"
    except Exception as e:
        return f"IOCTL Error: {e}"

st.set_page_config(page_title="vNPU Simulator", layout="wide")
st.title("vNPU-Sim System Monitoring Dashboard")

is_alive = check_firmware_alive()
if not is_alive:
    st.error("Firmware offline. Triggering Watchdog Recovery...")
    perform_watchdog_recovery()
    st.rerun()
else:
    st.success("Firmware online")

tab1, tab2, tab3, tab4 = st.tabs(["Architecture & Comm", "GPU Sim (VRAM)", "NPU Sim (GEMM)", "System Diagnostics"])

with tab1:
    st.subheader("Communication Architecture")
    st.markdown("""
    This system implements full Zero-Copy IPC:
    1. **Control Plane (Kernel IPC):** Streamlit UI sends lightweight commands via `ioctl()`. The Linux Kernel writes them to a Ring Buffer and signals the Firmware via `eventfd`.
    2. **Data Plane (Direct mmap):** Streamlit directly mmaps `/dev/vnpu0` for weight loading and result reading, completely eliminating TCP overhead and socket buffer copies.
    """)
    
    mm = get_shared_mmap()
    if mm:
        mm.seek(32)
        total_cycles = struct.unpack('<Q', mm.read(8))[0]
        st.metric("Total Simulated Cycles", total_cycles)
        mm.close()

with tab2:
    st.subheader("GPU Simulation: 2D Memory Manipulation")
    colA, colB = st.columns(2)
    with colA:
        clear_color = st.number_input("Clear Color (0-255)", 0, 255, 0)
        if st.button("CMD_CLEAR (Clear VRAM)"):
            res = submit_command(CMD_CLEAR, p0=clear_color)
            st.success(res)
            
        st.markdown("---")
        rect_x = st.number_input("X", 0, 640, 100)
        rect_y = st.number_input("Y", 0, 480, 100)
        rect_w = st.number_input("Width", 1, 640, 200)
        rect_h = st.number_input("Height", 1, 480, 150)
        rect_color = st.number_input("Color (0-255)", 0, 255, 255)
        
        if st.button("CMD_DRAW_RECT (Render)"):
            res = submit_command(CMD_DRAW_RECT, p0=rect_x, p1=rect_y, p2=rect_w, p3=rect_h, p4=rect_color)
            st.success(res)

    with colB:
        st.write("VRAM Buffer Visualization")
        if st.button("Fetch VRAM Frame"):
            mm = get_shared_mmap()
            if mm:
                mm.seek(HEADER_SIZE)
                raw_data = mm.read(NPU_MEM_BYTES)
                npu_mem_raw = np.frombuffer(raw_data, dtype=np.float32)
                img = npu_mem_raw.reshape((480, 640)).astype(np.uint8)
                st.image(img, caption="640x480 VRAM View")
                mm.close()

with tab3:
    dim = st.number_input("Matrix Dimension", min_value=2, max_value=256, value=64, step=1)
    if "current_dim" not in st.session_state or st.session_state.current_dim != dim:
        st.session_state.current_dim = dim
        st.session_state.mat_A = np.random.rand(dim, dim).astype(np.float32)
        st.session_state.mat_B = np.random.rand(dim, dim).astype(np.float32)

    col1, col2 = st.columns(2)
    with col1:
        st.write("Matrix A")
        st.dataframe(st.session_state.mat_A, height=150)
    with col2:
        st.write("Matrix B")
        st.dataframe(st.session_state.mat_B, height=150)

    if st.button("Push Weights (Zero-Copy)"):
        send_weights(0, st.session_state.mat_A)
        send_weights(dim * dim, st.session_state.mat_B)
        st.success("Memory updated via mmap")

    if st.button("CMD_MATRIX_MULTIPLY (Execute)"):
        res = submit_command(CMD_MATRIX_MULTIPLY, p0=0, p1=dim*dim, p2=dim*dim*2, p3=dim)
        st.code(res)

    if st.button("Pull GEMM Results"):
        mm = get_shared_mmap()
        if mm:
            offset_c = dim * dim * 2
            byte_offset = HEADER_SIZE + (offset_c * 4)
            mm.seek(byte_offset)
            raw_data = mm.read(dim * dim * 4)
            mat_C = np.frombuffer(raw_data, dtype=np.float32).reshape(dim, dim)
            
            expected_C = np.dot(st.session_state.mat_A, st.session_state.mat_B)
            mse = ((mat_C - expected_C)**2).mean()
            st.dataframe(mat_C, height=150)
            st.metric("Mean Squared Error (MSE)", f"{mse:.8f}")
            mm.close()

with tab4:
    st.subheader("Hardware Testing & Diagnostics")
    c1, c2 = st.columns(2)
    with c1:
        chk_offset = st.number_input("Checksum Offset", 0, NPU_MEM_SIZE, 0)
        chk_size = st.number_input("Checksum Size", 1, NPU_MEM_SIZE, 1024)
        chk_dest = st.number_input("Checksum Result Dest Offset", 0, NPU_MEM_SIZE, NPU_MEM_SIZE-1)
        if st.button("CMD_CHECKSUM"):
            res = submit_command(CMD_CHECKSUM, p0=chk_offset, p1=chk_size, p2=chk_dest)
            st.success(res)
            
        if st.button("Read Checksum Result"):
            mm = get_shared_mmap()
            if mm:
                byte_offset = HEADER_SIZE + (chk_dest * 4)
                mm.seek(byte_offset)
                val = struct.unpack('<f', mm.read(4))[0]
                st.info(f"Value at offset {chk_dest}: {val}")
                mm.close()

    with c2:
        st.write("Fault Injection")
        if st.button("CMD_HANG (Trigger Crash)"):
            res = submit_command(CMD_HANG)
            st.error(res)