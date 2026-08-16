"""串口调试工具：定位"上位机收不到数据"问题。

纯命令行，只依赖 pyserial，无需 GUI。可在本目录直接运行，
也可复制到上位机 platform/GUI/tools/ 下使用。

用法：
    python serial_debug.py                    # 交互模式：列出端口 → 选择 → 监听
    python serial_debug.py COM5               # 直接监听指定端口（默认 9600）
    python serial_debug.py COM5 --baud 115200 # 指定波特率监听
    python serial_debug.py COM5 --scan        # 自动扫描波特率（核心功能）

功能：
    1. 端口列表：显示 device + 描述（可区分 USB-TTL 与蓝牙 SPP 虚拟串口）
    2. 原始数据监听：HEX + ASCII 双视图，统计 字节数/行数/可解析帧数
    3. 波特率扫描 --scan：依次尝试 4800..128000，每个监听 3 秒，
       统计收到的字节数与可解析 "weight,temp" 帧数，
       一次确定 JDY-31 模块的真实波特率（出厂默认 9600，但可能被改过）
    4. 发送：监听/扫描时输入任意文本回车即发送，
       例如发 AT+VERSION 探测蓝牙模块、发 1 触发设备模式

根据结果判断问题在哪：
    - 所有波特率都收不到任何字节 → 链路问题：蓝牙未配对/未连接、
      模块 EN 引脚被拉低（命令模式不透传）、TX/RX 接反、未共地、
      固件未运行（OLED 无显示）或已自动睡眠（60s 无按键）
    - 某波特率有字节但无有效帧 → 波特率或协议问题（看 HEX 判断是否乱码）
    - 某波特率有完整帧（如 "123.45,25.6"）→ 主界面把波特率改成该值即可
"""
import argparse
import sys
import threading
import time

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    print("缺少 pyserial：pip install pyserial", file=sys.stderr)
    sys.exit(1)

BAUDS = [4800, 9600, 19200, 38400, 57600, 115200, 128000]
DEFAULT_BAUD = 9600


def list_port_names():
    return [(p.device, p.description or "") for p in list_ports.comports()]


def hex_dump(data: bytes) -> str:
    """单行 HEX+ASCII 视图，适合 9600 波特率下的短行数据。"""
    hexs = " ".join(f"{b:02X}" for b in data)
    ascii_ = "".join(chr(b) if 32 <= b < 127 else "." for b in data)
    return f"{hexs:<72}  |{ascii_}|"


def count_valid_frames(data: bytes) -> int:
    """统计可解析的 "weight,temp\\n" 帧数（float 解析成功即算）。"""
    n = 0
    for line in data.split(b"\n"):
        line = line.strip()
        if not line:
            continue
        try:
            float(line.split(b",")[0])
            n += 1
        except ValueError:
            pass
    return n


def scan_baud(port: str, duration: float = 3.0):
    """依次尝试常见波特率，打印每个波特率收到的字节数与有效帧数。"""
    print(f"\n===== 波特率扫描：{port}，每个波特率监听 {duration:.0f}s =====")
    print(f"{'波特率':>8}  {'字节数':>8}  {'有效帧':>6}  样例")
    found = None
    for baud in BAUDS:
        try:
            ser = serial.Serial(port, baud, timeout=0)
        except Exception as exc:
            print(f"波特率 {baud:>7}: 打开失败 {exc}")
            continue
        buf = b""
        t0 = time.time()
        sample = b""
        while time.time() - t0 < duration:
            n = ser.in_waiting
            if n:
                chunk = ser.read(n)
                buf += chunk
                if not sample:
                    sample = chunk
            time.sleep(0.02)
        ser.close()
        frames = count_valid_frames(buf)
        mark = ""
        if len(buf) > 0:
            mark = "  <<< 有数据"
        if frames > 0:
            mark = "  <<< 有效帧! 主界面用此波特率"
            found = baud
        print(f"{baud:>8}  {len(buf):>8}  {frames:>6}  "
              f"{hex_dump(sample[:16])}{mark}")
    if found:
        print(f"\n结论：波特率 {found} 能收到有效数据帧，"
              f"上位机/固件请统一使用该波特率。")
    else:
        print("\n结论：所有波特率均未收到有效帧。")
        print("  若连字节都没有 → 链路问题（见文件头说明），"
              "不波特率的事。")
        print("  若只有字节没有帧 → 协议不符或纯乱码，"
              "用 --baud 监听并观察 HEX 内容。")


def _read_line(remain: float):
    """非阻塞读一行输入（仅 Windows，msvcrt 轮询）。超时返回 None。"""
    import msvcrt
    buf = ""
    deadline = time.time() + remain
    while True:
        if msvcrt.kbhit():
            ch = msvcrt.getwch()
            if ch in ("\r", "\n"):
                return buf
            if ch in ("\b", "\x7f"):
                buf = buf[:-1]
            else:
                buf += ch
        elif time.time() >= deadline:
            return None
        else:
            time.sleep(0.02)


def listen(port: str, baud: int, timeout: float = None):
    """监听模式：后台读线程 + 前台输入发送。输入内容发送、q 退出。
    timeout 不为 None 时，监听满该秒数自动退出并打印统计
    （无头/自动化场景，避免界面静默挂起）。"""
    try:
        ser = serial.Serial(port, baud, timeout=0)
    except Exception as exc:
        print(f"打开 {port} 失败: {exc}", file=sys.stderr)
        print("  若报错含 \"管道的另一端上无任何进程\"(233) / "
              "\"不能访问网络位置\"(1231)：", file=sys.stderr)
        print("    蓝牙 RFCOMM 链路未建立或已断开（模块未上电、EN 脚被拉低、"
              "被手机等设备占用、供电不足 3.3V<3.6V）。", file=sys.stderr)
        print("  若报错为 \"拒绝访问\"(13)：端口被其他程序占用"
              "（上位机/串口助手需先断开）。", file=sys.stderr)
        sys.exit(1)

    stop = threading.Event()
    total_bytes = 0
    total_lines = 0
    total_frames = 0
    t0 = time.time()
    last_hint = t0 + 3.0   # 3 秒后第一次无数据提示（给数据一点到达时间）

    def reader():
        nonlocal total_bytes, total_lines, total_frames, last_hint
        buf = b""
        while not stop.is_set():
            n = ser.in_waiting
            if n:
                data = ser.read(n)
                total_bytes += len(data)
                buf += data
                print(f"[收 {len(data):3d}B 累计{total_bytes:5d}B] "
                      f"{hex_dump(data)}")
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    total_lines += 1
                    try:
                        float(line.split(b",")[0])
                        total_frames += 1
                    except ValueError:
                        pass
                last_hint = time.time()
            else:
                # 无数据心跳提示：证明程序还活着，链路确实没数据
                if time.time() - last_hint >= 5.0:
                    print(f"[{time.time() - t0:5.1f}s] 仍未收到数据"
                          f"（累计 {total_bytes} 字节）。检查：蓝牙是否已"
                          f"\"连接\"（不只是配对）、固件是否在运行、"
                          f"模块 EN 脚是否被拉低、TX/RX 是否接反。")
                    last_hint = time.time()
                time.sleep(0.02)

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    print(f"\n===== 监听中：{port} @ {baud} =====")
    print("  输入内容回车 = 发送（如 AT+VERSION），q = 退出")
    if timeout:
        print(f"  --timeout {timeout:.0f}s 后自动退出")
    try:
        deadline = t0 + timeout if timeout else None
        while True:
            if deadline is not None:
                remain = deadline - time.time()
                if remain <= 0:
                    break
                if sys.platform == "win32":
                    cmd = _read_line(remain)
                    if cmd is None:
                        break   # 超时自动退出
                else:
                    try:
                        cmd = input(f"> ({remain:.0f}s) ").strip()
                    except (KeyboardInterrupt, EOFError):
                        break
            else:
                try:
                    cmd = input("> ").strip()
                except (KeyboardInterrupt, EOFError):
                    break
            if cmd.lower() in ("q", "quit", "exit"):
                break
            if cmd:
                payload = cmd.encode()
                ser.write(payload)
                print(f"[发 {len(payload)}B] {hex_dump(payload)}")
    finally:
        stop.set()
        ser.close()
        print(f"\n统计：字节 {total_bytes}，行 {total_lines}，"
              f"可解析帧 {total_frames}")


def main():
    ap = argparse.ArgumentParser(description="串口调试工具")
    ap.add_argument("port", nargs="?", help="串口名，如 COM5")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="波特率")
    ap.add_argument("--scan", action="store_true", help="扫描波特率模式")
    ap.add_argument("--duration", type=float, default=3.0, help="扫描时每档秒数")
    ap.add_argument("--timeout", type=float, default=None,
                    help="监听模式自动退出秒数（默认一直监听）")
    args = ap.parse_args()

    if args.port is None:
        ports = list_port_names()
        if not ports:
            print("未发现任何串口。蓝牙请先在 Windows 设置里配对 "
                  "JDY-31（PIN 1234），配对后才会出现 COM 口。",
                  file=sys.stderr)
            sys.exit(1)
        print("可用串口：")
        for i, (dev, desc) in enumerate(ports):
            print(f"  [{i}] {dev}  {desc}")
        try:
            idx = int(input("选择序号: ").strip())
            args.port = ports[idx][0]
        except (ValueError, IndexError):
            print("无效选择", file=sys.stderr)
            sys.exit(1)

    if args.scan:
        scan_baud(args.port, args.duration)
    else:
        listen(args.port, args.baud, args.timeout)


if __name__ == "__main__":
    main()
