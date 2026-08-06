"""称重传感器上位机入口。

运行：
    python main.py
"""
import sys

from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QApplication

from app.main_window import APP_ICON_PATH, MainWindow


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("称重上位机")
    app.setWindowIcon(QIcon(str(APP_ICON_PATH)))
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
