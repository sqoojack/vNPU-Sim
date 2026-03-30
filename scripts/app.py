import socket
import struct
import numpy as np
import streamlit as st
import fcntl
import os

# 系統常量定義 (與 vnpu_common.h 嚴格對齊)
SERVER_IP = '127.0.0.1'
SERVER_PORT = 8080
HEADER_SIZE = 32
NPU_MEM_SIZE = 640 * 480
NPU_MEM_BYTES = NPU_MEM_SIZE * 4
# vnpu_shared_state 總大小約 1,234,984 bytes (含 Ring Buffer)
TOTAL_STATE_SIZE = HEADER_SIZE + NPU_MEM_BYTES + 8 + (256 * 24) 

VNPU_MAGIC = ord('V')
CMD_MATRIX_MULTIPLY = 5
VNPU_IOCTL_SUBMIT_CMD = (1 << 30) | (24 << 16) | (VNPU_MAGIC << 8) | 1

def check_firmware_alive():
    """檢查 Firmware Port 是否開啟"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(1.0)
        return s.connect_ex((SERVER_IP, SERVER_PORT)) == 0

def send_weights(offset, matrix):
    data_bytes = matrix.astype(np.float32).tobytes()
    size_in_bytes = len(data_bytes)
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(5.0)
        s.connect((SERVER_IP, SERVER_PORT))
        # Mode 1: Write Memory
        s.sendall(struct.pack('<B I I', 1, offset, size_in_bytes))
        s.sendall(data_bytes)

def read_npu_state():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(10.0) # 增加 Timeout 以應對大資料傳輸
        s.connect((SERVER_IP, SERVER_PORT))
        # Mode 0: Read All State
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
    
    # 參數對齊 vnpu_command: type, params[5]
    cmd_bytes = struct.pack('<I 5I', CMD_MATRIX_MULTIPLY, offA, offB, offC, dim, 0)
    
    if not os.path.exists("/dev/vnpu0"):
        return "Error: /dev/vnpu0 not found. Module loaded?"

    try:
        with open("/dev/vnpu0", "wb") as f:
            fcntl.ioctl(f.fileno(), VNPU_IOCTL_SUBMIT_CMD, cmd_bytes)
        return f"Success: Submit CMD_MATRIX_MULTIPLY (dim={dim})"
    except Exception as e:
        return f"IOCTL Error: {e}"

# --- Streamlit UI ---
st.set_page_config(page_title="vNPU Simulator", layout="wide")
st.title("vNPU-Sim System Monitoring Dashboard")

# 狀態檢查
is_alive = check_firmware_alive()
if not is_alive:
    st.error("❌ Firmware is NOT responding on port 8080. Please check firmware.log")
else:
    st.success("✅ Firmware System Online")

# 維度設定 (限制 256 以防越界 NPU 記憶體)
dim = st.sidebar.number_input("Matrix Dimension", min_value=2, max_value=256, value=64, step=1)

# 強制重新生成矩陣 (當維度改變時)
if "current_dim" not in st.session_state or st.session_state.current_dim != dim:
    st.session_state.current_dim = dim
    st.session_state.mat_A = np.random.rand(dim, dim).astype(np.float32)
    st.session_state.mat_B = np.random.rand(dim, dim).astype(np.float32)
    st.info(f"Dimension changed to {dim}x{dim}, matrices regenerated.")

st.subheader("1. Data Orchestration")
col1, col2 = st.columns(2)
with col1:
    st.write("Matrix A")
    st.dataframe(st.session_state.mat_A, height=200)
with col2:
    st.write("Matrix B")
    st.dataframe(st.session_state.mat_B, height=200)

if st.button("🚀 Push Weights to vNPU"):
    try:
        # 傳送 A (offset 0) 與 B (offset A 之後)
        send_weights(0, st.session_state.mat_A)
        send_weights(dim * dim * 4, st.session_state.mat_B)
        st.success("Memory updated.")
    except Exception as e:
        st.error(f"Failed to push weights: {e}")

st.subheader("2. Hardware Control")
if st.button("🔥 Execute GEMM Command"):
    result = trigger_hardware(dim)
    st.code(result)

st.subheader("3. Verification")
if st.button("📊 Pull & Verify Results"):
    try:
        raw_state = read_npu_state()
        
        if len(raw_state) < HEADER_SIZE + (dim * dim * 3 * 4):
            st.error(f"Received truncated data ({len(raw_state)} bytes). Firmware might have crashed.")
        else:
            # 解析 NPU 記憶體
            npu_mem_raw = np.frombuffer(raw_state[HEADER_SIZE:HEADER_SIZE+NPU_MEM_BYTES], dtype=np.float32)
            
            # C 的位置在 A 和 B 之後
            offset_c = dim * dim * 2
            mat_C = np.copy(npu_mem_raw[offset_c : offset_c + (dim * dim)]).reshape(dim, dim)
            
            # 本地計算對比
            expected_C = np.dot(st.session_state.mat_A, st.session_state.mat_B)
            
            c3, c4 = st.columns(2)
            with c3:
                st.write("vNPU Hardware Result")
                st.dataframe(mat_C, height=200)
            with c4:
                st.write("CPU Expected Result")
                st.dataframe(expected_C, height=200)
                
            mse = ((mat_C - expected_C)**2).mean()
            st.metric("Mean Squared Error", f"{mse:.8f}")
            
    except Exception as e:
        st.error(f"Error reading results: {e}")