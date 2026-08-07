/**
 ******************************************************************************
 * @file    scale.c
 * @brief   称重模块实现：软件 SPI(CPOL=0/CPHA=1) 驱动 ADS1220，
 *          单次转换 20SPS + 滑动平均滤波，提供去皮/标定/重量/温度接口。
 ******************************************************************************
 */
#include "scale.h"
#include "ADS1220.h"

#include "main.h"          /* HAL + SystemCoreClock */
#include <stdbool.h>

extern ADC_HandleTypeDef hadc1;   /* 温度通道（main.c 中定义） */

/* ------------------------------------------------------------------ */
/* 引脚映射（原理图 / 引脚分配.md）                                   */
/* ------------------------------------------------------------------ */
#define CS_PORT     GPIOA
#define CS_PIN      GPIO_PIN_3
#define SCLK_PORT   GPIOA
#define SCLK_PIN    GPIO_PIN_4
#define DRDY_PORT   GPIOA
#define DRDY_PIN    GPIO_PIN_5
#define MISO_PORT   GPIOA
#define MISO_PIN    GPIO_PIN_6
#define MOSI_PORT   GPIOA
#define MOSI_PIN    GPIO_PIN_7

/* ------------------------------------------------------------------ */
/* 采样/滤波参数                                                       */
/* ------------------------------------------------------------------ */
#define SAMPLE_FILTER_N    16          /* 滑动平均窗口（20SPS 下约 0.8 s） */
#define DRDY_TIMEOUT_US    200000u     /* 等待 DRDY 超时（µs） */

/* ------------------------------------------------------------------ */
/* 状态                                                               */
/* ------------------------------------------------------------------ */
static ADS1220_Handler_t  s_adc = {0};
static int32_t  s_filter_buf[SAMPLE_FILTER_N];
static uint8_t  s_filter_idx = 0;
static uint8_t  s_filter_cnt = 0;
static int32_t  s_filtered_raw = 0;
static int32_t  s_tare_offset = 0;
static float    s_scale_g_per_lsb = SCALE_G_PER_LSB;

/* ------------------------------------------------------------------ */
/* 微秒延时（DWT 周期计数，72MHz 精确）                                */
/* ------------------------------------------------------------------ */
static void delay_us(uint32_t us)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t ticks = us * (SystemCoreClock / 1000000u);
  while ((DWT->CYCCNT - start) < ticks)
  {
  }
}

/* ------------------------------------------------------------------ */
/* 软件 SPI 底层（ADS1220 库回调）                                    */
/* ------------------------------------------------------------------ */
static void adc_cs_high(void)
{
  HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET);
}

static void adc_cs_low(void)
{
  HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_RESET);
}

static void adc_transmit(uint8_t data)
{
  uint8_t i;
  for (i = 0; i < 8; i++)
  {
    HAL_GPIO_WritePin(SCLK_PORT, SCLK_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(MOSI_PORT, MOSI_PIN,
                      (data & 0x80u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    data <<= 1;
    HAL_GPIO_WritePin(SCLK_PORT, SCLK_PIN, GPIO_PIN_SET);  /* 上升沿：ADC 采样 */
  }
  HAL_GPIO_WritePin(SCLK_PORT, SCLK_PIN, GPIO_PIN_RESET);
}

static uint8_t adc_receive(void)
{
  uint8_t r = 0;
  uint8_t i;
  for (i = 0; i < 8; i++)
  {
    HAL_GPIO_WritePin(SCLK_PORT, SCLK_PIN, GPIO_PIN_SET);
    r = (uint8_t)((r << 1) |
        ((HAL_GPIO_ReadPin(MISO_PORT, MISO_PIN) == GPIO_PIN_SET) ? 1u : 0u));
    HAL_GPIO_WritePin(SCLK_PORT, SCLK_PIN, GPIO_PIN_RESET);
  }
  return r;
}

static void adc_delay_us(uint32_t us)
{
  delay_us(us);
}

/* ------------------------------------------------------------------ */
/* ADS1220 配置                                                        */
/* 输入：AINP=AIN1(INA826 输出), AINN=AIN0(2.5V 基准) → 直接测电桥放大差
 * 基准：外部 REFP0/REFN0 = 2.5V/GND（REF5025）
 * 速率：Normal 20SPS + 50Hz 工频 FIR 抑制（中国电网）
 * 模式：单次转换（主循环轮询 DRDY）                                   */
/* ------------------------------------------------------------------ */
static ADS1220_Parameters_t s_params =
{
  .InputMuxConfig  = P1N0,          /* AINP=AIN1, AINN=AIN0 */
  .GainConfig      = _1_,
  .PGAdisable      = false,         /* PGA 使能，允许差分满量程 ±VREF */
  .DataRate        = _20_SPS_,
  .OperatingMode   = NormalMode,
  .ConversionMode  = false,         /* 单次转换 */
  .TempeSensorMode = false,
  .BurnOutCurrentSrc = false,
  .VoltageRef      = ExternalREF0,  /* REFP0/REFN0 = 2.5V/GND */
  .FIRFilter       = Rej50Hz,       /* 仅 20SPS 有效 */
  .LowSidePwr      = false,
  .IDACcurrent     = Off,
  .IDAC1routing    = Disabled,
  .IDAC2routing    = Disabled,
  .DRDYMode        = false,
};

/* ------------------------------------------------------------------ */
/* 内部函数                                                           */
/* ------------------------------------------------------------------ */

/** 启动一次单次转换并等待 DRDY 拉低，读出 24bit 数据。超时返回 0。 */
static uint8_t sample_once(int32_t *out)
{
  uint32_t t0;
  uint32_t timeout_ticks;

  ADS1220_StartSync(&s_adc);
  t0 = DWT->CYCCNT;
  timeout_ticks = DRDY_TIMEOUT_US * (SystemCoreClock / 1000000u);
  while (HAL_GPIO_ReadPin(DRDY_PORT, DRDY_PIN) != GPIO_PIN_RESET)
  {
    if ((DWT->CYCCNT - t0) > timeout_ticks)
    {
      return 0;                     /* 超时：ADC 未响应 */
    }
  }
  ADS1220_ReadData(&s_adc, out);
  return 1;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

void scale_init(void)
{
  uint8_t i;

  /* CS 默认拉高（未选中） */
  HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET);

  /* 使能 DWT 周期计数器（µs 延时/超时用） */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  /* 绑定 ADS1220 库回调 */
  s_adc.ADC_CS_HIGH    = adc_cs_high;
  s_adc.ADC_CS_LOW     = adc_cs_low;
  s_adc.ADC_Transmit   = adc_transmit;
  s_adc.ADC_Receive    = adc_receive;
  s_adc.ADC_Delay_US   = adc_delay_us;

  for (i = 0; i < SAMPLE_FILTER_N; i++)
  {
    s_filter_buf[i] = 0;
  }

  ADS1220_Init(&s_adc, &s_params);

  /* 预热一次，保证后续 DRDY 时序稳定（首次上电首帧可能无效） */
  (void)sample_once(&s_filtered_raw);
}

uint8_t scale_update(void)
{
  int32_t raw;
  int32_t sum = 0;
  uint8_t i;

  if (!sample_once(&raw))
  {
    return 0;
  }

  s_filter_buf[s_filter_idx] = raw;
  s_filter_idx = (uint8_t)((s_filter_idx + 1u) % SAMPLE_FILTER_N);
  if (s_filter_cnt < SAMPLE_FILTER_N)
  {
    s_filter_cnt++;
  }

  for (i = 0; i < s_filter_cnt; i++)
  {
    sum += s_filter_buf[i];
  }
  s_filtered_raw = sum / (int32_t)s_filter_cnt;
  return 1;
}

void scale_tare(void)
{
  s_tare_offset = s_filtered_raw;
}

int32_t scale_raw_net(void)
{
  return s_filtered_raw - s_tare_offset;
}

int32_t scale_weight_centi(void)
{
  float gc = (float)(s_filtered_raw - s_tare_offset) * s_scale_g_per_lsb * 100.0f;

  if (gc >  999999.0f) gc =  999999.0f;
  if (gc < -999999.0f) gc = -999999.0f;
  return (int32_t)gc;
}

int32_t scale_temp_centi(void)
{
  uint16_t raw = 0;

  /* STM32 ADC1 通道 1（PA1）读外部模拟温度传感器 */
  if (HAL_ADC_Start(&hadc1) == HAL_OK)
  {
    if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK)
    {
      raw = (uint16_t)HAL_ADC_GetValue(&hadc1);
    }
    HAL_ADC_Stop(&hadc1);
  }

  /* V = raw/4095*3.3V；T(°C) = V/(mV_PER_C/1000)；
   * T×10 = raw * 33000 / (4095 * TEMP_SENSOR_MV_PER_C) */
  return (int32_t)((int32_t)raw * 33000L / (4095L * (int32_t)TEMP_SENSOR_MV_PER_C));
}
