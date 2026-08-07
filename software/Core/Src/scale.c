/**
 ******************************************************************************
 * @file    scale.c
 * @brief   称重模块实现：软件 SPI(CPOL=0/CPHA=1) 驱动 ADS1220 单次转换，
 *          非阻塞采样（DRDY 就绪才读）+ 去极值滑动平均滤波，
 *          去皮/标定参数存内部 Flash（掉电保存）。
 ******************************************************************************
 */
#include "scale.h"
#include "ADS1220.h"

#include "main.h"          /* HAL + SystemCoreClock */
#include <string.h>
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
/* 参数区：内部 Flash 最后一页（F103C8 64KB，页 63: 0x0800FC00）        */
/* ------------------------------------------------------------------ */
#define FLASH_PARAM_ADDR   0x0800FC00u
#define PARAM_MAGIC        0xA5A5A5A5u

typedef struct
{
  uint32_t magic;       /* 有效性标记 */
  int32_t  zero_offset; /* 去皮零点（原始码） */
  uint32_t k_bits;      /* K 值（float 位模式），单位 g/lsb */
  uint32_t crc;         /* magic ^ zero ^ k 校验 */
} scale_param_t;

/* ------------------------------------------------------------------ */
/* 采样/滤波参数                                                       */
/* ------------------------------------------------------------------ */
#define FILTER_WIN  8               /* 滤波窗口（20SPS 下约 0.4 s） */

/* ------------------------------------------------------------------ */
/* 状态                                                               */
/* ------------------------------------------------------------------ */
static ADS1220_Handler_t s_adc = {0};
static int32_t  s_raw_buf[FILTER_WIN];
static uint8_t  s_raw_cnt = 0;
static int32_t  s_filtered_raw = 0;
static int32_t  s_tare_offset = 0;
static float    s_scale_g_per_lsb = SCALE_G_PER_LSB;

static scale_param_t s_param;

/* ------------------------------------------------------------------ */
/* ADS1220 配置：单次转换 20SPS + 50Hz 工频抑制，差分 AIN1-AIN0         */
/* ------------------------------------------------------------------ */
static ADS1220_Parameters_t s_params =
{
  .InputMuxConfig  = P1N0,          /* AINP=AIN1(INA826 输出), AINN=AIN0(2.5V) */
  .GainConfig      = _1_,
  .PGAdisable      = false,
  .DataRate        = _20_SPS_,
  .OperatingMode   = NormalMode,
  .ConversionMode  = false,         /* 单次转换：每轮 START 一次 */
  .TempeSensorMode = false,
  .BurnOutCurrentSrc = false,
  .VoltageRef      = ExternalREF0,  /* REFP0/REFN0 = 2.5V/GND */
  .FIRFilter       = Rej50Hz,
  .LowSidePwr      = false,
  .IDACcurrent     = Off,
  .IDAC1routing    = Disabled,
  .IDAC2routing    = Disabled,
  .DRDYMode        = false,
};

/* ------------------------------------------------------------------ */
/* Flash 参数读写                                                      */
/* ------------------------------------------------------------------ */
static uint32_t param_crc(const scale_param_t *p)
{
  return p->magic ^ (uint32_t)p->zero_offset ^ p->k_bits;
}

static void param_load(void)
{
  const uint32_t *p = (const uint32_t *)FLASH_PARAM_ADDR;
  union { float f; uint32_t u; } k;

  s_param.magic      = p[0];
  s_param.zero_offset = (int32_t)p[1];
  s_param.k_bits     = p[2];
  s_param.crc        = p[3];

  if ((s_param.magic != PARAM_MAGIC) || (s_param.crc != param_crc(&s_param)))
  {
    /* 无有效参数（首次上电/被擦除）：用默认值 */
    s_param.magic = PARAM_MAGIC;
    s_param.zero_offset = 0;
    k.f = SCALE_G_PER_LSB;
    s_param.k_bits = k.u;
    s_param.crc = param_crc(&s_param);
  }

  s_tare_offset = s_param.zero_offset;
  k.u = s_param.k_bits;
  s_scale_g_per_lsb = k.f;
}

static void param_save(void)
{
  FLASH_EraseInitTypeDef er;
  uint32_t err = 0;

  s_param.crc = param_crc(&s_param);

  HAL_FLASH_Unlock();
  er.TypeErase = FLASH_TYPEERASE_PAGES;
  er.PageAddress = FLASH_PARAM_ADDR;
  er.NbPages = 1;
  HAL_FLASHEx_Erase(&er, &err);
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, FLASH_PARAM_ADDR +  0u, s_param.magic);
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, FLASH_PARAM_ADDR +  4u, (uint32_t)s_param.zero_offset);
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, FLASH_PARAM_ADDR +  8u, s_param.k_bits);
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, FLASH_PARAM_ADDR + 12u, s_param.crc);
  HAL_FLASH_Lock();
}

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
/* 滤波：去极值滑动平均（窗口 8，去掉最大/最小后平均）                  */
/* ------------------------------------------------------------------ */
static int32_t filter_push(int32_t in)
{
  uint8_t i, j;
  int32_t tmp[FILTER_WIN];
  int32_t sum;

  if (s_raw_cnt < FILTER_WIN)
  {
    s_raw_buf[s_raw_cnt++] = in;
  }
  else
  {
    for (i = 1; i < FILTER_WIN; i++)
    {
      s_raw_buf[i - 1] = s_raw_buf[i];
    }
    s_raw_buf[FILTER_WIN - 1] = in;
  }

  if (s_raw_cnt >= FILTER_WIN)
  {
    /* 冒泡排序（8 元素，20Hz 下开销可忽略） */
    for (i = 0; i < FILTER_WIN; i++)
    {
      tmp[i] = s_raw_buf[i];
    }
    for (i = 0; i < FILTER_WIN - 1; i++)
    {
      for (j = 0; j < FILTER_WIN - 1 - i; j++)
      {
        if (tmp[j] > tmp[j + 1])
        {
          int32_t t = tmp[j];
          tmp[j] = tmp[j + 1];
          tmp[j + 1] = t;
        }
      }
    }
    sum = 0;
    for (i = 1; i < FILTER_WIN - 1; i++)   /* 去掉最大最小 */
    {
      sum += tmp[i];
    }
    s_filtered_raw = sum / (int32_t)(FILTER_WIN - 2);
  }
  else
  {
    sum = 0;
    for (i = 0; i < s_raw_cnt; i++)
    {
      sum += s_raw_buf[i];
    }
    s_filtered_raw = sum / (int32_t)s_raw_cnt;
  }
  return s_filtered_raw;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

void scale_init(void)
{
  uint8_t i;

  /* CS 默认拉高（未选中） */
  HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET);

  /* 使能 DWT 周期计数器（µs 延时用） */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  /* 绑定 ADS1220 库回调 */
  s_adc.ADC_CS_HIGH    = adc_cs_high;
  s_adc.ADC_CS_LOW     = adc_cs_low;
  s_adc.ADC_Transmit   = adc_transmit;
  s_adc.ADC_Receive    = adc_receive;
  s_adc.ADC_Delay_US   = adc_delay_us;

  for (i = 0; i < FILTER_WIN; i++)
  {
    s_raw_buf[i] = 0;
  }
  s_raw_cnt = 0;

  /* 读 Flash 标定参数 */
  param_load();

  ADS1220_Init(&s_adc, &s_params);

  /* 启动首次转换 */
  ADS1220_StartSync(&s_adc);
}

uint8_t scale_update(void)
{
  int32_t raw;

  /* 数据未就绪则直接返回（非阻塞） */
  if (HAL_GPIO_ReadPin(DRDY_PORT, DRDY_PIN) != GPIO_PIN_RESET)
  {
    return 0;
  }

  ADS1220_ReadData(&s_adc, &raw);
  filter_push(raw);
  return 1;
}

void scale_start(void)
{
  ADS1220_StartSync(&s_adc);
}

void scale_tare(void)
{
  s_tare_offset = s_filtered_raw;
  s_param.zero_offset = s_tare_offset;
  param_save();
}

void scale_calibrate(float weight_g)
{
  union { float f; uint32_t u; } k;
  int32_t net = s_filtered_raw - s_tare_offset;

  if (net == 0)
  {
    return;                    /* 无有效信号，忽略 */
  }
  s_scale_g_per_lsb = weight_g / (float)net;
  if ((s_scale_g_per_lsb <= 0.0f) || (s_scale_g_per_lsb > 1000.0f))
  {
    return;                    /* 合理性检查 */
  }

  k.f = s_scale_g_per_lsb;
  s_param.k_bits = k.u;
  param_save();
}

void scale_power_down(void)
{
  ADS1220_PowerDown(&s_adc);
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
