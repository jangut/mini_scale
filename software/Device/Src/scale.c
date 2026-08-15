/**
 **********************************************************************************
 * @file   scale.c
 * @brief  Weighing scale core: ADS1220 sampling, median+average filter,
 *         tare, raw->gram conversion, overload/fault detection.
 *
 * ADS1220 is read in continuous mode @20SPS. Input AIN1 single-ended vs AVSS,
 * external 2.5V reference (REFP0), PGA bypassed, gain 1 (see scale.h for the
 * raw->gram calibration placeholder).
 **********************************************************************************
 **/
#include "scale.h"
#include "delay.h"
#include "ADS1220.h"
#include "stm32f1xx_hal.h"

/* ------------------------------------------------------------------ */
/*  ADS1220 software SPI (CPOL=0, CPHA=1)                              */
/* ------------------------------------------------------------------ */
#define ADC_CS_PORT     GPIOA
#define ADC_CS_PIN      GPIO_PIN_3
#define ADC_SCLK_PORT   GPIOA
#define ADC_SCLK_PIN    GPIO_PIN_4
#define ADC_MISO_PORT   GPIOA
#define ADC_MISO_PIN    GPIO_PIN_6
#define ADC_MOSI_PORT   GPIOA
#define ADC_MOSI_PIN    GPIO_PIN_7
#define ADC_DRDY_PORT   GPIOA
#define ADC_DRDY_PIN    GPIO_PIN_5

static ADS1220_Handler_t    s_adc;
static ADS1220_Parameters_t s_params;

static void ADC_CS_HIGH(void) { HAL_GPIO_WritePin(ADC_CS_PORT, ADC_CS_PIN, GPIO_PIN_SET); }
static void ADC_CS_LOW(void)  { HAL_GPIO_WritePin(ADC_CS_PORT, ADC_CS_PIN, GPIO_PIN_RESET); }
/* DRDY# low = data ready: return 1 = busy, 0 = ready */
static uint8_t ADC_DRDY_Read(void)
{
  return (HAL_GPIO_ReadPin(ADC_DRDY_PORT, ADC_DRDY_PIN) == GPIO_PIN_SET) ? 1u : 0u;
}
static void ADC_Delay_US(uint32_t us) { Delay_us(us); }

static uint8_t SPI_Shift(uint8_t data)
{
  uint8_t recv = 0;
  uint8_t i;
  for (i = 0; i < 8; i++)
  {
    HAL_GPIO_WritePin(ADC_SCLK_PORT, ADC_SCLK_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(ADC_MOSI_PORT, ADC_MOSI_PIN, (data & 0x80u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    ADC_Delay_US(1);
    HAL_GPIO_WritePin(ADC_SCLK_PORT, ADC_SCLK_PIN, GPIO_PIN_SET);    /* rising edge */
    ADC_Delay_US(1);
    HAL_GPIO_WritePin(ADC_SCLK_PORT, ADC_SCLK_PIN, GPIO_PIN_RESET);  /* falling edge */
    ADC_Delay_US(1);
    recv = (uint8_t)((recv << 1) | ((HAL_GPIO_ReadPin(ADC_MISO_PORT, ADC_MISO_PIN) == GPIO_PIN_SET) ? 1u : 0u));
    data <<= 1;
  }
  return recv;
}
static void ADC_Transmit(uint8_t d) { SPI_Shift(d); }
static uint8_t ADC_Receive(void)    { return SPI_Shift(0xFFu); }
static uint8_t ADC_TransmitReceive(uint8_t d) { return SPI_Shift(d); }

/* Explicit pin config: gpio.c (CubeMX) wrongly sets PA6(MISO) as output,
   it must be an input for software SPI. */
static void ADS1220_GPIO_Init(void)
{
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  g.Pin   = ADC_CS_PIN | ADC_SCLK_PIN | ADC_MOSI_PIN;
  g.Mode  = GPIO_MODE_OUTPUT_PP;
  g.Pull  = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOA, &g);
  g.Pin   = ADC_DRDY_PIN | ADC_MISO_PIN;
  g.Mode  = GPIO_MODE_INPUT;
  g.Pull  = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &g);
  HAL_GPIO_WritePin(ADC_CS_PORT, ADC_CS_PIN, GPIO_PIN_SET);
}

/* ------------------------------------------------------------------ */
/*  Filtering                                                          */
/* ------------------------------------------------------------------ */
#define MEDIAN_N   5
#define AVG_N      32

static int32_t s_medBuf[MEDIAN_N];
static uint8_t s_medIdx;
static int32_t s_avgBuf[AVG_N];
static uint8_t s_avgCnt;     /* filled entries so far */
static uint8_t s_avgIdx;
static int32_t s_iir;        /* IIR low-pass output (alpha ~0.4) */

static int32_t Scale_MedianFilter(int32_t raw)
{
  uint8_t i, j;
  int32_t tmp[MEDIAN_N];
  s_medBuf[s_medIdx] = raw;
  s_medIdx = (uint8_t)((s_medIdx + 1u) % MEDIAN_N);
  for (i = 0; i < MEDIAN_N; i++) { tmp[i] = s_medBuf[i]; }
  /* simple insertion sort */
  for (i = 1; i < MEDIAN_N; i++)
  {
    int32_t key = tmp[i];
    j = i;
    while (j > 0 && tmp[j - 1] > key) { tmp[j] = tmp[j - 1]; j--; }
    tmp[j] = key;
  }
  return tmp[MEDIAN_N / 2];
}

static int32_t Scale_AverageFilter(int32_t med)
{
  int32_t sum = 0;
  uint8_t i;
  s_avgBuf[s_avgIdx] = med;
  s_avgIdx = (uint8_t)((s_avgIdx + 1u) % AVG_N);
  if (s_avgCnt < AVG_N) { s_avgCnt++; }
  for (i = 0; i < s_avgCnt; i++) { sum += s_avgBuf[i]; }
  return sum / (int32_t)s_avgCnt;
}

/* one-pole IIR low-pass, alpha = 0.25.
   Integer division adds a small dead-zone (<4 LSB) that suppresses jitter
   while still tracking real load changes (tau ~ 4 samples = 200ms @20SPS). */
static int32_t Scale_LowPass(int32_t in)
{
  s_iir += ((in - s_iir) / 4);
  return s_iir;
}

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */
#define FAULT_RAW_MAX     0x7FFFF0u
#define STABLE_LSB        400       /* ~0.28g at 0.0007g/LSB calibrated */
#define STABLE_CNT        10

static Scale_State_t s_state;
static int32_t s_zeroRaw;      /* tare offset (raw) */
static int32_t s_rawFilt;      /* filtered raw */
static int32_t s_lastFilt;
static uint8_t s_faultCnt;
static uint8_t s_stableCnt;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Take `samples` single-shot conversions (StartSync + wait DRDY#) and
   feed them through the filter chain, updating s_rawFilt. */
static void Scale_Warmup(uint8_t samples)
{
  uint8_t i;
  for (i = 0; i < samples; i++)
  {
    uint32_t timeout = 60u;
    int32_t raw = 0;
    ADS1220_StartSync(&s_adc);
    while (ADC_DRDY_Read() != 0u && (timeout-- > 0u)) { Delay_ms(1); }
    if (ADC_DRDY_Read() == 0u)
    {
      ADS1220_ReadData(&s_adc, &raw);
      s_rawFilt = Scale_LowPass(Scale_AverageFilter(Scale_MedianFilter(raw)));
    }
  }
}

void Scale_Init(void)
{
  uint8_t i;
  ADS1220_GPIO_Init();

  s_adc.ADC_CS_HIGH         = ADC_CS_HIGH;
  s_adc.ADC_CS_LOW          = ADC_CS_LOW;
  s_adc.ADC_Transmit        = ADC_Transmit;
  s_adc.ADC_Receive         = ADC_Receive;
  s_adc.ADC_TransmitReceive = ADC_TransmitReceive;
  s_adc.ADC_DRDY_Read       = ADC_DRDY_Read;
  s_adc.ADC_Delay_US        = ADC_Delay_US;

  /* Single-ended AIN1 vs AVSS (PGA must be bypassed), external 2.5V ref,
     gain 1, SINGLE-SHOT mode (each Scale_Update starts a conversion via
     StartSync, see below), 20 SPS. */
  s_params.InputMuxConfig = P1NAVSS;
  s_params.GainConfig     = _1_;
  s_params.PGAdisable     = true;
  s_params.VoltageRef     = ExternalREF0;
  s_params.FIRFilter      = S50or60Hz;  /* reject mains 50/60Hz hum (20SPS FIR mode):
                                           biggest source of reading wander */
  s_params.DataRate       = _20_SPS_;
  s_params.OperatingMode  = NormalMode;
  s_params.ConversionMode = false;   /* single-shot, StartSync per sample */
  s_params.TempeSensorMode = false;
  s_params.BurnOutCurrentSrc = false;
  s_params.LowSidePwr     = false;
  s_params.IDACcurrent    = Off;
  s_params.IDAC1routing   = Disabled;
  s_params.IDAC2routing   = Disabled;
  s_params.DRDYMode       = false;

  ADS1220_Init(&s_adc, &s_params);

  for (i = 0; i < MEDIAN_N; i++) { s_medBuf[i] = 0; }
  for (i = 0; i < AVG_N; i++)    { s_avgBuf[i] = 0; }
  s_medIdx = 0; s_avgIdx = 0; s_avgCnt = 0;
  s_faultCnt = 0; s_stableCnt = 0;
  s_state = SCALE_STATE_INIT;

  /* warm-up a few samples, then auto-tare at power-up (empty platform).
     Single-shot: StartSync then wait DRDY# (max ~60ms per sample). */
  Scale_Warmup(MEDIAN_N + AVG_N);
  s_zeroRaw  = s_rawFilt;
  s_lastFilt = s_rawFilt;
  s_state    = SCALE_STATE_READY;
}

void Scale_Sleep(void)
{
  ADS1220_PowerDown(&s_adc);
}

void Scale_Wakeup(void)
{
  /* Re-init the ADC (registers may have been lost during the power-down),
     then take a few samples so the filters start from current reality.
     The tare zero (s_zeroRaw) is deliberately kept: waking up must not
     re-zero a scale that still has a load on it. */
  ADS1220_Init(&s_adc, &s_params);
  Scale_Warmup(5u);
  s_lastFilt = s_rawFilt;
  s_faultCnt = 0; s_stableCnt = 0;
  s_state    = SCALE_STATE_READY;
}

void Scale_Tare(void)
{
  s_zeroRaw  = s_rawFilt;
  s_stableCnt = 0;
}

void Scale_Update(void)
{
  int32_t raw;
  int32_t delta;
  uint32_t timeout;

  /* single-shot mode: start a conversion and wait for DRDY# (20SPS ~50ms) */
  ADS1220_StartSync(&s_adc);
  timeout = 60u;
  while (ADC_DRDY_Read() != 0u && (timeout-- > 0u)) { Delay_ms(1); }
  if (ADC_DRDY_Read() != 0u)
  {
    return;    /* no conversion finished, keep previous value */
  }

  ADS1220_ReadData(&s_adc, &raw);

  /* fault: out-of-range raw. Lower bound is slightly negative so that a
     small zero offset / noise around 0V does not trip the fault path
     (ADS1220 single-ended output sits near 0 code at 0V input). */
  if ((raw <= -0x8000) || ((uint32_t)raw >= FAULT_RAW_MAX))
  {
    if (s_faultCnt < 255u) { s_faultCnt++; }
    if (s_faultCnt >= 3u)
    {
      s_state = SCALE_STATE_FAULT;
    }
    return;
  }
  s_faultCnt = 0;

  s_rawFilt = Scale_LowPass(Scale_AverageFilter(Scale_MedianFilter(raw)));

  /* stability: change below threshold for N consecutive updates */
  delta = s_rawFilt - s_lastFilt;
  if (delta < 0) { delta = -delta; }
  if (delta <= STABLE_LSB)
  {
    if (s_stableCnt < STABLE_CNT) { s_stableCnt++; }
  }
  else
  {
    s_stableCnt = 0;
  }
  s_lastFilt = s_rawFilt;

  if (Scale_GetWeight() > SCALE_OVERLOAD_G)
  {
    s_state = SCALE_STATE_OVERLOAD;
  }
  else
  {
    s_state = (s_stableCnt >= STABLE_CNT) ? SCALE_STATE_STABLE : SCALE_STATE_READY;
  }
}

float Scale_GetWeight(void)
{
  return (float)(s_rawFilt - s_zeroRaw) * SCALE_LSB_TO_G;
}

int32_t Scale_GetRaw(void)         { return s_rawFilt; }
int32_t Scale_GetRawFiltered(void) { return s_rawFilt; }
Scale_State_t Scale_GetState(void) { return s_state; }
