import socket
import struct
import numpy as np
import streamlit as st
import fcntl
import os
import time
import subprocess

SERVER_IP = '127.0.0.1'
SERVER_PORT = 8080
HEADER_SIZE = 32
NPU_MEM_SIZE = 640 * 480
NPU_MEM_BYTES = NPU_MEM_SIZE * 4
TOTAL_STATE_SIZE = HEADER_SIZE + NPU_MEM_BYTES + 8 + (256 * 24) 

VNPU_MAGIC = ord('V')
CMD_CLEAR = 1
CMD_DRAW_RECT = 2
CMD_CHECKSUM = 4
CMD_MATRIX_MULTIPLY = 5
CMD_VECTOR_ADD = 6
CMD_HANG = 9
VNPU_IOCTL_SUBMIT_CMD = (1 << 30) | (24 << 16) | (VNPU_MAGIC << 8) | 1

def check_firmware_alive():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(1.0)
        if s.connect_ex((SERVER_IP, SERVER_PORT)) == 0:
            try:
                s.sendall(struct.pack('<B', 99))
            except Exception:
                pass
            return True
        return False

def perform_watchdog_recovery():
    subprocess.run(["pkill", "-f", "./build/firmware"])
    time.sleep(1)
    subprocess.Popen(["./build/firmware"])
    time.sleep(2)

def send_weights(offset, matrix):
    data_bytes = matrix.astype(np.float32).tobytes()
    size_in_bytes = len(data_bytes)
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(5.0)
        s.connect((SERVER_IP, SERVER_PORT))
        s.sendall(struct.pack('<B I I', 1, offset, size_in_bytes))
        s.sendall(data_bytes)

def send_diagnostic_command(cmd_id):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(5.0)
        s.connect((SERVER_IP, SERVER_PORT))
        s.sendall(struct.pack('<B I', 2, cmd_id))
        response = s.recv(1024)
        return response.decode('utf-8')

def read_npu_state():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(10.0)
        s.connect((SERVER_IP, SERVER_PORT))
        s.sendall(struct.pack('<B', 0))
        
        data = bytearray()
        while len(data) < TOTAL_STATE_SIZE:
            packet = s.recv(65536)
            if not packet:
                break
            data.extend(packet)
            
        return bytes(data)

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
    This system implements two distinct communication planes:
    1. **Control Plane (Kernel IPC):** Streamlit UI sends lightweight commands via `ioctl()`. The Linux Kernel writes them to a Ring Buffer and signals the Firmware via `eventfd`.
    2. **Data Plane (TCP Bypass):** Large matrix weights and full memory state reads bypass the Kernel completely, using a local TCP socket to prevent kernel space memory congestion.
    """)
    st.graphviz_chart('''
        digraph G {
            rankdir=LR;
            node [shape=box, style=filled, fillcolor=lightgrey];
            UI [label="Streamlit UI (User Space)"];
            Kernel [label="vNPU Driver (Kernel Space)", fillcolor=lightblue];
            FW [label="Firmware (User Space)", fillcolor=lightgreen];
            UI -> Kernel [label="ioctl (Commands)"];
            Kernel -> FW [label="eventfd (IRQ Trigger)"];
            UI -> FW [label="TCP 8080 (Weights/State)"];
        }
    ''')

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
            raw_state = read_npu_state()
            if len(raw_state) >= HEADER_SIZE + NPU_MEM_BYTES:
                npu_mem_raw = np.frombuffer(raw_state[HEADER_SIZE:HEADER_SIZE+NPU_MEM_BYTES], dtype=np.float32)
                img = npu_mem_raw.reshape((480, 640)).astype(np.uint8)
                st.image(img, caption="640x480 VRAM View")
            else:
                st.error("Truncated data")

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

    if st.button("Push Weights"):
        send_weights(0, st.session_state.mat_A)
        send_weights(dim * dim * 4, st.session_state.mat_B)
        st.success("Memory updated via TCP")

    if st.button("CMD_MATRIX_MULTIPLY (Execute)"):
        res = submit_command(CMD_MATRIX_MULTIPLY, p0=0, p1=dim*dim, p2=dim*dim*2, p3=dim)
        st.code(res)

    if st.button("Pull GEMM Results"):
        raw_state = read_npu_state()
        if len(raw_state) >= HEADER_SIZE + NPU_MEM_BYTES:
            npu_mem_raw = np.frombuffer(raw_state[HEADER_SIZE:HEADER_SIZE+NPU_MEM_BYTES], dtype=np.float32)
            offset_c = dim * dim * 2
            mat_C = np.copy(npu_mem_raw[offset_c : offset_c + (dim * dim)]).reshape(dim, dim)
            expected_C = np.dot(st.session_state.mat_A, st.session_state.mat_B)
            mse = ((mat_C - expected_C)**2).mean()
            st.dataframe(mat_C, height=150)
            st.metric("Mean Squared Error (MSE)", f"{mse:.8f}")

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
            raw_state = read_npu_state()
            npu_mem_raw = np.frombuffer(raw_state[HEADER_SIZE:HEADER_SIZE+NPU_MEM_BYTES], dtype=np.float32)
            st.info(f"Value at offset {chk_dest}: {npu_mem_raw[chk_dest]}")

    with c2:
        st.write("Direct OOB Command (Bypass Kernel)")
        diag_cmd_id = st.number_input("TCP Diag Command ID", 1, 99, 1)
        if st.button("Send Direct TCP Command"):
            reply = send_diagnostic_command(diag_cmd_id)
            st.success(f"Response: {reply}")
            
        st.write("Fault Injection")
        if st.button("CMD_HANG (Trigger Crash)"):
            res = submit_command(CMD_HANG)
            st.error(res)