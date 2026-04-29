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
CMD_MATRIX_MULTIPLY = 5
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

def trigger_hardware(dim):
    offA = 0
    offB = dim * dim
    offC = dim * dim * 2
    
    cmd_bytes = struct.pack('<I 5I', CMD_MATRIX_MULTIPLY, offA, offB, offC, dim, 0)
    
    if not os.path.exists("/dev/vnpu0"):
        return "Error: /dev/vnpu0 not found"

    try:
        with open("/dev/vnpu0", "wb") as f:
            fcntl.ioctl(f.fileno(), VNPU_IOCTL_SUBMIT_CMD, cmd_bytes)
        return f"Success: Submit CMD_MATRIX_MULTIPLY dim={dim}"
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

dim = st.sidebar.number_input("Matrix Dimension", min_value=2, max_value=256, value=64, step=1)

if "current_dim" not in st.session_state or st.session_state.current_dim != dim:
    st.session_state.current_dim = dim
    st.session_state.mat_A = np.random.rand(dim, dim).astype(np.float32)
    st.session_state.mat_B = np.random.rand(dim, dim).astype(np.float32)

st.subheader("1. Data Orchestration")
col1, col2 = st.columns(2)
with col1:
    st.write("Matrix A")
    st.dataframe(st.session_state.mat_A, height=200)
with col2:
    st.write("Matrix B")
    st.dataframe(st.session_state.mat_B, height=200)

if st.button("Push Weights"):
    try:
        send_weights(0, st.session_state.mat_A)
        send_weights(dim * dim * 4, st.session_state.mat_B)
        st.success("Memory updated")
    except Exception as e:
        st.error(f"Push failed: {e}")

st.subheader("2. Hardware Control")
if st.button("Execute GEMM"):
    result = trigger_hardware(dim)
    st.code(result)

st.subheader("3. Verification and Observability")
if st.button("Pull Results"):
    try:
        raw_state = read_npu_state()
        
        if len(raw_state) < HEADER_SIZE + (dim * dim * 3 * 4):
            st.error("Truncated data")
        else:
            header = struct.unpack('<I I I f Q I I', raw_state[0:32])
            temperature = header[3]
            last_heartbeat = header[4]
            watchdog_count = header[5]
            
            col_m1, col_m2 = st.columns(2)
            with col_m1:
                st.metric("NPU Temperature", f"{temperature:.2f} °C")
            with col_m2:
                st.metric("Watchdog Resets", f"{watchdog_count}")
            
            current_time = int(time.time())
            if last_heartbeat > 0 and (current_time - last_heartbeat) > 5:
                st.warning("Heartbeat timeout. Triggering Watchdog Recovery...")
                perform_watchdog_recovery()
                st.rerun()

            npu_mem_raw = np.frombuffer(raw_state[HEADER_SIZE:HEADER_SIZE+NPU_MEM_BYTES], dtype=np.float32)
            
            offset_c = dim * dim * 2
            mat_C = np.copy(npu_mem_raw[offset_c : offset_c + (dim * dim)]).reshape(dim, dim)
            
            expected_C = np.dot(st.session_state.mat_A, st.session_state.mat_B)
            
            c3, c4 = st.columns(2)
            with c3:
                st.write("vNPU Result")
                st.dataframe(mat_C, height=200)
            with c4:
                st.write("CPU Result")
                st.dataframe(expected_C, height=200)
                
            mse = ((mat_C - expected_C)**2).mean()
            st.metric("MSE", f"{mse:.8f}")
            
    except Exception as e:
        st.error(f"Read error: {e}")