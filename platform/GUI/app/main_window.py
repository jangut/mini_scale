"""主窗口：top_bar / graph / datas+export / state 四区布局。"""
import csv
import math
import random
import sys
import threading
import time
from collections import deque
from datetime import datetime
from pathlib import Path

import pyqtgraph as pg
from PySide6.QtCore import QSettings, Qt, QTimer, Signal
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QDoubleSpinBox, QFileDialog, QGridLayout,
    QHBoxLayout, QHeaderView, QLabel, QMainWindow, QPushButton, QSplitter,
    QStatusBar, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
)

from .recorder import Recorder
from .serial_worker import SerialWorker
from .weather import fetch_city_temp

WEIGHT_UNIT = "g"
TEMP_UNIT = "°C"
MAX_PLOT_POINTS = 3000      # 折线图保留的最大点数（滚动窗口）
SIM_INTERVAL_MS = 10        # 模拟数据帧间隔


def _app_root() -> Path:
    """源码运行返回项目根；PyInstaller 打包后返回资源解包目录（_internal）。"""
    if getattr(sys, "frozen", False):
        return Path(sys._MEIPASS)
    return Path(__file__).resolve().parent.parent


APP_ICON_PATH = _app_root() / "LOGO.png"

# 状态栏里连接状态点的颜色
COLOR_OK = "#2e9e44"
COLOR_ERR = "#d64545"
COLOR_IDLE = "#8a8a8a"


class MainWindow(QMainWindow):
    # 子线程回传结果（跨线程信号自动排队到 GUI 线程）
    weather_done = Signal(float, str)   # (temp_c, 城市名)；失败时 temp 为 None

    def __init__(self):
        super().__init__()
        self.setWindowTitle("称重传感器上位机")
        self.setWindowIcon(QIcon(str(APP_ICON_PATH)))
        self.resize(1280, 700)

        # ---- 数据状态 ----
        self._mono_start = time.monotonic()  # 时间轴基准（单调时钟，不回拨）
        self._plot_t = deque(maxlen=MAX_PLOT_POINTS)
        self._plot_w = deque(maxlen=MAX_PLOT_POINTS)
        self._records = []          # (ts, iso, weight, temp)
        self._last_weight = None
        self._last_temp = None
        self._rate_last = None
        self._rate_count = 0
        self._rate_hz = 0.0
        self._last_status_at = 0.0

        # ---- 核心对象 ----
        self._worker = SerialWorker(self)
        self._worker.data_ready.connect(self._on_data)
        self._worker.connected.connect(self._on_connection)
        self._worker.error.connect(self._on_error)

        self._recorder = Recorder(threshold=0.5, stable_window=2.0)

        # ---- 温度来源（串口 / 网络定位 / 手动设定） ----
        self._settings = QSettings("MiniScale", "WeighScale")
        self._temp_source = str(
            self._settings.value("temp_source", "serial") or "serial")
        self._city = str(self._settings.value("city", "") or "").strip()
        self._manual_temp = float(
            self._settings.value("manual_temp", 25.0) or 25.0)
        self._net_temp = None   # 网络气温（网络来源时有效）
        self._net_city = None
        self.weather_done.connect(self._on_weather_done)
        self._weather_timer = QTimer(self)
        self._weather_timer.setInterval(10 * 60 * 1000)  # 每 10 分钟刷新
        self._weather_timer.timeout.connect(self._fetch_weather)

        self._sim_timer = QTimer(self)
        self._sim_timer.setInterval(SIM_INTERVAL_MS)
        self._sim_timer.timeout.connect(self._sim_tick)
        self._sim_state = None

        self._build_ui()
        self._refresh_ports()

        # 默认自动连接第一个可用串口（波特率 9600，与固件一致），
        # 插好 USB-TTL / 蓝牙 COM 后打开上位机即可直接接收数据。
        port = self._port_combo.currentText()
        if port and not port.startswith("（"):
            try:
                baud = int(self._baud_combo.currentText())
            except ValueError:
                baud = 9600
            self._worker.connect(port, baud)

        # 网络来源且已设置城市：启动后异步获取并周期性刷新
        if self._temp_source == "network" and self._city:
            self._weather_timer.start()
            QTimer.singleShot(500, self._fetch_weather)

    # ==================================================================
    # UI 构建
    # ==================================================================
    def _build_ui(self):
        central = QWidget(self)
        root = QVBoxLayout(central)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(6)

        root.addLayout(self._build_top_bar())

        splitter = QSplitter(Qt.Horizontal, central)
        splitter.addWidget(self._build_graph_panel())
        splitter.addWidget(self._build_datas_panel())
        splitter.setStretchFactor(0, 1)
        splitter.setStretchFactor(1, 0)
        splitter.setSizes([760, 320])
        root.addWidget(splitter, 1)

        self.setCentralWidget(central)
        self._build_status_bar()

    def _build_top_bar(self):
        bar = QHBoxLayout()
        bar.setSpacing(8)

        # 串口选择
        bar.addWidget(QLabel("串口:"))
        self._port_combo = QComboBox()
        self._port_combo.setMinimumWidth(110)
        bar.addWidget(self._port_combo)

        refresh_btn = QPushButton("刷新")
        refresh_btn.setToolTip("重新扫描可用串口")
        refresh_btn.clicked.connect(self._refresh_ports)
        bar.addWidget(refresh_btn)

        bar.addWidget(QLabel("波特率:"))
        self._baud_combo = QComboBox()
        self._baud_combo.setEditable(True)
        for b in ("9600", "115200", "57600", "38400", "19200", "4800"):
            self._baud_combo.addItem(b)
        self._baud_combo.setCurrentText("9600")
        self._baud_combo.setMinimumWidth(90)
        bar.addWidget(self._baud_combo)

        self._connect_btn = QPushButton("连接")
        self._connect_btn.clicked.connect(self._toggle_connect)
        bar.addWidget(self._connect_btn)

        self._sim_cb = QCheckBox("模拟数据")
        self._sim_cb.setToolTip("无硬件时模拟称重过程，便于调试")
        self._sim_cb.toggled.connect(self._on_sim_toggled)
        bar.addWidget(self._sim_cb)

        # 温度来源设置（弹出框：串口 / 网络定位 / 手动设定）
        bar.addSpacing(10)
        self._temp_btn = QPushButton()
        self._temp_btn.setToolTip("选择温度来源（串口 / 网络定位 / 手动设定）")
        self._temp_btn.clicked.connect(self._on_temp_source_clicked)
        bar.addWidget(self._temp_btn)
        self._update_temp_button()

        # 记录参数
        bar.addSpacing(12)
        bar.addWidget(QLabel("阈值(g):"))
        self._threshold_spin = QDoubleSpinBox()
        self._threshold_spin.setRange(0.01, 100.0)
        self._threshold_spin.setDecimals(2)
        self._threshold_spin.setSingleStep(0.1)
        self._threshold_spin.setValue(0.5)
        self._threshold_spin.setToolTip(
            "重量相对上次记录值变化超过该值时才可能触发一次新记录，兼作噪声抑制")
        self._threshold_spin.valueChanged.connect(self._on_params_changed)
        bar.addWidget(self._threshold_spin)

        bar.addWidget(QLabel("稳定窗口(s):"))
        self._stable_spin = QDoubleSpinBox()
        self._stable_spin.setRange(0.1, 30.0)
        self._stable_spin.setDecimals(1)
        self._stable_spin.setSingleStep(0.1)
        self._stable_spin.setValue(2.0)
        self._stable_spin.setToolTip(
            "触发后读数需连续稳定该时长才确认记录（防抖）")
        self._stable_spin.valueChanged.connect(self._on_params_changed)
        bar.addWidget(self._stable_spin)

        bar.addStretch(1)

        self._conn_label = QLabel("未连接")
        self._conn_label.setStyleSheet(f"color: {COLOR_IDLE};")
        bar.addWidget(self._conn_label)
        return bar

    def _build_graph_panel(self):
        panel = QWidget()
        lay = QVBoxLayout(panel)
        lay.setContentsMargins(0, 0, 0, 0)

        # 绘图区 + 悬浮提示文字（用叠加布局，提示不进入坐标轴计算，
        # 否则它的文本边界会把时间轴撑出负刻度、使 0 居中）
        holder = QWidget()
        holder_lay = QGridLayout(holder)
        holder_lay.setContentsMargins(0, 0, 0, 0)
        holder_lay.setSpacing(0)

        self._plot = pg.PlotWidget()
        self._plot.showGrid(x=True, y=True, alpha=0.25)
        self._plot.setLabel("left", f"重量 ({WEIGHT_UNIT})")
        self._plot.setLabel("bottom", "时间 (s)")
        self._plot.addLegend(offset=(10, 10))
        vb = self._plot.getViewBox()
        # 关闭 autoRange 默认 padding，并硬性限制时间轴下限为 0：
        # 无论自动缩放、初始状态还是手动拖拽，x 轴都不会出现负刻度
        vb.setDefaultPadding(0)
        vb.setLimits(xMin=0)

        self._curve = self._plot.plot(
            pen=pg.mkPen("#2f7bd6", width=2), name="重量")
        self._marks = pg.ScatterPlotItem(
            size=10, brush=pg.mkBrush(230, 60, 60),
            pen=pg.mkPen("#8f1d1d", width=1), name="记录点")
        self._plot.addItem(self._marks)

        self._plot_hint = QLabel("等待数据…", holder)
        self._plot_hint.setAlignment(Qt.AlignCenter)
        self._plot_hint.setStyleSheet(
            "color: #999999; background: transparent; font-size: 14px;")
        holder_lay.addWidget(self._plot, 0, 0)
        holder_lay.addWidget(self._plot_hint, 0, 0)

        lay.addWidget(holder, 1)
        return panel

    def _build_datas_panel(self):
        panel = QWidget()
        lay = QVBoxLayout(panel)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(6)

        head = QHBoxLayout()
        head.addWidget(QLabel("数据记录"))
        self._count_label = QLabel("0 条")
        self._count_label.setStyleSheet("color: #666;")
        head.addWidget(self._count_label)
        head.addStretch(1)
        lay.addLayout(head)

        self._table = QTableWidget(0, 3)
        self._table.setHorizontalHeaderLabels(
            ["时间", f"重量({WEIGHT_UNIT})", f"温度({TEMP_UNIT})"])
        self._table.horizontalHeader().setSectionResizeMode(
            QHeaderView.Stretch)
        self._table.setEditTriggers(QTableWidget.NoEditTriggers)
        self._table.setSelectionBehavior(QTableWidget.SelectRows)
        self._table.verticalHeader().setVisible(False)
        lay.addWidget(self._table, 1)

        export_btn = QPushButton("导出 CSV")
        export_btn.clicked.connect(self._on_export_clicked)
        lay.addWidget(export_btn)
        return panel

    def _build_status_bar(self):
        sb = QStatusBar(self)
        self.setStatusBar(sb)
        self._status_label = QLabel("就绪")
        sb.addPermanentWidget(self._status_label)

    # ==================================================================
    # 串口控制
    # ==================================================================
    def _refresh_ports(self):
        ports = self._worker.list_ports()
        current = self._port_combo.currentText()
        self._port_combo.clear()
        self._port_combo.addItems(ports if ports else ["（无串口）"])
        if current in ports:
            self._port_combo.setCurrentText(current)

    def _toggle_connect(self):
        if self._worker.is_connected():
            self._worker.disconnect()
            return
        if self._sim_cb.isChecked():
            self.statusBar().showMessage("模拟模式下无需连接串口", 3000)
            return
        port = self._port_combo.currentText()
        if not port or port.startswith("（"):
            self.statusBar().showMessage("请先选择串口", 3000)
            return
        try:
            baud = int(self._baud_combo.currentText())
        except ValueError:
            self.statusBar().showMessage("波特率必须是整数", 3000)
            return
        self._worker.connect(port, baud)

    def _on_connection(self, ok: bool, info: str):
        if ok:
            self._connect_btn.setText("断开")
            self._conn_label.setText(f"● {info}")
            self._conn_label.setStyleSheet(f"color: {COLOR_OK};")
            self.statusBar().showMessage(f"已连接 {info}", 3000)
        else:
            self._connect_btn.setText("连接")
            self._conn_label.setText("未连接")
            self._conn_label.setStyleSheet(f"color: {COLOR_IDLE};")

    def _on_error(self, msg: str):
        self.statusBar().showMessage(f"错误: {msg}", 6000)
        self._conn_label.setText("出错")
        self._conn_label.setStyleSheet(f"color: {COLOR_ERR};")

    # ==================================================================
    # 数据接收（串口与模拟共用）
    # ==================================================================
    def _on_data(self, ts: float, weight: float, temp: float):
        self._last_weight = weight
        self._last_temp = temp

        # 折线图（滚动窗口）；时间轴用单调时钟，保证只增不减
        mono = time.monotonic()
        rel = mono - self._mono_start
        self._plot_t.append(rel)
        self._plot_w.append(weight)
        self._curve.setData(list(self._plot_t), list(self._plot_w))
        if self._plot_hint is not None:
            self._plot_hint.hide()
            self._plot_hint = None

        # 记录判定（稳定窗口也基于单调时钟，不受墙钟回拨影响）
        event = self._recorder.update(weight, mono)
        if event is not None:
            _, rec_value, _ = event
            # 显示/导出时间用当前真实墙钟时刻
            iso = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
            # 记录温度严格按所选来源：
            #   手动 -> 手动设定值；串口 -> 串口温度（有效时）；网络 -> 不记录
            if self._temp_source == "manual":
                temp_val = self._manual_temp
            elif (self._temp_source == "serial"
                  and temp is not None and not math.isnan(temp)):
                temp_val = temp
            else:
                temp_val = None
            self._records.append((iso, rec_value, temp_val))
            self._append_record_row(iso, rec_value, temp_val)
            self._marks.addPoints([{"pos": (rel, rec_value)}])

        # 速率统计 + 状态栏刷新（节流）
        self._rate_count += 1
        if self._rate_last is None:
            self._rate_last = ts
        elif ts - self._rate_last >= 1.0:
            self._rate_hz = self._rate_count / (ts - self._rate_last)
            self._rate_count = 0
            self._rate_last = ts
        if ts - self._last_status_at >= 0.2:
            self._last_status_at = ts
            self._update_status()

    def _append_record_row(self, iso: str, weight: float, temp):
        row = self._table.rowCount()
        self._table.insertRow(row)
        time_str = iso.split(" ")[1]
        items = [QTableWidgetItem(time_str),
                 QTableWidgetItem(f"{weight:.3f}"),
                 QTableWidgetItem("--" if temp is None else f"{temp:.3f}")]
        for col, it in enumerate(items):
            it.setTextAlignment(Qt.AlignCenter)
            self._table.setItem(row, col, it)
        self._table.scrollToBottom()
        self._count_label.setText(f"{self._table.rowCount()} 条")

    def _update_status(self):
        w = self._last_weight
        t = self._last_temp
        w_str = "--" if w is None else f"{w:.3f} {WEIGHT_UNIT}"
        # 温度按所选来源显示
        if self._temp_source == "serial":
            t_str = ("--" if (t is None or math.isnan(t))
                     else f"{t:.2f} {TEMP_UNIT} (串口)")
        elif self._temp_source == "network":
            if self._net_temp is not None:
                city = self._net_city or self._city or ""
                t_str = f"{self._net_temp:.1f} {TEMP_UNIT} (网络·{city})"
            else:
                t_str = "--"
        else:  # manual
            t_str = f"{self._manual_temp:.1f} {TEMP_UNIT} (手动)"
        self._status_label.setText(
            f"重量: {w_str}  |  温度: {t_str}  |  速率: {self._rate_hz:.0f} 行/s"
            f"  |  记录: {len(self._records)} 条")

    # ==================================================================
    # 记录参数
    # ==================================================================
    def _on_params_changed(self):
        self._recorder.threshold = self._threshold_spin.value()
        self._recorder.stable_window = self._stable_spin.value()

    # ==================================================================
    # 温度来源（串口 / 网络定位 / 手动设定）
    # ==================================================================
    def _update_temp_button(self):
        from .temp_source_dialog import SOURCE_NAMES
        label = SOURCE_NAMES.get(self._temp_source, "串口")
        self._temp_btn.setText(f"温度:{label}")

    def _on_temp_source_clicked(self):
        from .temp_source_dialog import TempSourceDialog
        dlg = TempSourceDialog(self._temp_source, self._city,
                               self._manual_temp, self)
        if not dlg.exec():
            return
        source, city, manual_temp = dlg.result_values()
        self._temp_source = source
        self._city = city
        self._manual_temp = manual_temp
        self._settings.setValue("temp_source", source)
        self._settings.setValue("city", city)
        self._settings.setValue("manual_temp", manual_temp)
        if source == "network":
            self._weather_timer.start()
            if city:
                self._fetch_weather()
            else:
                self.statusBar().showMessage(
                    "网络温度需要先设置城市（温度按钮 → 网络定位）", 4000)
        else:
            self._weather_timer.stop()
        self._update_temp_button()
        self._update_status()

    def _fetch_weather(self):
        if not self._city:
            return
        threading.Thread(target=self._weather_worker, args=(self._city,),
                         daemon=True).start()

    def _weather_worker(self, city):
        temp, name = fetch_city_temp(city)
        self.weather_done.emit(temp, name if name else city)

    def _on_weather_done(self, temp, name):
        if temp is None:
            self._net_temp = None
            self.statusBar().showMessage(
                f"获取 {name} 气温失败，稍后自动重试", 5000)
        else:
            self._net_temp = temp
            self._net_city = name
        self._update_status()

    # ==================================================================
    # CSV 导出
    # ==================================================================
    def _on_export_clicked(self):
        if not self._records:
            self.statusBar().showMessage("暂无记录可导出", 3000)
            return
        default = (f"weight_records_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
                   ".csv")
        path, _ = QFileDialog.getSaveFileName(
            self, "导出 CSV", default, "CSV 文件 (*.csv)")
        if not path:
            return
        if self.export_csv_to_path(path):
            self.statusBar().showMessage(f"已导出 {len(self._records)} 条到 "
                                         f"{path}", 5000)

    def export_csv_to_path(self, path: str) -> bool:
        """导出所有记录为 CSV。列：timestamp, weight_g, temp_c。"""
        if not self._records:
            return False
        with open(path, "w", newline="", encoding="utf-8-sig") as f:
            writer = csv.writer(f)
            writer.writerow(["timestamp", "weight_g", "temp_c"])
            for iso, weight, temp in self._records:
                writer.writerow([
                    iso,
                    f"{weight:.3f}",
                    "" if temp is None else f"{temp:.3f}",
                ])
        return True

    # ==================================================================
    # 模拟数据（无硬件调试）
    # ==================================================================
    def _on_sim_toggled(self, checked: bool):
        if checked:
            self._worker.disconnect()
            self._connect_btn.setEnabled(False)
            self._port_combo.setEnabled(False)
            self._baud_combo.setEnabled(False)
            self._sim_state = {
                "t": 0.0, "from": 0.0, "target": 0.0,
                "hop_at": 0.0, "hop_dur": 0.5, "next_hop": 1.5,
            }
            self._sim_timer.start()
            self.statusBar().showMessage("模拟模式：模拟称重过程…", 3000)
        else:
            self._sim_timer.stop()
            self._sim_state = None
            self._connect_btn.setEnabled(True)
            self._port_combo.setEnabled(True)
            self._baud_combo.setEnabled(True)

    def _sim_tick(self):
        st = self._sim_state
        if st is None:
            return
        st["t"] += SIM_INTERVAL_MS / 1000.0
        t = st["t"]

        # 周期性阶跃：模拟"放上/取下重物"
        if t >= st["next_hop"]:
            st["from"] = st["target"]
            st["target"] = round(random.uniform(0.0, 50.0), 1)
            st["hop_at"] = t
            st["hop_dur"] = random.uniform(0.3, 0.8)
            st["next_hop"] = t + random.uniform(3.0, 6.0)

        frac = (t - st["hop_at"]) / st["hop_dur"] if st["hop_dur"] else 1.0
        frac = min(1.0, max(0.0, frac))
        base = st["from"] + (st["target"] - st["from"]) * frac
        # 蠕变 + 噪声（幅度远小于默认阈值，不会误触发）
        creep = 0.05 * math.sin(t * 0.7)
        noise = random.gauss(0.0, 0.05)
        weight = base + creep + noise
        temp = 25.5 + 0.4 * math.sin(t / 12.0) + random.gauss(0.0, 0.03)
        self._on_data(time.time(), weight, temp)

    # ==================================================================
    # 生命周期
    # ==================================================================
    def closeEvent(self, event):
        self._sim_timer.stop()
        self._weather_timer.stop()
        self._worker.disconnect()
        super().closeEvent(event)
