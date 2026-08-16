# 基于 STM32 的小型高精度电子秤（mini_scale）

一款以 **STM32F103C8T6** 为核心的小型高精度电子秤：称重传感器信号经仪表放大器调理后，由 **ADS1220（24 位 Δ-Σ ADC）** 采样，固件完成滤波、去皮、标定与温度补偿，结果同时显示在 OLED 上并通过串口/蓝牙上报给 PC 上位机（实时折线图、自动记录、CSV 导出）。

## 特性

- **高精度采样**：ADS1220 24 位 ADC，外部 REF5025（2.5V）基准，20SPS，软件 SPI 读取
- **满量程 500 g**，过载保护 600 g（120% FS），校准后残差 < 0.25 g
- **抗干扰**：中值滤波 + 滑动平均 + IIR 低通三级数字滤波
- **去皮（TARE）**：按键一键去皮，支持自动零点跟踪
- **温度补偿**：DS18B20 采集环境温度，参与称重漂移补偿
- **低功耗**：读数稳定 2 分钟自动进入 STOP 模式，任意按键 EXTI 唤醒
- **开机预热**：上电后自动等待模拟链路稳定再上报数据，避免漂移误报
- **双模式**：称重模式 / 外设自检模式（ADS1220、DS18B20、UART 回环、按键）
- **蓝牙**：JDY-31 模块，UART 透传，免线连接上位机
- **上位机**：PySide6 + pyqtgraph 图形界面，可打包为免安装 exe

## 系统架构

```
称重传感器(电桥) ──► INA826 仪表放大器 ──► ADS1220 (24bit ADC, 软件SPI) ──┐
                                                                        │
DS18B20 (温度) ────────────────────────────────► STM32F103C8T6 ◄─────────┘
                                                      │  │
                                    OLED(SSD1306, I²C) │  └─► UART ──► JDY-31 蓝牙
                                             按键×2    └─────► UART(USB转串口)
                                                                        │
                                                        PC 上位机 (platform/GUI)
```

## 目录结构

```
mini_scale/
├── framework/            # 外壳设计（SolidWorks：PCB.SLDPRT / 装配体1.SLDASM）
├── hardware/             # 硬件设计
│   ├── mini_scale/       #   Altium Designer 工程（原理图 P1.schdoc / PCB1.pcbdoc / BOM）
│   ├── ProPrj_miniscale2.0_2026-08-06.epro2   # 嘉立创EDA专业版工程
│   └── simulink/         #   Proteus 仿真（simulink.dsn + SIMULINK.png）
├── software/             # STM32 固件（Keil MDK-ARM V5.32）
│   ├── Core/             #   CubeMX 生成（main / USART / TIM2 / GPIO）
│   ├── Device/           #   设备驱动与业务逻辑（ADS1220 / scale / oled / ds18b20 / jdy_slave / self_test / app）
│   ├── ADS1220-master/   #   第三方 ADS1220 驱动库
│   ├── MDK-ARM/          #   Keil 工程文件
│   ├── tools/            #   标定 / 漂移分析 / 串口调试脚本（Python）
│   ├── software.ioc      #   CubeMX 工程
│   ├── ADS1220.md        #   ADS1220 数据手册（TI ZHCSBH5D）
│   ├── 引脚分配.md        #   STM32 引脚功能分配手册
│   └── 流程图.md          #   软件流程图
└── platform/GUI/         # PC 上位机（Python + PySide6，详见其 README.md）
```

## 硬件电路（hardware/）

| 模块 | 器件 | 说明 |
| :--- | :--- | :--- |
| 主控 | STM32F103C8T6（LQFP48） | 72 MHz，HSE 晶振 |
| 采样 ADC | ADS1220（24 位） | 软件 SPI（CS=PA3, SCLK=PA4, DRDY=PA5, MISO=PA6, MOSI=PA7），20SPS |
| 信号调理 | INA826 仪表放大器 | 称重传感器差分信号 → ADS1220 AIN1（单端 vs AVSS） |
| 基准 | REF5025 | 2.5V 精密基准 → REFP0 |
| 温度 | DS18B20 | 单总线，12 位转换，约 0.75 s 周期 |
| 显示 | SSD1306 OLED 128×64 | 软件 I²C（PB6=SCL, PB7=SDA） |
| 按键 | KEY1=PB4 / KEY2=PB5 | 去皮、模式切换、唤醒（EXTI 上升沿） |
| 蓝牙 | JDY-31 | UART 透传（9600 默认） |
| 下载 | SWD | 引脚 34/37 |

原理图与 PCB 位于 `hardware/mini_scale/PCB_Project/`（Altium Designer），嘉立创 EDA 专业版工程见 `hardware/ProPrj_miniscale2.0_2026-08-06.epro2`，Proteus 仿真电路见 `hardware/simulink/`。完整引脚分配见 `software/引脚分配.md`。

> ⚠️ 固件中 ADS1220 必须使用**软件模拟 SPI**（走线限制，勿开启硬件 SPI 外设）。

## 外壳（framework/）

SolidWorks 三维模型：PCB 外形 `PCB.SLDPRT`、外壳零件 `零件1~3.SLDPRT` 及装配体 `装配体1.SLDASM`。

## 固件（software/）

基于 STM32CubeMX（`software.ioc`）+ Keil MDK-ARM V5.32 工程（`software/MDK-ARM/`）。

### 串口协议（与上位机约定）

每行一条记录，逗号分隔，以换行符结尾，约 20 Hz 上报：

```
123.45,25.6     ← 重量(g), 温度(°C)，两位 / 一位小数
```

- 开机预热期间上报 `WARMUP:<剩余秒数>`，进入休眠前发送 `SLEEP`
- 自检模式下执行外设自检（含 10 s UART 回环测试），不做常规上报
- 波特率需与上位机设置一致（固件默认 9600）

### 按键与模式

| 按键 | 功能 |
| :--- | :--- |
| KEY2（PB5） | 短按去皮 |
| KEY1（PB4） | 长按（>2 s）切换称重模式 ↔ 外设自检模式；任意按键均可从休眠唤醒 |

### 低功耗

读数在 ±1 g 内稳定持续 2 分钟且无按键活动时，自动进入 STOP 模式（ADS1220 掉电、OLED 熄屏）；任意按键 EXTI 上升沿唤醒，唤醒后恢复 72 MHz 时钟并保留去皮零点。调试器（SWD）连接时自动禁用休眠，避免核心停止导致无法 halt。

### 标定

`scale.h` 中 `SCALE_LSB_TO_G` 为线性标定（2026-08-16，0~200 g 砝码组），`scale.c` 内叠加二次非线性校正（`SCALE_NL_A/B`），标定脚本见 `software/tools/`（`calibrate.py`、`calibrate_full.py`、`refit_nl.py` 等）。

### 构建

1. 用 Keil MDK-ARM V5.32 打开 `software/MDK-ARM/software.uvprojx`
2. 编译并下载（SWD，STM32F103C8T6）
3. 如需重新生成外设代码，用 CubeMX 打开 `software/software.ioc`

## 上位机（platform/GUI/）

Python 桌面程序（PySide6 + pyqtgraph + pyserial）：实时重量折线图、自动记录（变化超阈值 + 稳定确认）、温度显示（串口 / 网络定位 / 手动）、CSV 导出，支持模拟数据与虚拟串口测试，可打包为单文件 exe（`dist/MiniScale.exe`）。

```powershell
conda activate speech
cd platform/GUI
python main.py          # 运行
.\build.ps1             # 打包 exe
```

详细说明（界面布局、串口协议、记录规则、无硬件测试、打包等）见 [`platform/GUI/README.md`](platform/GUI/README.md)。

## 相关文档

| 文档 | 内容 |
| :--- | :--- |
| [`software/引脚分配.md`](software/引脚分配.md) | STM32 引脚功能分配（电源 / 外设 / 速查表） |
| [`software/ADS1220.md`](software/ADS1220.md) | ADS1220 数据手册（TI ZHCSBH5D，含电桥测量参考设计） |
| [`software/流程图.md`](software/流程图.md) | 软件流程图 |
| [`platform/GUI/README.md`](platform/GUI/README.md) | 上位机使用与开发文档 |
| [`software/ADS1220-master/README.md`](software/ADS1220-master/README.md) | ADS1220 驱动库说明 |
