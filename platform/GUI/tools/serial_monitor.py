"""最简串口监视器：实时打印串口收到的原始数据（无界面、无解析）。

用法:
    python serial_monitor.py COM4 9600

按 Ctrl+C 退出。用途：验证板子->蓝牙->电脑 链路是否真的通了，
以及测出 JDY-31 模块的真实波特率。
"""
import sys
import time

import serial


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
    print(f"[监听] {port} @ {baud}  按 Ctrl+C 退出")
    ser = serial.Serial(port, baud, timeout=0.1)
    try:
        while True:
            n = ser.in_waiting
            if n:
                data = ser.read(n)
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()
            time.sleep(0.05)
    except KeyboardInterrupt:
        print("\n[退出]")
    finally:
        ser.close()


if __name__ == "__main__":
    main()