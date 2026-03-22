import socket
import struct
import numpy as np

SERVER_IP = '127.0.0.1'
SERVER_PORT = 8080

def send_weights(offset, matrix):
    """將神經網路權重透過 TCP 寫入 Firmware 的 NPU 記憶體"""
    data_bytes = matrix.astype(np.float32).tobytes()
    size_in_bytes = len(data_bytes)
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((SERVER_IP, SERVER_PORT))
        # 傳送 Mode 1 (Write), offset, size, data
        s.sendall(struct.pack('<B I I', 1, offset, size_in_bytes))
        s.sendall(data_bytes)
    print(f"Sent {size_in_bytes} bytes to offset {offset}")

def read_npu_state():
    """透過 TCP 取得 Firmware 完整的 NPU 狀態與記憶體"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((SERVER_IP, SERVER_PORT))
        # 傳送 Mode 0 (Read)
        s.sendall(struct.pack('<B', 0))
        
        # 接收完整結構體資料 (需對應 vnpu_shared_state 的大小)
        data = b''
        while True:
            packet = s.recv(4096)
            if not packet: break
            data += packet
            
        return data

# --- Demo 流程 ---
if __name__ == "__main__":
    dim = 4
    # 產生兩個 4x4 的測試矩陣
    mat_A = np.random.rand(dim, dim).astype(np.float32)
    mat_B = np.random.rand(dim, dim).astype(np.float32)
    
    print("Matrix A:\n", mat_A)
    print("Matrix B:\n", mat_B)
    
    # 寫入 NPU 記憶體 (假設 A 放 offset 0，B 放 offset 16)
    send_weights(0, mat_A)
    send_weights(16 * 4, mat_B) # 16 個 float * 4 bytes
    
    print("Weights sent. Please use C++ Driver Client to trigger CMD_MATRIX_MULTIPLY (A=0, B=16, C=32, N=4)")
    input("Press Enter after firmware completes calculation to read results...")
    
    state_data = read_npu_state()
    
    # 解析 NPU_MEM 中的結果 (Offset 32)
    # Header size (magic, running, frame, temp, heartbeat, watchdog) = 24 bytes
    header_size = 28 # 對齊考量，視編譯器而定，可用 struct.calcsize 確認
    
    # 直接從記憶體區塊切出結果矩陣 C
    npu_mem_bytes = state_data[24 : 24 + (640*480*4)] 
    mat_C = np.frombuffer(npu_mem_bytes, dtype=np.float32)
    
    print("Result Matrix C (from Firmware):\n", mat_C[32 : 32 + 16].reshape(4,4))
    print("Numpy Expected Result:\n", np.dot(mat_A, mat_B))