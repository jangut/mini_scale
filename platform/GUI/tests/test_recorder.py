"""记录状态机单元测试。"""
import unittest

from app.recorder import Recorder


class FakeClock:
    """可控时钟，让稳定窗口判定可预测。"""

    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def advance(self, dt):
        self.t += dt


class RecorderTest(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.rec = Recorder(threshold=0.5, stable_window=2.0,
                            now=self.clock)

    def test_noise_below_threshold_never_records(self):
        """基线附近小幅波动（< 阈值）不应触发记录。"""
        self.assertIsNone(self.rec.update(10.0))
        self.clock.advance(0.1)
        for i in range(60):
            v = 10.0 + (i % 5) * 0.1  # 最大偏离 0.4 < 0.5
            self.assertIsNone(self.rec.update(v), f"第 {i} 帧不应记录")
            self.clock.advance(0.1)

    def test_step_records_once_after_stable_window(self):
        """阶跃超过阈值后，稳定满窗口应记录一次。"""
        self.assertIsNone(self.rec.update(10.0))   # 基线
        self.clock.advance(0.1)
        self.assertIsNone(self.rec.update(12.0))   # 触发 ARMING
        self.clock.advance(0.1)
        events = []
        for _ in range(30):
            ev = self.rec.update(12.0)
            if ev:
                events.append(ev)
            self.clock.advance(0.1)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0][0], "record")
        self.assertAlmostEqual(events[0][1], 12.0)

    def test_no_repeat_while_stable(self):
        """记录后保持稳定，不应重复记录。"""
        self.rec.update(10.0)
        self.clock.advance(0.1)
        self.rec.update(12.0)
        self.clock.advance(0.1)
        for _ in range(30):
            self.rec.update(12.0)
            self.clock.advance(0.1)
        # 再稳定 5 秒
        for _ in range(50):
            self.assertIsNone(self.rec.update(12.0), "稳定时不应重复记录")
            self.clock.advance(0.1)

    def test_second_step_records_again(self):
        """记录后再次阶跃应再次记录。"""
        self.rec.update(10.0)
        self.clock.advance(0.1)
        self.rec.update(12.0)          # 第一次阶跃
        self.clock.advance(0.1)
        for _ in range(30):
            self.rec.update(12.0)
            self.clock.advance(0.1)    # 第一次记录完成

        self.rec.update(15.0)          # 第二次阶跃
        self.clock.advance(0.1)
        events = []
        for _ in range(30):
            ev = self.rec.update(15.0)
            if ev:
                events.append(ev)
            self.clock.advance(0.1)
        self.assertEqual(len(events), 1)
        self.assertAlmostEqual(events[0][1], 15.0)

    def test_continuous_change_resets_stable_timer(self):
        """持续缓慢爬升（每帧都小幅变化但未超稳定判定）不应误记录。"""
        self.rec.update(10.0)
        self.clock.advance(0.1)
        self.rec.update(12.0)          # 触发
        self.clock.advance(0.1)
        v = 12.0
        events = []
        # 每帧 +0.4（< 阈值），但相对 potential 变化累计超过阈值会重置计时
        for i in range(40):
            v += 0.4
            ev = self.rec.update(v)
            if ev:
                events.append(ev)
            self.clock.advance(0.1)
        self.assertEqual(len(events), 0, "持续爬升不应记录")

    def test_first_value_is_baseline_not_recorded(self):
        """首个读数只作为基线，不立即记录。"""
        self.assertIsNone(self.rec.update(42.0))
        self.assertFalse(self.rec.armed)

    def test_reset_clears_state(self):
        self.rec.update(10.0)
        self.clock.advance(0.1)
        self.rec.update(12.0)
        self.assertTrue(self.rec.armed)
        self.rec.reset()
        self.assertFalse(self.rec.armed)
        self.assertIsNone(self.rec.update(7.0))  # 新基线


if __name__ == "__main__":
    unittest.main()
