"""称重记录状态机：变化超过阈值触发，稳定窗口确认后记录一次。

状态机：
- 基线/已记录（IDLE）：重量相对上次记录值变化超过 threshold 时进入 ARMING。
- 变化中（ARMING）：持续跟踪潜在记录值；只要读数仍在显著变化（相对潜在值
  变化 >= threshold）就重置稳定计时；当连续 stable_window 秒内读数稳定
  （相对潜在值变化 < threshold）即确认记录，回到 IDLE。

特点：
- 记录后不会重复记录同一个稳定值；只有再次偏离超过阈值才会重新触发。
- 阈值同时充当噪声抑制，避免传感器抖动导致误记录。
"""
import time


class Recorder:
    def __init__(self, threshold: float = 0.5, stable_window: float = 2.0,
                 now=None):
        if threshold <= 0:
            raise ValueError("threshold 必须大于 0")
        if stable_window <= 0:
            raise ValueError("stable_window 必须大于 0")
        self.threshold = threshold
        self.stable_window = stable_window
        self._now = now or time.time
        self._last_recorded = None   # 最近一次记录（或基线）的重量
        self._potential = None       # ARMING 中的潜在记录值
        self._stable_since = None    # 潜在值开始稳定的时刻
        self.armed = False           # 是否处于"变化中待确认"状态

    def reset(self):
        """清空状态（重新连接 / 参数重置时调用）。"""
        self._last_recorded = None
        self._potential = None
        self._stable_since = None
        self.armed = False

    def update(self, value: float, ts: float = None):
        """喂入一个新读数。

        返回 ("record", value, ts) 表示此刻确认记录一次；
        否则返回 None。
        """
        ts = ts if ts is not None else self._now()

        if self._last_recorded is None:
            # 首个读数只作为基线，不记录（避免刚开机记录到乱值）
            self._last_recorded = value
            self.armed = False
            return None

        if not self.armed:
            if abs(value - self._last_recorded) >= self.threshold:
                self.armed = True
                self._potential = value
                self._stable_since = ts
            return None

        # ARMING：变化中，等待稳定确认
        if abs(value - self._potential) >= self.threshold:
            # 仍在显著变化：更新潜在值并重置稳定计时
            self._potential = value
            self._stable_since = ts
            return None

        if ts - self._stable_since >= self.stable_window:
            # 稳定窗口满足：确认记录
            self._last_recorded = value
            self._potential = None
            self._stable_since = None
            self.armed = False
            return ("record", value, ts)

        return None
