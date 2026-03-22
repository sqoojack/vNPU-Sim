import socket
import struct
import numpy as np

SERVER_IP = '127.0.0.1'
SERVER_PORT = 8080
# 經過修改後的 Header (加入 _padding)，精確為 32 bytes
HEADER_SIZE = 32
NPU_MEM_BYTES = 640 * 480 * 4

def send_weights(offset, matrix):
    data_bytes = matrix.astype(np.float32).tobytes()
    size_in_bytes = len(data_bytes)
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((SERVER_IP, SERVER_PORT))
        s.sendall(struct.pack('<B I I', 1, offset, size_in_bytes))
        s.sendall(data_bytes)
    print(f"Sent {size_in_bytes} bytes to offset {offset}")

def read_npu_state():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((SERVER_IP, SERVER_PORT))
        s.sendall(struct.pack('<B', 0))
        
        # 確保接收到足夠的資料量以供 numpy 解析
        expected_size = HEADER_SIZE + NPU_MEM_BYTES
        data = bytearray()
        while len(data) < expected_size:
            packet = s.recv(8192)
            if not packet:
                break
            data.extend(packet)
            
        return bytes(data)

if __name__ == "__main__":
    dim = 4
    mat_A = np.random.rand(dim, dim).astype(np.float32)
    mat_B = np.random.rand(dim, dim).astype(np.float32)
    
    send_weights(0, mat_A)
    send_weights(16 * 4, mat_B)
    
    input("Press Enter after firmware completes calculation to read results...")
    state_data = read_npu_state()
    
    npu_mem_bytes = state_data[HEADER_SIZE : HEADER_SIZE + NPU_MEM_BYTES] 
    mat_C = np.frombuffer(npu_mem_bytes, dtype=np.float32)
    
    print("Result Matrix C:\n", mat_C[32 : 32 + 16].reshape(4,4))
    print("Expected:\n", np.dot(mat_A, mat_B))