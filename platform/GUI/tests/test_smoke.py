"""无头冒烟测试：模拟模式下验证记录、折线图与 CSV 导出。

以 offscreen 平台运行（不弹窗），用事件循环驱动模拟数据几秒钟，
检查：折线图有数据、记录表新增行、CSV 导出格式正确。
"""
import os
import tempfile
import time

os.environ["QT_QPA_PLATFORM"] = "offscreen"  # 必须在导入 PySide6 之前

import unittest

from PySide6.QtCore import QSettings
from PySide6.QtWidgets import QApplication

from app.main_window import MainWindow

APP = None


def get_app():
    global APP
    if APP is None:
        APP = QApplication.instance() or QApplication([])
    return APP


def pump(app, seconds):
    """处理事件循环 seconds 秒（模拟模式下 QTimer 会持续触发）。"""
    deadline = time.time() + seconds
    while time.time() < deadline:
        app.processEvents()
        time.sleep(0.02)


class SmokeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # 清掉本机残留设置，避免测试触发网络请求
        s = QSettings("MiniScale", "WeighScale")
        s.remove("city")
        s.remove("temp_source")
        s.remove("manual_temp")
        cls.app = get_app()
        cls.win = MainWindow()

    @classmethod
    def tearDownClass(cls):
        cls.win.close()

    def test_sim_records_plots_and_exports(self):
        """模拟模式：折线图有数据、记录表新增行、CSV 导出格式正确。"""
        win = self.win
        # 调小参数加快稳定确认
        win._threshold_spin.setValue(1.0)
        win._stable_spin.setValue(0.5)

        # 确定性：手动喂一组阶跃数据（0g 稳定 → 25g 稳定），
        # 保证至少记录 1 条，不依赖模拟的随机目标值。
        t0 = time.time()
        for i in range(30):            # 基线 0g
            win._on_data(t0 + i * 0.05, 0.0, 25.0)
            time.sleep(0.01)
        for i in range(80):            # 阶跃到 25g 并稳定 0.8s
            win._on_data(t0 + 1.5 + i * 0.05, 25.0, 25.0)
            time.sleep(0.01)
        self.assertGreaterEqual(win._table.rowCount(), 1,
                                "确定性阶跃应触发记录")

        # 模拟模式：验证折线图数据量与时间轴非负
        win._sim_cb.setChecked(True)
        pump(self.app, 8.0)
        self.assertGreater(len(win._plot_t), 100, "折线图应有大量数据点")
        self.assertEqual(len(win._plot_t), len(win._plot_w))
        # 时间轴不应出现负刻度（padding 归零 + 单调时钟）
        x_min, _ = win._plot.viewRange()[0]
        self.assertGreaterEqual(x_min, -1e-6,
                                f"时间轴下限不应为负，实际 {x_min:.4f}")
        self.assertEqual(win._table.rowCount(), len(win._records))

        # CSV 导出
        with tempfile.TemporaryDirectory() as td:
            path = os.path.join(td, "out.csv")
            self.assertTrue(win.export_csv_to_path(path))
            with open(path, encoding="utf-8-sig") as f:
                lines = f.read().splitlines()
            self.assertEqual(lines[0], "timestamp,weight_g,temp_c")
            self.assertEqual(len(lines) - 1, len(win._records))
            # 每行 3 列，重量可解析为浮点数
            for line in lines[1:]:
                parts = line.split(",")
                self.assertEqual(len(parts), 3)
                float(parts[1])

    def test_initial_axes_never_negative(self):
        """刚启动（无数据）时时间轴左端应为 0，0 不应居中。"""
        win = self.win
        xr = win._plot.viewRange()[0]
        self.assertGreaterEqual(xr[0], -1e-6,
                                f"启动时时间轴左端不应为负，实际 {xr[0]:.4f}")
        self.assertFalse(xr[0] < 0 < xr[1], "0 不应位于时间轴中间")

    def test_net_temp_source(self):
        """温度来源：网络定位时显示网络气温；切回串口后显示串口温度。"""
        win = self.win
        win._temp_source = "network"
        win._city = "北京"
        win._net_temp = None
        win._update_status()
        self.assertIn("--", win._status_label.text())
        # 模拟网络天气回传
        win._on_weather_done(24.7, "北京")
        txt = win._status_label.text()
        self.assertIn("网络·北京", txt)
        self.assertIn("24.7", txt)
        # 切回串口：显示串口温度（显式刷新状态栏，绕过 0.2s 节流）
        win._temp_source = "serial"
        win._on_data(time.time(), 10.0, 30.5)
        win._update_status()
        self.assertIn("30.50", win._status_label.text())
        self.assertIn("(串口)", win._status_label.text())

    def test_manual_temp_source(self):
        """手动来源：状态栏显示手动值，记录也写入手动温度（覆盖串口/模拟温度）。"""
        win = self.win
        win._temp_source = "manual"
        win._manual_temp = 26.5
        win._last_temp = None
        win._update_status()
        self.assertIn("26.5", win._status_label.text())
        self.assertIn("(手动)", win._status_label.text())
        # 触发一次记录：喂数据时模拟/串口温度给 25.5，
        # 手动来源下记录温度必须写入手动值 26.5
        win._threshold_spin.setValue(1.0)
        win._stable_spin.setValue(0.5)
        win._on_data(time.time(), 10.0, 25.5)   # 基线
        time.sleep(0.01)
        for _ in range(80):                      # 阶跃到 20 并稳定
            win._on_data(time.time(), 20.0, 25.5)
            time.sleep(0.01)
        self.assertGreaterEqual(len(win._records), 1, "应触发一次记录")
        self.assertEqual(win._records[-1][2], 26.5,
                         "手动来源下记录温度应为手动值")

    def test_temp_source_dialog(self):
        """温度来源对话框：构造与取值。"""
        from app.temp_source_dialog import TempSourceDialog
        dlg = TempSourceDialog("network", "上海", 22.0)
        src, city, manual = dlg.result_values()
        self.assertEqual(src, "network")
        self.assertEqual(city, "上海")
        dlg2 = TempSourceDialog("manual")
        self.assertEqual(dlg2.result_values()[0], "manual")
        dlg.deleteLater()
        dlg2.deleteLater()

    def test_threshold_controls_recorder(self):
        win = self.win
        win._threshold_spin.setValue(3.0)
        self.assertEqual(win._recorder.threshold, 3.0)
        win._stable_spin.setValue(1.5)
        self.assertEqual(win._recorder.stable_window, 1.5)


if __name__ == "__main__":
    unittest.main()
