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
#define STABLE_LSB        400       /* ~0.33g at 0.00084g/LSB calibrated */
#define STABLE_CNT        10

/* Zero-drift tracking (auto-zero): on an unloaded scale, slowly pull the
   tare zero toward the current reading so that thermal / creep / supply
   drift does not accumulate on the display. Tracking stops as soon as a
   real load above AUTO_ZERO_G is applied.
   Rates: with DIV=4 the display absorbs drift up to ~1.5 g/s (steady-state
   error = drift * 0.2s, must stay below AUTO_ZERO_G); loads >= 0.3 g are
   never absorbed. Tune these two macros to the measured drift rate. */
#define AUTO_ZERO_G       0.3f      /* |weight| below this -> absorb drift (g) */
#define AUTO_ZERO_DIV     4         /* tracking rate: 1/4 per sample (~0.2s tau) */

/* Load-step freeze: when the filtered raw jumps faster than a real drift
   could (>= AZ_STEP_RATE LSB/sample, ~5 g placed), a load is being placed
   or removed. Auto-zero is then frozen for AZ_FREEZE_MS so the tare zero
   is not nudged while the filtered reading transitions through the
   < 0.3 g window (otherwise ~0.1 g of the load would be silently eaten). */
#define AZ_STEP_RATE      200       /* per-sample raw jump = load step (drift max ~30) */
#define AZ_FREEZE_MS      2000u     /* hold auto-zero off after a step */

/* Power-on soak (fully adaptive): after power-on the zero drifts
   exponentially, but the direction, amplitude and rate vary every
   power-on (measured +6 g one boot, -16 g the next) - far above the
   normal auto-zero window (0.3 g), which is why the display visibly
   climbs. While soaking, the zero unconditionally follows the reading
   (any drift shape is absorbed, the display holds at 0). The soak ends
   when the REAL drift rate (rawFilt per-sample change) has been below
   SOAK_SETTLE_RATE for SOAK_SETTLE_MS - i.e. the drift is over and the
   normal slow auto-zero can safely take over - or after SOAK_MAX_MS
   (safety net). TRADEOFF: anything placed on the scale during the soak
   is absorbed to zero - the warm-up phase is not a measuring phase. */
#define SOAK_SETTLE_RATE  30        /* raw drift < 30 LSB/sample (~0.5 g/s) -> over */
#define SOAK_SETTLE_MS    5000u     /* drift-rate low for this long -> hand over */
#define SOAK_MAX_MS       60000u    /* safety net: force end after 60 s */

static Scale_State_t s_state;
static int32_t s_zeroRaw;      /* tare offset (raw) */
static int32_t s_rawFilt;      /* filtered raw */
static int32_t s_lastFilt;
static uint8_t s_faultCnt;
static uint8_t s_stableCnt;
static uint32_t s_soakT0;      /* power-on soak start tick */
static uint8_t  s_soaking;     /* 1 = soak active */
static uint32_t s_soakSettleT0;/* tick when the raw drift rate went low */
static int32_t  s_prevFilt;    /* previous filtered raw, for the drift-rate check */

/* Temperature compensation: the bridge zero drifts with temperature
   (fitted -43 raw LSB per degC from the 15-min capture). Compensation is
   RELATIVE: the reference is the temperature at tare (and at the first
   real reading after power-on), so a tare always shows 0 and only the
   temperature change afterwards is compensated. */
#define TEMP_COEF_LSB_PER_C  (-43)      /* raw LSB per degC (measured) */

/* Full-range nonlinearity correction (fit from the 0-500 g capture:
   true = A*w + B*w^2 with w = raw*LSB display value; residual < 0.25 g
   over 50-500 g, hysteresis ~0.4 g not correctable in software). */
#define SCALE_NL_A          1.0008f
#define SCALE_NL_B          7.23e-6f
static float   s_tempC = 25.0f;         /* current temperature (degC) */
static float   s_tempRef = 25.0f;       /* tare-time temperature (degC) */
static uint8_t s_tempRefInit = 0u;      /* first real reading sets the reference */
static uint32_t s_azFreezeUntil = 0u;   /* auto-zero frozen until this tick */

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Take `samples` successful single-shot conversions (StartSync + wait
   DRDY#) and feed them through the filter chain, updating s_rawFilt.
   Timed-out conversions are simply retried (bounded), never fed as 0. */
static void Scale_Warmup(uint8_t samples)
{
  uint8_t got = 0u;
  uint16_t tries = 0u;   /* uint16: samples*3 can exceed uint8 range */
  while (got < samples && tries < (uint16_t)(samples * 3u))
  {
    uint32_t timeout = 60u;
    int32_t raw = 0;
    tries++;
    ADS1220_StartSync(&s_adc);
    while (ADC_DRDY_Read() != 0u && (timeout-- > 0u)) { Delay_ms(1); }
    if (ADC_DRDY_Read() == 0u)
    {
      ADS1220_ReadData(&s_adc, &raw);
      s_rawFilt = Scale_LowPass(Scale_AverageFilter(Scale_MedianFilter(raw)));
      got++;
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
  s_soakT0 = HAL_GetTick();   /* power-on soak starts now */
  s_soaking = 1u;
  s_soakSettleT0 = 0u;
  s_state = SCALE_STATE_INIT;

  /* warm-up a few samples, then auto-tare at power-up (empty platform).
     Single-shot: StartSync then wait DRDY# (max ~60ms per sample). */
  Scale_Warmup(MEDIAN_N + AVG_N);
  s_zeroRaw  = s_rawFilt;
  s_lastFilt = s_rawFilt;
  s_prevFilt = s_rawFilt;
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
  s_tempRef  = s_tempC;     /* temperature compensation is relative to tare */
  s_stableCnt = 0;
}

/* Feed the current temperature (from the app's DS18B20 task). The first
   real reading after power-on also sets the compensation reference, so
   the display starts near 0 without a tare. */
void Scale_SetTempC(float temp_c)
{
  if (!s_tempRefInit)
  {
    s_tempRef = temp_c;
    s_tempRefInit = 1u;
  }
  s_tempC = temp_c;
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
     (ADS1220 single-ended output sits near 0 code at 0V input).
     NOTE: both comparisons are SIGNED - casting a negative raw to
     uint32_t here would misclassify it as "too large" and drop valid
     samples, biasing the filters upward. */
  if ((raw < -0x8000) || (raw > (int32_t)FAULT_RAW_MAX))
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

  /* zero-drift tracking (auto-zero): on an unloaded scale the tare zero is
     slowly pulled toward the reading, so thermal / creep / supply drift
     does not accumulate on the display (the "weight keeps climbing"
     symptom). As soon as a real load above AUTO_ZERO_G is present the
     tracking stops, so genuine weight is never absorbed.
     During the power-on soak the zero unconditionally follows the reading
     (drift direction/amplitude/rate vary every power-on, so no fixed
     gates can separate it from a load); the soak ends when the raw drift
     RATE has been low for SOAK_SETTLE_MS (the drift is really over) or
     after SOAK_MAX_MS. TRADEOFF: anything placed on the scale during the
     soak is absorbed - the warm-up phase is not a measuring phase.
     s_stableCnt is cleared only when the zero actually moved (step != 0),
     otherwise a settled unloaded scale could never reach STABLE. */
  if ((s_state != SCALE_STATE_FAULT) && (s_state != SCALE_STATE_OVERLOAD))
  {
    float w = Scale_GetWeight();
    int32_t delta = s_rawFilt - s_zeroRaw;
    int32_t rawRate = s_rawFilt - s_prevFilt;   /* real drift rate this sample */
    s_prevFilt = s_rawFilt;
    if ((rawRate > AZ_STEP_RATE) || (rawRate < -AZ_STEP_RATE))
    {
      s_azFreezeUntil = HAL_GetTick() + AZ_FREEZE_MS;   /* load placed/removed */
    }
    if (s_soaking)
    {
      if ((int32_t)(HAL_GetTick() - s_soakT0) >= (int32_t)SOAK_MAX_MS)
      {
        s_soaking = 0u;   /* safety net */
      }
      else if ((rawRate > -SOAK_SETTLE_RATE) && (rawRate < SOAK_SETTLE_RATE))
      {
        if (s_soakSettleT0 == 0u) { s_soakSettleT0 = HAL_GetTick(); }
        else if ((int32_t)(HAL_GetTick() - s_soakSettleT0) >= (int32_t)SOAK_SETTLE_MS)
        {
          s_soaking = 0u;   /* drift over: safe hand-over to normal auto-zero */
        }
      }
      else
      {
        s_soakSettleT0 = 0u;   /* drift still active */
      }
      if (s_soaking && (delta != 0))
      {
        s_zeroRaw += delta;   /* unconditional full absorption */
        s_stableCnt = 0u;     /* zero moved: not a settled reading */
      }
    }
    else if ((int32_t)(HAL_GetTick() - s_azFreezeUntil) < 0)
    {
      /* frozen after a load step: leave the tare zero untouched */
    }
    else if ((w > -AUTO_ZERO_G) && (w < AUTO_ZERO_G))
    {
      int32_t step = delta / AUTO_ZERO_DIV;
      if (step != 0)
      {
        s_zeroRaw += step;
        s_stableCnt = 0u;   /* zero moved: not a settled reading */
      }
    }
  }
}

float Scale_GetWeight(void)
{
  float raw = (float)(s_rawFilt - s_zeroRaw);
  float w;
  raw -= (float)TEMP_COEF_LSB_PER_C * (s_tempC - s_tempRef);  /* temperature comp. */
  w = raw * SCALE_LSB_TO_G;
  /* full-range nonlinearity correction: map display value to true weight */
  w = SCALE_NL_A * w + SCALE_NL_B * w * w;
  return w;
}

int32_t Scale_GetRaw(void)         { return s_rawFilt; }
int32_t Scale_GetRawFiltered(void) { return s_rawFilt; }
Scale_State_t Scale_GetState(void) { return s_state; }
