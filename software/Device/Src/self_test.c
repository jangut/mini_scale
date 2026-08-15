/**
 **********************************************************************************
 * @file   self_test.c
 * @brief  Peripheral self-test for mini_scale hardware
 *
 * Pin map (from netlist file 网表文件.txt):
 *   OLED   : SCL=PB6, SDA=PB7 (software I2C, 4-wire module, no RES)
 *   ADS1220: CS=PA3, SCLK=PA4, DRDY=PA5, MISO=PA6, MOSI=PA7 (software SPI)
 *   Temp   : PA1 (ADC1_IN1)
 *   JDY-31 : USART1 TX=PA9, RX=PA10
 *   KEY1   : PB4 (active high, 10k pulldown)
 *   KEY2   : PB5 (active high, 10k pulldown)
 **********************************************************************************
 **/
#include "self_test.h"
#include "oled.h"
#include "delay.h"
#include "usart.h"
#include "ds18b20.h"
#include "ADS1220.h"

/* ------------------------------------------------------------------ */
/*  ADS1220 software SPI low-level callbacks                           */
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

static ADS1220_Handler_t    s_adcHandler;
static ADS1220_Parameters_t s_adcParams;

static void ADC_CS_HIGH(void)
{
  HAL_GPIO_WritePin(ADC_CS_PORT, ADC_CS_PIN, GPIO_PIN_SET);
}

static void ADC_CS_LOW(void)
{
  HAL_GPIO_WritePin(ADC_CS_PORT, ADC_CS_PIN, GPIO_PIN_RESET);
}

/* DRDY# is low when data is ready: return 1 = busy, 0 = ready */
static uint8_t ADC_DRDY_Read(void)
{
  return (HAL_GPIO_ReadPin(ADC_DRDY_PORT, ADC_DRDY_PIN) == GPIO_PIN_SET) ? 1u : 0u;
}

static void ADC_Delay_US(uint32_t us)
{
  Delay_us(us);
}

/* one 8-bit SPI shift: CPOL=0, CPHA=1 (sample on rising edge) */
static uint8_t SPI_Shift(uint8_t data)
{
  uint8_t recv = 0;
  uint8_t i;
  for (i = 0; i < 8; i++)
  {
    /* CPOL=0, CPHA=1: ADS1220 latches DIN on rising edge, updates DOUT on
       falling edge -> set MOSI while SCLK low, sample MISO after falling edge */
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

static void ADC_Transmit(uint8_t data)
{
  SPI_Shift(data);
}

static uint8_t ADC_Receive(void)
{
  return SPI_Shift(0xFFu);
}

static uint8_t ADC_TransmitReceive(uint8_t data)
{
  return SPI_Shift(data);
}

/* ------------------------------------------------------------------ */
/*  Test items                                                         */
/* ------------------------------------------------------------------ */

/* Test 1: ADS1220 - start single-shot conversion, wait DRDY, read raw */
static int32_t Test_ADS1220(void)
{
  int32_t sample = 0;

  s_adcHandler.ADC_CS_HIGH           = ADC_CS_HIGH;
  s_adcHandler.ADC_CS_LOW            = ADC_CS_LOW;
  s_adcHandler.ADC_Transmit          = ADC_Transmit;
  s_adcHandler.ADC_Receive           = ADC_Receive;
  s_adcHandler.ADC_TransmitReceive   = ADC_TransmitReceive;
  s_adcHandler.ADC_DRDY_Read         = ADC_DRDY_Read;
  s_adcHandler.ADC_Delay_US          = ADC_Delay_US;

  /* all-zero parameters = defaults (P0N1 diff input, gain 1, internal ref) */
  /* s_adcParams is a static global, already zero-initialized */

  ADS1220_Init(&s_adcHandler, &s_adcParams);
  ADS1220_Reset(&s_adcHandler);
  Delay_ms(10);

  ADS1220_StartSync(&s_adcHandler);      /* start single-shot conversion */

  {
    uint32_t timeout = 5000;             /* wait DRDY# low, up to 5s */
    while (ADC_DRDY_Read() && (timeout-- > 0u))
    {
      Delay_ms(1);
    }
  }

  ADS1220_ReadData(&s_adcHandler, &sample);
  return sample;
}

/* Test 2: DS18B20 on PA1 (single bus, digital temperature sensor) */
static void Test_Temperature(uint8_t *present, int16_t *temp)
{
  *present = (DS18B20_Init() == 0u) ? 1u : 0u;   /* DS18B20_Init: 0 = present */
  *temp = 0;
  if (*present)
  {
    *temp = DS18B20_Get_Temp();                  /* 0.1C resolution, e.g. 253 = 25.3C */
  }
}

/* Test 3: USART1 (JDY-31 bluetooth) - send AT+VERSION, check response */
static uint8_t Test_UART(void)
{
  uint8_t cmd[]  = "AT+VERSION\r\n";
  uint8_t resp[8];
  static const uint32_t bauds[2] = {115200u, 9600u};  /* JDY-31 default is often 9600 */
  uint8_t i;
  HAL_StatusTypeDef st;

  for (i = 0u; i < 2u; i++)
  {
    /* flush RXNE before each attempt */
    while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) != RESET)
    {
      (void)huart1.Instance->DR;
    }

    if (huart1.Init.BaudRate != bauds[i])
    {
      huart1.Init.BaudRate = bauds[i];
      if (HAL_UART_Init(&huart1) != HAL_OK)
      {
        continue;
      }
    }

    HAL_UART_Transmit(&huart1, cmd, sizeof(cmd) - 1u, 200);
    Delay_ms(200);
    st = HAL_UART_Receive(&huart1, resp, 1u, 200);   /* wait for 1st byte */
    if (st == HAL_OK)
    {
      return 1u;
    }
  }
  return 0u;
}

/* Test 4: buttons PB4/PB5 (active high) */
static void Test_Key(uint8_t *k1, uint8_t *k2)
{
  *k1 = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4) == GPIO_PIN_SET) ? 1u : 0u;
  *k2 = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5) == GPIO_PIN_SET) ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/*  Run all tests, show results on OLED                                */
/* ------------------------------------------------------------------ */
void SelfTest_Run(void)
{
  int32_t  adcRaw;
  uint8_t  present;
  int16_t  temp;
  uint8_t  uartOk;
  uint8_t  k1, k2;

  OLED_Init();
  OLED_Clear();

  OLED_ShowString(0, 0,  (const uint8_t *)"Periph Test", 8, 1);

  /* 1. ADS1220 */
  OLED_ShowString(0, 8,  (const uint8_t *)"ADS1220:", 8, 1);
  OLED_Refresh();
  adcRaw = Test_ADS1220();
  OLED_ShowNum(72, 8, (uint32_t)(adcRaw & 0x00FFFFFFu), 6, 8, 1);
  OLED_Refresh();

  /* 2. DS18B20 on PA1 */
  OLED_ShowString(0, 16, (const uint8_t *)"Temp:", 8, 1);
  OLED_Refresh();
  Test_Temperature(&present, &temp);
  if (present)
  {
    OLED_ShowString(48, 16, (const uint8_t *)"OK", 8, 1);
    if (temp < 0)
    {
      OLED_ShowChar(72, 16, '-', 8, 1);
      temp = (int16_t)-temp;
      OLED_ShowNum(78, 16, (uint32_t)(temp / 10), 3, 8, 1);
    }
    else
    {
      OLED_ShowNum(72, 16, (uint32_t)(temp / 10), 3, 8, 1);
    }
    OLED_ShowChar(90, 16, 'C', 8, 1);
  }
  else
  {
    OLED_ShowString(48, 16, (const uint8_t *)"NO ", 8, 1);
  }
  OLED_Refresh();

  /* 3. UART / JDY-31 */
  OLED_ShowString(0, 24, (const uint8_t *)"UART:", 8, 1);
  OLED_Refresh();
  uartOk = Test_UART();
  OLED_ShowString(48, 24, uartOk ? (const uint8_t *)"OK  " : (const uint8_t *)"NO  ", 8, 1);
  OLED_Refresh();

  /* 4. keys - poll for 10s so you can press them and watch the result */
  OLED_ShowString(0, 32, (const uint8_t *)"K1:", 8, 1);
  OLED_ShowString(40, 32, (const uint8_t *)"K2:", 8, 1);
  OLED_Refresh();
  {
    uint32_t t = 0u;
    while (t < 10000u)
    {
      Test_Key(&k1, &k2);
      OLED_ShowNum(24, 32, k1, 1, 8, 1);
      OLED_ShowNum(64, 32, k2, 1, 8, 1);
      OLED_Refresh();
      Delay_ms(100);
      t += 100u;
    }
  }
}
