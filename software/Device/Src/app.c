/**
 **********************************************************************************
 * @file   app.c
 * @brief  Application main loop: read scale, show on OLED, report over UART.
 *
 * UART protocol (matches platform/GUI host program):
 *   one line per record: "weight,temp\n"  (weight in g, 2 decimals;
 *   temp in degC, 1 decimal; baud 9600 (JDY-31 default, see .ioc), ~20Hz)
 *
 * Keys:
 *   KEY2 (PB5, "去皮") = tare on press
 *   KEY1 (PB4, "开关") = long press (>2s) toggles device status mode
 *
 * Low power:
 *   Auto-sleep after AUTO_SLEEP_MS without key activity (STOP mode).
 *   Wake-up on any key press (PB4/PB5 EXTI rising edge, see App_Init).
 *   NOTE: OLED keeps its supply; OLED_DisPlay_Off() is used to cut the
 *   display current while sleeping (hardware power switch would be better).
 **********************************************************************************
 **/
#include "app.h"
#include "oled.h"
#include "delay.h"
#include "usart.h"
#include "scale.h"
#include "ds18b20.h"

#define APP_LOOP_MS        5u       /* loop delay only; scheduling uses HAL_GetTick */
#define TEMP_CONVERT_MS    750u     /* DS18B20 12-bit conversion time */
#define AUTO_SLEEP_MS      60000u   /* no-key timeout before auto sleep (60s) */

/* defined in main.c (CubeMX-generated); re-run it after STOP wake-up to
   restore the 72MHz PLL clock (STOP mode falls back to HSI) */
extern void SystemClock_Config(void);

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

/* Format a fixed-point value as decimal string with 1 fractional digit
   (v10 = value * 10), e.g. 256 -> "25.6", -32 -> "-3.2". NUL-terminated. */
static uint8_t App_Fmt1(char *out, int32_t v10)
{
  uint8_t n = 0;
  if (v10 < 0) { out[n++] = '-'; v10 = -v10; }
  n += App_itoa(out + n, (uint32_t)(v10 / 10));
  out[n++] = '.';
  out[n++] = (char)('0' + v10 % 10);
  out[n] = 0;
  return n;
}

/* Same as App_Fmt1 but with 2 fractional digits (v100 = value * 100). */
static uint8_t App_Fmt2(char *out, int32_t v100)
{
  uint8_t n = 0;
  if (v100 < 0) { out[n++] = '-'; v100 = -v100; }
  n += App_itoa(out + n, (uint32_t)(v100 / 100));
  out[n++] = '.';
  out[n++] = (char)('0' + (v100 / 10) % 10);
  out[n++] = (char)('0' + v100 % 10);
  out[n] = 0;
  return n;
}

/* ------------------------------------------------------------------ */
/*  Temperature task (non-blocking)                                    */
/* ------------------------------------------------------------------ */
static float    s_tempC = 25.0f;
static uint8_t  s_tempState;    /* 0=idle, 1=waiting conversion */
static uint32_t s_tempDue;      /* absolute tick when conversion is done */

/* Current temperature as fixed-point (degC * 10), rounded to nearest. */
static int32_t App_Temp10(void)
{
  return (int32_t)(s_tempC * 10.0f + ((s_tempC >= 0.0f) ? 0.5f : -0.5f));
}

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
    s_tempDue = HAL_GetTick() + TEMP_CONVERT_MS;
  }
  else if (s_tempState == 1u)
  {
    if ((int32_t)(HAL_GetTick() - s_tempDue) >= 0)
    {
      s_tempC = (float)Read_Temp_Result() * 0.1f;
      s_tempState = 0u;
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Keys + auto-sleep                                                  */
/* ------------------------------------------------------------------ */
static uint8_t s_mode = 0u;   /* 0 = normal, 1 = device/calibration */

/* key state (file scope so App_Sleep/App_Wakeup can sync it) */
static uint8_t  s_lastK2 = 0u;
static uint8_t  s_k1Pressed = 0u;
static uint32_t s_k1Start = 0u;
static uint8_t  s_k1Toggled = 0u;
static uint32_t s_lastAct = 0u;   /* tick of last key activity */
static uint8_t  s_wakeSync = 0u;  /* set after wake-up to ignore the wake key */

/* scheduling timestamps (file scope so App_Wakeup can reset them) */
static uint32_t s_lastUart = 0u;
static uint32_t s_lastDisp = 0u;

static void App_Key(void)
{
  uint8_t k1, k2;

  k2 = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5) == GPIO_PIN_SET) ? 1u : 0u;
  k1 = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4) == GPIO_PIN_SET) ? 1u : 0u;

  /* right after wake-up the wake-up key may still be held: consume that
     press entirely - it must neither tare (KEY2) nor, if held >2s, toggle
     the mode (KEY1). s_k1Toggled is pre-set so a held KEY1 cannot switch
     screens; it re-arms once the key is released (else branch below). */
  if (s_wakeSync)
  {
    s_lastK2 = k2;
    s_k1Pressed = k1 ? 1u : 0u;
    s_k1Toggled = k1 ? 1u : 0u;
    s_k1Start = HAL_GetTick();
    s_lastAct = HAL_GetTick();   /* the wake-up press counts as activity */
    s_wakeSync = 0u;
  }

  /* KEY2 = tare on press */
  if (k2 && !s_lastK2)
  {
    Scale_Tare();
    s_lastAct = HAL_GetTick();
  }
  s_lastK2 = k2;

  /* KEY1 = long press (>2s) toggles device/calibration mode.
     Timed with HAL_GetTick (loop period varies due to ADC DRDY wait). */
  if (k1)
  {
    if (!s_k1Pressed)
    {
      s_k1Pressed = 1u;
      s_k1Start = HAL_GetTick();
      s_lastAct = HAL_GetTick();
    }
    else if (((uint32_t)(HAL_GetTick() - s_k1Start) >= 2000u) && !s_k1Toggled)
    {
      s_k1Toggled = 1u;
      s_mode ^= 1u;
    }
  }
  else
  {
    s_k1Pressed = 0u;
    s_k1Toggled = 0u;
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
    /* device status mode: peripheral status screen (like the self-test) */
    int32_t raw = Scale_GetRaw();
    char rb[12];
    uint8_t rn = 0;
    uint8_t k1d, k2d;

    OLED_ShowString(0, 0, (const uint8_t *)"Periph Test", 8, 1);

    OLED_ShowString(0, 8, (const uint8_t *)"ADS1220:", 8, 1);
    if (raw < 0) { rb[rn++] = '-'; raw = -raw; }  /* sign-aware raw code */
    rn += App_itoa(rb + rn, (uint32_t)raw);
    rb[rn] = 0;
    OLED_ShowString(48, 8, (const uint8_t *)rb, 8, 1);

    OLED_ShowChinese(0, 16, 3, 16, 1);    /* 温 */
    OLED_ShowChinese(16, 16, 4, 16, 1);   /* 度 */
    {
      char tb[8];   /* 8: room for "-125.0" (6 chars) + NUL */
      App_Fmt1(tb, App_Temp10());
      OLED_ShowString(36, 22, (const uint8_t *)tb, 8, 1);
    }
    OLED_ShowChinese(72, 16, 0, 16, 1);   /* ℃ (x=72: clear of "-25.6" ending at 66) */

    OLED_ShowString(0, 32, (const uint8_t *)"UART:", 8, 1);
    OLED_ShowNum(48, 32, huart1.Init.BaudRate, 5, 8, 1);

    k2d = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5) == GPIO_PIN_SET) ? 1u : 0u;
    k1d = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4) == GPIO_PIN_SET) ? 1u : 0u;
    OLED_ShowString(0, 40, (const uint8_t *)"K1:", 8, 1);
    OLED_ShowNum(24, 40, k1d, 1, 8, 1);
    OLED_ShowString(40, 40, (const uint8_t *)"K2:", 8, 1);
    OLED_ShowNum(64, 40, k2d, 1, 8, 1);

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

  /* temp line (16x16 Chinese from oledfont.h): "温度 25.6℃" */
  OLED_ShowChinese(0, 30, 3, 16, 1);     /* 温 */
  OLED_ShowChinese(16, 30, 4, 16, 1);    /* 度 */
  {
    char tb[8];   /* 8: room for "-125.0" (6 chars) + NUL */
    App_Fmt1(tb, App_Temp10());
    OLED_ShowString(36, 30, (const uint8_t *)tb, 16, 1);
  }
  OLED_ShowChinese(80, 30, 0, 16, 1);    /* ℃ (x=80: clear of "-10.5" ending at 76) */

  /* status line */
  switch (st)
  {
    case SCALE_STATE_STABLE:   OLED_ShowString(0, 52, (const uint8_t *)"STABLE", 8, 1); break;
    case SCALE_STATE_OVERLOAD: OLED_ShowString(0, 52, (const uint8_t *)"OVERLOAD", 8, 1); break;
    case SCALE_STATE_FAULT:    OLED_ShowString(0, 52, (const uint8_t *)"FAULT", 8, 1); break;
    default:                   OLED_ShowString(0, 52, (const uint8_t *)"READY", 8, 1); break;
  }

  OLED_Refresh();
}

/* ------------------------------------------------------------------ */
/*  UART report: "weight,temp\n"                                       */
/* ------------------------------------------------------------------ */
static void App_UartSend(void)
{
  float w = Scale_GetWeight();
  int32_t t10  = App_Temp10();
  char buf[24];
  uint8_t len = 0;

  if (s_mode == 1u)
  {
    /* device/calibration mode: raw,temp\n (raw ADC value for the host) */
    int32_t raw = Scale_GetRaw();
    if (raw < 0) { buf[len++] = '-'; raw = -raw; }
    len += App_itoa(buf + len, (uint32_t)raw);
    buf[len++] = ',';
    len += App_Fmt1(buf + len, t10);
    buf[len++] = '\n';
  }
  else
  {
    /* normal mode: weight,temp\n */
    int32_t w100 = (int32_t)(w * 100.0f + ((w >= 0.0f) ? 0.5f : -0.5f));
    len += App_Fmt2(buf + len, w100);
    buf[len++] = ',';
    len += App_Fmt1(buf + len, t10);
    buf[len++] = '\n';
  }

  if (HAL_UART_Transmit(&huart1, (uint8_t *)buf, len, 50) != HAL_OK)
  {
    /* send failed (busy/timeout): clear the sticky error flags so the
       next report can go out; this report is simply skipped */
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    __HAL_UART_CLEAR_PEFLAG(&huart1);
    __HAL_UART_CLEAR_FEFLAG(&huart1);
    __HAL_UART_CLEAR_NEFLAG(&huart1);
  }
}

/* ------------------------------------------------------------------ */
/*  Low power: auto sleep + wake-up                                    */
/* ------------------------------------------------------------------ */
static void App_Wakeup(void);

static void App_Sleep(void)
{
  /* brief hint, then power down */
  OLED_Clear();
  OLED_ShowString(0, 28, (const uint8_t *)"Sleep...", 16, 1);
  OLED_Refresh();
  Delay_ms(800);

  Scale_Sleep();          /* ADS1220 power down */
  OLED_DisPlay_Off();     /* SSD1306 sleep mode cuts most OLED current */
  Delay_Deinit();         /* stop TIM2 so it re-inits after wake-up */

  /* STOP mode; wake-up source: KEY1/KEY2 rising edge (PB4/PB5 EXTI,
     configured as GPIO_MODE_IT_RISING in gpio.c, NVIC in App_Init).
     HAL_SuspendTick() is REQUIRED: SysTick fires every 1ms, so without
     suspending it the __WFI() inside HAL_PWR_EnterSTOPMode is woken
     immediately by a pending SysTick -> the MCU never really sleeps. */
  HAL_SuspendTick();
  HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);

  /* --- execution resumes here after wake-up --- */
  App_Wakeup();
}

static void App_Wakeup(void)
{
  HAL_ResumeTick();       /* re-enable SysTick (suspended before STOP) */
  SystemClock_Config();   /* STOP falls back to HSI: restore 72MHz PLL */

  OLED_Init();            /* re-init + display on (was powered off) */
  Scale_Wakeup();         /* re-init ADC, quick warm-up, keeps tare zero */
  s_tempState = 0u;       /* restart the temperature cycle */

  s_lastAct  = HAL_GetTick();
  s_lastUart = 0u;        /* force an immediate UART report */
  s_lastDisp = 0u;
  s_wakeSync = 1u;        /* the wake-up key must not trigger actions */
}

/* ------------------------------------------------------------------ */
/*  Public                                                             */
/* ------------------------------------------------------------------ */

/* Debugger attached? C_DEBUGEN is set by the SWD probe while connected.
   When a debugger is attached we must NOT enter STOP mode: a stopped
   core cannot be halted by the probe (Keil: "Could not stop Cortex-M
   device"), so auto-sleep is skipped during debug sessions. */
static uint8_t App_DebuggerAttached(void)
{
  return (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) ? 1u : 0u;
}

void App_Init(void)
{
  OLED_Init();
  Scale_Init();

  /* KEY1/KEY2 EXTI wake-up from STOP mode. The pins are already
     configured as GPIO_MODE_IT_RISING by gpio.c; the ISRs live in
     stm32f1xx_it.c (USER CODE section). Without NVIC enabled the EXTI
     would never fire (and the device could not wake up). */
  HAL_NVIC_SetPriority(EXTI4_IRQn, 2u, 0u);
  HAL_NVIC_EnableIRQ(EXTI4_IRQn);
  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 2u, 0u);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

  OLED_Clear();
  OLED_ShowChinese(48, 0, 1, 16, 1);     /* 重 */
  OLED_ShowChinese(64, 0, 2, 16, 1);     /* 量 */
  OLED_ShowString(0, 24, (const uint8_t *)"Scale v1.0", 16, 1);
  OLED_Refresh();
  Delay_ms(600);

  s_lastAct = HAL_GetTick();   /* start the auto-sleep timeout from now */
}

void App_Loop(void)
{
  uint32_t now;

  Scale_Update();
  App_TempTask();
  App_Key();

  now = HAL_GetTick();
  if ((uint32_t)(now - s_lastUart) >= 50u)   /* ~20Hz report */
  {
    App_UartSend();
    s_lastUart = now;
  }
  if ((uint32_t)(now - s_lastDisp) >= 200u)  /* ~5Hz display refresh */
  {
    App_Display();
    s_lastDisp = now;
  }

  /* auto sleep: no key activity for AUTO_SLEEP_MS. Skipped while a
     debugger is attached - a core in STOP mode cannot be halted by the
     SWD probe ("Could not stop Cortex-M device" in Keil). */
  if (!App_DebuggerAttached() && ((uint32_t)(now - s_lastAct) >= AUTO_SLEEP_MS))
  {
    App_Sleep();            /* blocks until wake-up */
    now = HAL_GetTick();    /* tick jumped; re-sync local copy */
  }

  Delay_ms(APP_LOOP_MS);
}
