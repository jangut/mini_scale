/**
 ******************************************************************************
 * @file    scale.h
 * @brief   称重模块：软件 SPI + ADS1220(24bit ADC) + 采样/滤波/去皮/标定
 *
 * 引脚定义（与 hardware 原理图 / 引脚分配.md 一致）：
 *   PA3 = CS, PA4 = SCLK, PA5 = DRDY, PA6 = MISO, PA7 = MOSI
 *   PA1 = 温度传感器模拟输入（STM32 ADC1_IN1）
 *
 * 采样架构：ADS1220 单次转换 + TIM3 采样节拍（50ms）驱动。
 *   节拍中断置标志 -> 主循环调用 scale_update()（非阻塞，DRDY 就绪才读）
 *   -> 读完后调用 scale_start() 启动下一轮转换。
 *
 * 标定参数（零点 + K 值）保存在内部 Flash 最后一页，掉电不丢。
 ******************************************************************************
 */
#ifndef SCALE_H
#define SCALE_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* 默认标定参数（首次上电 / Flash 无有效参数时使用）                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 1 个 ADC 原始码（滤波后、去皮后）对应的克数。
 * 标定方法：放已知砝码 W(g) 后调用 scale_calibrate(W)，K 值自动存 Flash；
 * 也可直接改本宏作为出厂默认值。
 * 注意：ADS1220 满量程 ±2.5V（外部基准 REF5025），信号链为
 *   应变片 -> INA826(G≈989) -> ADS1220 AIN1-AIN0 差分。
 */
#define SCALE_G_PER_LSB       1.0f

/** 模拟温度传感器灵敏度（mV/°C）：LM35 类 = 10，其余按传感器手册改 */
#define TEMP_SENSOR_MV_PER_C  10

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

/** 初始化：读 Flash 参数、ADS1220 初始化、启动首次转换（须在时钟/GPIO 之后调用） */
void     scale_init(void);

/** 非阻塞采样：DRDY 就绪则读 24bit 并滤波，返回 1=有新数据；未就绪返回 0 */
uint8_t  scale_update(void);

/** 启动一次转换（单次模式，由采样节拍调用；初始化后也要调一次） */
void     scale_start(void);

/** 去皮：当前读数置零并写入 Flash */
void     scale_tare(void);

/** 标定：以已知砝码重量 weight_g(g) 计算 K 值并写入 Flash */
void     scale_calibrate(float weight_g);

/** 休眠前给 ADS1220 下电（省电） */
void     scale_power_down(void);

/** 去皮后的原始码（int32，24bit 有符号） */
int32_t  scale_raw_net(void);

/** 重量 × 100（0.01 g 分辨率） */
int32_t  scale_weight_centi(void);

/** 温度 × 10（0.1 °C 分辨率） */
int32_t  scale_temp_centi(void);

#endif /* SCALE_H */
