# stress_test.py
# python stress_test.py
import subprocess
import time

def run_stress_test():
    print("=== vGPU-Sim Stress Test Automation ===")
    
    # 1. 先啟動 Firmware
    print("Launching Firmware...")
    fw_process = subprocess.Popen(['./firmware'], stdout=subprocess.DEVNULL)
    time.sleep(2) # 給予足夠時間初始化共用記憶體與同步物件
    
    # 2. 再啟動 Driver
    print("Launching Driver...")
    process = subprocess.Popen(
        ['./driver', '0'],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=0
    )
    
    print("[TEST] Starting Command Flood (100 commands)...")
    
    # 產生 100 個繪圖指令 (1) 並在最後離開 (0)
    input_cmds = "1\n" * 100 + "0\n"
    
    try:
        # 執行並等待結果
        stdout, stderr = process.communicate(input=input_cmds, timeout=30)
        
        # 分析 Driver 的輸出
        print(f"output: {stdout}")
        full_count = stdout.count("Buffer full")
        # 修正：搜尋 Driver 輸出的成功字串
        success_count = stdout.count("Success")
        
        print("\n=== Test Results ===")
        print(f"Commands Sent: 100")
        print(f"Buffer Full Events: {full_count}")
        print(f"Successful Sends: {success_count}")
        
        if success_count >= 100:
            print("\n[PASS] All commands processed successfully.")
        if full_count > 0:
            print("[PASS] Ring Buffer Logic Verified: Backpressure detected.")
            
    except subprocess.TimeoutExpired:
        print("[FAIL] Test Timed Out! Driver is likely blocked by Firmware.")
        process.kill()
    finally:
        fw_process.terminate()

if __name__ == "__main__":
    run_stress_test()