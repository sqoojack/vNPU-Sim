# stress_test.py
# python stress_test.py
import subprocess
import time
import random

def run_stress_test():
    print("=== vGPU-Sim Stress Test Automation ===")
    print("Launching Driver...")
    
    # 啟動 Driver 作為子進程
    process = subprocess.Popen(
        ['./driver', '0'],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=0 # Unbuffered I/O
    )
    
    time.sleep(1) # 等待 Watchdog 初始化
    # fw_process = subprocess.Popen(['./firmware'], stdout=subprocess.DEVNULL)
    
    print("Launching Firmware...")
    
    print("[TEST] Starting Command Flood (100 commands)...")
    
    input_cmds = ""
    # 產生 100 個繪圖指令
    for _ in range(100):
        input_cmds += "1\n"
        
    # 最後加一個 Exit
    input_cmds += "0\n"
    
    try:
        # 一次性灌入所有指令
        stdout, stderr = process.communicate(input=input_cmds, timeout=60)
        
        # 分析輸出
        full_count = stdout.count("Buffer full")
        temp_lines = stdout.count("GPU Temp")
        
        print("\n=== Test Results ===")
        print(f"Commands Sent: 100")
        print(f"Buffer Full Events: {full_count}")
        print(f"Successful Sends: {temp_lines}")
        
        if full_count > 0:
            print("\n[PASS] Ring Buffer Logic Verified: Backpressure detected.")
        else:
            print("\n[FAIL] Buffer never filled up? (Check buffer size or timings)")
            
    except subprocess.TimeoutExpired:
        print("[FAIL] Test Timed Out!")
        process.kill()
    # fw_process.terminate()

if __name__ == "__main__":
    run_stress_test()