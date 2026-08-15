/**
 **********************************************************************************
 * @file   app.c
 * @brief  Application main loop: read scale, show on OLED, report over UART.
 *
 * UART protocol (matches platform/GUI host program):
 *   one line per record: "weight,temp\n"  (weight in g, 2 decimals;
 *   temp in degC, 1 decimal; baud 115200, ~20Hz)
 *
 * Keys:
 *   KEY2 (PB5, "去皮") = tare
 *   KEY1 (PB4, "开关") = reserved
 **********************************************************************************
 **/
#include "app.h"
#include "oled.h"
#include "delay.h"
#include "usart.h"
#include "scale.h"
#include "ds18b20.h"

#define APP_LOOP_MS        5u       /* loop fast; sampling rate set by ADS1220 DRDY (~20Hz) */
#define TEMP_CONVERT_MS    750u     /* DS18B20 12-bit conversion time */
#define DISPLAY_EVERY_N    40u      /* refresh OLED every ~200ms */

/* ------------------------------------------------------------------ */
/*  Integer -> string (returns number of chars written)                */
/* ------------------------------------------------------------------ */
static uint8_t App_itoa(char *out, uint32_t val)
{
  uint8_t n = 0;
  uint8_t len;
  char tmp[10];
  if (val == 0u) { out[0] = '0'; return 1u; }
  while (val > 0u)
  {
    tmp[n++] = (char)('0' + (val % 10));
    val /= 10;
  }
  len = n;
  while (n > 0u)
  {
    *out++ = tmp[--n];
  }
  return len;
}

/* ------------------------------------------------------------------ */
/*  Temperature task (non-blocking)                                    */
/* ------------------------------------------------------------------ */
static float    s_tempC = 25.0f;
static uint8_t  s_tempState;    /* 0=idle, 1=waiting conversion */
static uint32_t s_tempCnt;

/* read the conversion result (call after DS18B20_Start + delay) */
static int16_t Read_Temp_Result(void)
{
  uint8_t TL, TH;
  int16_t tem;
  uint8_t neg = 0;

  DS18B20_Rst();
  if (DS18B20_Check())
  {
    return 0;                    /* sensor absent */
  }
  DS18B20_Write_Byte(0xCC);      /* skip ROM */
  DS18B20_Write_Byte(0xBE);      /* read scratchpad */
  TL = DS18B20_Read_Byte();
  TH = DS18B20_Read_Byte();

  if (TH > 7u)
  {
    TH = (uint8_t)~TH;
    TL = (uint8_t)~TL;
    neg = 1;
  }
  tem = (int16_t)((TH << 8) | TL);
  tem = (int16_t)((float)tem * 0.625f);
  return neg ? (int16_t)-tem : tem;
}

static void App_TempTask(void)
{
  if (s_tempState == 0u)
  {
    DS18B20_Start();             /* start conversion, fast (a few ms) */
    s_tempState = 1u;
    s_tempCnt = 0u;
  }
  else if (s_tempState == 1u)
  {
    if (++s_tempCnt >= (TEMP_CONVERT_MS / APP_LOOP_MS))
    {
      s_tempC = (float)Read_Temp_Result() * 0.1f;
      s_tempState = 0u;
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Keys                                                               */
/* ------------------------------------------------------------------ */
static uint8_t s_mode = 0u;   /* 0 = normal, 1 = device/calibration */

static void App_Key(void)
{
  static uint8_t lastK2 = 0u;
  static uint32_t k1Hold = 0u;
  static uint8_t  k1Toggled = 0u;
  uint8_t k1, k2;

  k2 = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5) == GPIO_PIN_SET) ? 1u : 0u;
  k1 = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4) == GPIO_PIN_SET) ? 1u : 0u;

  /* KEY2 = tare on press */
  if (k2 && !lastK2)
  {
    Scale_Tare();
  }
  lastK2 = k2;

  /* KEY1 = long press (>2s) toggles device/calibration mode */
  if (k1)
  {
    if (k1Hold < 0xFFFFFFFFu) { k1Hold++; }
    if ((k1Hold >= (2000u / APP_LOOP_MS)) && !k1Toggled)
    {
      k1Toggled = 1u;
      s_mode ^= 1u;
    }
  }
  else
  {
    k1Hold = 0u;
    k1Toggled = 0u;
  }
}

/* ------------------------------------------------------------------ */
/*  OLED display                                                       */
/* ------------------------------------------------------------------ */
static void App_Display(void)
{
  float w = Scale_GetWeight();
  Scale_State_t st = Scale_GetState();
  int32_t w10;
  char s[10];
  uint8_t n = 0;
  uint8_t x, i;

  OLED_Clear();

  if (s_mode == 1u)
  {
    /* device/calibration mode: show raw ADC value + temp */
    int32_t raw = Scale_GetRaw();
    OLED_ShowString(0, 0, (const uint8_t *)"CAL", 16, 1);
    if (raw < 0) { s[n++] = '-'; raw = -raw; }
    n += App_itoa(s + n, (uint32_t)raw);
    s[n] = 0;
    x = (uint8_t)((128u - (uint32_t)n * 12u) / 2u);
    for (i = 0; i < n; i++)
    {
      OLED_ShowChar(x, 20, s[i], 24, 1);
      x = (uint8_t)(x + 12u);
    }
    OLED_ShowString(0, 52, (const uint8_t *)"T:", 8, 1);
    {
      int32_t t10 = (int32_t)(s_tempC * 10.0f + ((s_tempC >= 0.0f) ? 0.5f : -0.5f));
      char tb[6];
      uint8_t m = 0;
      if (t10 < 0) { tb[m++] = '-'; t10 = -t10; }
      m += App_itoa(tb + m, (uint32_t)(t10 / 10));
      tb[m++] = '.';
      tb[m++] = (char)('0' + t10 % 10);
      tb[m] = 0;
      OLED_ShowString(18, 52, (const uint8_t *)tb, 8, 1);
    }
    OLED_ShowString(44, 52, (const uint8_t *)"C", 8, 1);
    OLED_Refresh();
    return;
  }

  /* weight, one decimal, 24x12 font, centered */
  w10 = (int32_t)(w * 10.0f + ((w >= 0.0f) ? 0.5f : -0.5f));
  if (w10 < 0) { s[n++] = '-'; w10 = -w10; }
  n += App_itoa(s + n, (uint32_t)(w10 / 10));
  s[n++] = '.';
  s[n++] = (char)('0' + w10 % 10);
  s[n] = 0;
  x = (uint8_t)((128u - (uint32_t)n * 12u) / 2u);
  for (i = 0; i < n; i++)
  {
    OLED_ShowChar(x, 4, s[i], 24, 1);
    x = (uint8_t)(x + 12u);
  }

  /* status + temp line (8x6 font) */
  switch (st)
  {
    case SCALE_STATE_STABLE:   OLED_ShowString(0, 32, (const uint8_t *)"STABLE", 8, 1); break;
    case SCALE_STATE_OVERLOAD: OLED_ShowString(0, 32, (const uint8_t *)"OVERLOAD", 8, 1); break;
    case SCALE_STATE_FAULT:    OLED_ShowString(0, 32, (const uint8_t *)"FAULT", 8, 1); break;
    default:                   OLED_ShowString(0, 32, (const uint8_t *)"READY", 8, 1); break;
  }

  OLED_ShowString(60, 32, (const uint8_t *)"T:", 8, 1);
  {
    int32_t t10 = (int32_t)(s_tempC * 10.0f + ((s_tempC >= 0.0f) ? 0.5f : -0.5f));
    char tb[6];
    uint8_t m = 0;
    if (t10 < 0) { tb[m++] = '-'; t10 = -t10; }
    m += App_itoa(tb + m, (uint32_t)(t10 / 10));
    tb[m++] = '.';
    tb[m++] = (char)('0' + t10 % 10);
    tb[m] = 0;
    OLED_ShowString(78, 32, (const uint8_t *)tb, 8, 1);
  }
  OLED_ShowString(104, 32, (const uint8_t *)"C", 8, 1);

  OLED_Refresh();
}

/* ------------------------------------------------------------------ */
/*  UART report: "weight,temp\n"                                       */
/* ------------------------------------------------------------------ */
static void App_UartSend(void)
{
  float w = Scale_GetWeight();
  int32_t t10  = (int32_t)(s_tempC * 10.0f + ((s_tempC >= 0.0f) ? 0.5f : -0.5f));
  char buf[24];
  uint8_t len = 0;

  if (s_mode == 1u)
  {
    /* device/calibration mode: raw,temp\n (raw ADC value for the host) */
    int32_t raw = Scale_GetRaw();
    if (raw < 0) { buf[len++] = '-'; raw = -raw; }
    len += App_itoa(buf + len, (uint32_t)raw);
    buf[len++] = ',';
    if (t10 < 0) { buf[len++] = '-'; t10 = -t10; }
    len += App_itoa(buf + len, (uint32_t)(t10 / 10));
    buf[len++] = '.';
    buf[len++] = (char)('0' + t10 % 10);
    buf[len++] = '\n';
  }
  else
  {
    /* normal mode: weight,temp\n */
    int32_t w100 = (int32_t)(w * 100.0f + ((w >= 0.0f) ? 0.5f : -0.5f));
    if (w100 < 0) { buf[len++] = '-'; w100 = -w100; }
    len += App_itoa(buf + len, (uint32_t)(w100 / 100));
    buf[len++] = '.';
    buf[len++] = (char)('0' + (w100 / 10) % 10);
    buf[len++] = (char)('0' + w100 % 10);
    buf[len++] = ',';

    if (t10 < 0) { buf[len++] = '-'; t10 = -t10; }
    len += App_itoa(buf + len, (uint32_t)(t10 / 10));
    buf[len++] = '.';
    buf[len++] = (char)('0' + t10 % 10);
    buf[len++] = '\n';
  }

  HAL_UART_Transmit(&huart1, (uint8_t *)buf, len, 50);
}

/* ------------------------------------------------------------------ */
/*  Public                                                             */
/* ------------------------------------------------------------------ */
void App_Init(void)
{
  OLED_Init();
  Scale_Init();

  OLED_Clear();
  OLED_ShowString(0, 0, (const uint8_t *)"Scale v1.0", 16, 1);
  OLED_Refresh();
  Delay_ms(600);
}

void App_Loop(void)
{
  static uint32_t loopCnt = 0u;

  Scale_Update();
  App_TempTask();
  App_Key();
  App_UartSend();

  loopCnt++;
  if ((loopCnt % DISPLAY_EVERY_N) == 0u)
  {
    App_Display();
  }

  Delay_ms(APP_LOOP_MS);
}
