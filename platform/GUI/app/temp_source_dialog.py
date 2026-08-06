"""温度来源选择对话框：串口 / 网络定位 / 手动设定。"""
import threading

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QButtonGroup, QDialog, QDialogButtonBox, QDoubleSpinBox, QHBoxLayout,
    QLabel, QLineEdit, QPushButton, QRadioButton, QVBoxLayout, QWidget,
)

from .weather import fetch_ip_city

SOURCES = ("serial", "network", "manual")
SOURCE_NAMES = {"serial": "串口", "network": "网络", "manual": "手动"}


class TempSourceDialog(QDialog):
    locate_done = Signal(str)  # IP 定位结果：城市名，失败为空字符串

    def __init__(self, current_source="serial", city="", manual_temp=25.0,
                 parent=None):
        super().__init__(parent)
        self.setWindowTitle("温度来源")
        self.setMinimumWidth(360)

        self.locate_done.connect(self._on_locate_done)

        # ---- 来源单选 ----
        self._serial_rb = QRadioButton("串口（单片机回传的真实温度）")
        self._network_rb = QRadioButton("网络定位（当地气温）")
        self._manual_rb = QRadioButton("手动设定")
        self._group = QButtonGroup(self)
        for rb in (self._serial_rb, self._network_rb, self._manual_rb):
            self._group.addButton(rb)
        if current_source == "network":
            self._network_rb.setChecked(True)
        elif current_source == "manual":
            self._manual_rb.setChecked(True)
        else:
            self._serial_rb.setChecked(True)

        # ---- 网络定位设置区 ----
        net_box = QWidget()
        net_lay = QHBoxLayout(net_box)
        net_lay.setContentsMargins(24, 0, 0, 0)
        net_lay.addWidget(QLabel("城市:"))
        self._city_edit = QLineEdit(city)
        self._city_edit.setPlaceholderText("如:北京")
        net_lay.addWidget(self._city_edit, 1)
        locate_btn = QPushButton("IP 定位")
        locate_btn.setToolTip("按公网 IP 自动定位（走代理时可能不准）")
        locate_btn.clicked.connect(self._on_locate_clicked)
        net_lay.addWidget(locate_btn)
        net_hint = QLabel("无串口温度时使用当地气温，每 10 分钟刷新一次")
        net_hint.setStyleSheet("color: #888; font-size: 11px;")
        net_hint.setContentsMargins(24, 0, 0, 0)

        # ---- 手动设定区 ----
        manual_box = QWidget()
        manual_lay = QHBoxLayout(manual_box)
        manual_lay.setContentsMargins(24, 0, 0, 0)
        manual_lay.addWidget(QLabel("温度值:"))
        self._manual_spin = QDoubleSpinBox()
        self._manual_spin.setRange(-50.0, 150.0)
        self._manual_spin.setDecimals(1)
        self._manual_spin.setSingleStep(0.5)
        self._manual_spin.setValue(manual_temp)
        manual_lay.addWidget(self._manual_spin)
        manual_lay.addWidget(QLabel("°C"))

        # ---- 按钮 ----
        btns = QDialogButtonBox(QDialogButtonBox.Ok
                                | QDialogButtonBox.Cancel)
        btns.accepted.connect(self.accept)
        btns.rejected.connect(self.reject)

        lay = QVBoxLayout(self)
        lay.addWidget(self._serial_rb)
        lay.addWidget(self._network_rb)
        lay.addWidget(net_box)
        lay.addWidget(net_hint)
        lay.addSpacing(4)
        lay.addWidget(self._manual_rb)
        lay.addWidget(manual_box)
        lay.addSpacing(8)
        lay.addWidget(btns)

    # ------------------------------------------------------------------
    def _on_locate_clicked(self):
        self._city_edit.setEnabled(False)
        threading.Thread(target=self._locate_worker, daemon=True).start()

    def _locate_worker(self):
        city, _lat, _lon = fetch_ip_city()
        self.locate_done.emit(city or "")

    def _on_locate_done(self, city):
        self._city_edit.setEnabled(True)
        if city:
            self._city_edit.setText(city)

    # ------------------------------------------------------------------
    def result_values(self):
        """返回 (source, city, manual_temp)。"""
        if self._network_rb.isChecked():
            source = "network"
        elif self._manual_rb.isChecked():
            source = "manual"
        else:
            source = "serial"
        return (source, self._city_edit.text().strip(),
                self._manual_spin.value())
