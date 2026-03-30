import subprocess
import time

def run_stress_test():
    print("=== vGPU-Sim Stress Test Automation ===")
    
    print("Launching Firmware...")
    fw_process = subprocess.Popen(['./build/firmware'], stdout=subprocess.DEVNULL)
    time.sleep(2)
    
    print("Launching Driver...")
    process = subprocess.Popen(
        ['./build/driver', '0'],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=0
    )
    
    print("[TEST] Starting Command Flood (1000 commands)...")
    
    # 修復：將指令提高至 1000 個，強制超越 Ring Buffer 長度 (256) 徹底激發並驗證滿載行為
    input_cmds = "1\n" * 1000 + "0\n"
    
    try:
        stdout, stderr = process.communicate(input=input_cmds, timeout=30)
        
        full_count = stdout.count("Buffer full")
        success_count = stdout.count("Success")
        
        print("\n=== Test Results ===")
        print(f"Commands Sent: 1000")
        print(f"Buffer Full Events: {full_count}")
        print(f"Successful Sends: {success_count}")
        
        if success_count >= 1000:
            print("\n[PASS] All commands processed successfully.")
        if full_count > 0:
            print("[PASS] Ring Buffer Logic Verified: Backpressure detected.")
        else:
            print("[WARN] Ring Buffer never filled up.")
            
    except subprocess.TimeoutExpired:
        print("[FAIL] Test Timed Out! Driver is likely blocked by Firmware.")
        process.kill()
    finally:
        fw_process.terminate()

if __name__ == "__main__":
    run_stress_test()