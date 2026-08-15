"""串口读取模块：扫描端口、连接、后台轮询读取并按行解析数据帧。

数据帧协议（与单片机约定）：
    每行一条记录，逗号分隔，以换行符结尾，例如：
        123.45,25.6     # 重量(g), 温度(°C)
        123.45          # 仅重量
    解析失败的行（如串口刚上电的乱码）会被静默跳过。
"""
import time

from PySide6.QtCore import QObject, QTimer, Signal

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:  # 便于在未安装 pyserial 时仍能导入做 UI 调试
    serial = None
    list_ports = None


class SerialWorker(QObject):
    """在 GUI 线程内用 QTimer 轮询串口（数据量不大，足够实时）。"""

    data_ready = Signal(float, float, float)  # (ts, weight, temp)，temp 可能为 nan
    connected = Signal(bool, str)             # (是否已连接, 描述信息)
    error = Signal(str)                       # 出错信息

    def __init__(self, parent=None):
        super().__init__(parent)
        self._serial = None
        self._buffer = b""
        self._timer = QTimer(self)
        self._timer.setInterval(15)  # ~66Hz 轮询
        self._timer.timeout.connect(self._poll)

    # ------------------------------------------------------------------
    # 端口扫描
    # ------------------------------------------------------------------
    def list_ports(self):
        if list_ports is None:
            return []
        try:
            return [p.device for p in list_ports.comports()]
        except Exception:
            return []

    # ------------------------------------------------------------------
    # 连接管理
    # ------------------------------------------------------------------
    def is_connected(self):
        return self._serial is not None and self._serial.is_open

    def connect(self, port: str, baudrate: int):
        if serial is None:
            self.error.emit("未安装 pyserial 库")
            return
        try:
            self._serial = serial.Serial(port, baudrate, timeout=0)
        except Exception as exc:
            self.error.emit(f"打开 {port} 失败: {exc}")
            self.connected.emit(False, "")
            return
        self._buffer = b""
        self._timer.start()
        self.connected.emit(True, f"{port} @ {baudrate}")

    def disconnect(self):
        self._timer.stop()
        if self._serial is not None:
            try:
                self._serial.close()
            except Exception:
                pass
            self._serial = None
        self.connected.emit(False, "")

    # ------------------------------------------------------------------
    # 数据读取
    # ------------------------------------------------------------------
    def _poll(self):
        if not self.is_connected():
            return
        try:
            n = self._serial.in_waiting
            data = self._serial.read(n) if n else b""
        except Exception as exc:
            self.error.emit(f"串口读取失败: {exc}")
            self.disconnect()
            return

        self._buffer += data
        while b"\n" in self._buffer:
            line, self._buffer = self._buffer.split(b"\n", 1)
            self._parse_line(line)

        # 兜底：长时间无换行符（协议不符或乱码）时丢弃，防止内存膨胀
        if len(self._buffer) > 8192:
            self._buffer = b""

    def _parse_line(self, raw: bytes):
        text = raw.decode("utf-8", errors="replace").strip()
        if not text:
            return
        parts = text.split(",")
        try:
            weight = float(parts[0].strip())
            temp = float(parts[1].strip()) if len(parts) > 1 else float("nan")
        except (ValueError, IndexError):
            return  # 跳过坏行（乱码、半行、空字段等，不能让它崩掉解析循环）
        self.data_ready.emit(time.time(), weight, temp)
