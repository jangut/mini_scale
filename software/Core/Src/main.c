/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "scale.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
TIM_HandleTypeDef htim3;
volatile uint8_t g_sample_flag = 0;   /* TIM3 采样节拍置 1 */
volatile uint8_t g_wakeup_flag = 0;   /* EXTI4 唤醒置 1 */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
 * @brief  发送一帧称重数据，格式："<weight_g>,<temp_c>\r\n"（与 GUI 约定）。
 *         温度无效（超 0~125°C）时只发重量列，GUI 温度显示 "--"。
 * @param  w_centi  重量 × 100（0.01 g）
 * @param  t_deci   温度 × 10（0.1 °C）
 */
static void uart_send_frame(int32_t w_centi, int32_t t_deci)
{
  char buf[40];
  int32_t w_int, w_frac;
  uint16_t n;

  if (w_centi < 0)
  {
    w_centi = 0;                 /* 去皮后负值显示 0.00 */
  }
  w_int  = w_centi / 100;
  w_frac = w_centi % 100;

  if ((t_deci >= 0) && (t_deci <= 1250))
  {
    n = (uint16_t)snprintf(buf, sizeof(buf), "%ld.%02ld,%ld.%ld\r\n",
                           (long)w_int, (long)w_frac,
                           (long)(t_deci / 10), (long)(t_deci % 10));
  }
  else
  {
    n = (uint16_t)snprintf(buf, sizeof(buf), "%ld.%02ld\r\n",
                           (long)w_int, (long)w_frac);
  }
  HAL_UART_Transmit(&huart1, (uint8_t *)buf, n, 100u);
}

/**
 * @brief  按键检测（高有效，外部 10k 下拉；按下接 3.3V）。
 *         按下并稳定 30ms 后触发一次；长按会周期性重复触发（对去皮无害）。
 * @note   消抖状态在两个按键调用间共享，实际使用中不会同时按两个键。
 */
static uint8_t key_stable_pressed(GPIO_TypeDef *port, uint16_t pin)
{
  static uint32_t t0 = 0;
  static uint8_t armed = 0;

  if (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET)
  {
    if (!armed)
    {
      t0 = HAL_GetTick();
      armed = 1;
    }
    if ((HAL_GetTick() - t0) >= 30u)
    {
      armed = 0;
      return 1;
    }
  }
  else
  {
    armed = 0;
  }
  return 0;
}

/**
 * @brief  发送字符串（串口命令应答用）
 */
static void uart_send_str(const char *s)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)strlen(s), 100u);
}

/**
 * @brief  TIM3 采样节拍初始化：50ms 更新中断（72MHz：PSC=3600-1, ARR=1000-1）
 */
static void tim3_init(void)
{
  __HAL_RCC_TIM3_CLK_ENABLE();
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 3600u - 1u;      /* 72MHz/3600 = 20kHz */
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 1000u - 1u;         /* 20kHz/1000 = 20Hz = 50ms */
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_NVIC_SetPriority(TIM3_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(TIM3_IRQn);
  if (HAL_TIM_Base_Start_IT(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
 * @brief  TIM3 更新中断回调：置采样标志
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM3)
  {
    g_sample_flag = 1;
  }
}

/**
 * @brief  EXTI 中断回调：KEY1(PB4) 唤醒标志
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_4)
  {
    g_wakeup_flag = 1;
  }
}

/**
 * @brief  进入 STOP 休眠；KEY1(PB4/EXTI4) 唤醒后重新初始化系统
 */
static void system_enter_stop(void)
{
  GPIO_InitTypeDef exti_cfg = {0};

  /* 停采样节拍，ADS1220 下电 */
  HAL_TIM_Base_Stop_IT(&htim3);
  scale_power_down();

  /* SysTick 暂停，进入 STOP（WFI 等 EXTI4 唤醒） */
  HAL_SuspendTick();
  HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);

  /* ===== 被 KEY1 唤醒，从此继续执行 ===== */
  HAL_ResumeTick();

  /* STOP 模式丢失时钟树，全部重新初始化 */
  SystemClock_Config();
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_USART1_UART_Init();
  scale_init();

  /* MX_GPIO_Init 会重置 PB4，重新配 EXTI4 唤醒源 */
  exti_cfg.Pin = GPIO_PIN_4;
  exti_cfg.Mode = GPIO_MODE_IT_RISING;
  exti_cfg.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &exti_cfg);
  HAL_NVIC_SetPriority(EXTI4_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(EXTI4_IRQn);

  tim3_init();

  /* 等按键松开，避免唤醒瞬间又按着再次触发休眠 */
  HAL_Delay(300);
}

/**
 * @brief  串口命令解析（行格式，与上位机数据流互不干扰）：
 *         TARE            去皮并保存 Flash
 *         CAL <重量g>     标定 K 值并保存 Flash（放已知砝码后发送）
 *         SLEEP           进入休眠
 */
static void uart_cmd_exec(char *cmd)
{
  if (strncmp(cmd, "TARE", 4) == 0)
  {
    scale_tare();
    uart_send_str("OK TARE\r\n");
  }
  else if (strncmp(cmd, "CAL", 3) == 0)
  {
    float w;
    if (sscanf(cmd + 3, "%f", &w) == 1)
    {
      scale_calibrate(w);
      uart_send_str("OK CAL\r\n");
    }
    else
    {
      uart_send_str("ERR CAL\r\n");
    }
  }
  else if (strncmp(cmd, "SLEEP", 5) == 0)
  {
    uart_send_str("OK SLEEP\r\n");
    system_enter_stop();
  }
  else
  {
    uart_send_str("ERR\r\n");
  }
}

/**
 * @brief  轮询串口接收并解析命令（非阻塞）
 */
static void uart_cmd_poll(void)
{
  static char line[32];
  static uint8_t n = 0;
  uint8_t c;

  while (HAL_UART_Receive(&huart1, &c, 1, 0) == HAL_OK)
  {
    if ((c == '\r') || (c == '\n'))
    {
      if (n > 0)
      {
        line[n] = '\0';
        uart_cmd_exec(line);
        n = 0;
      }
    }
    else if (n < (sizeof(line) - 1))
    {
      line[n++] = (char)c;
    }
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  GPIO_InitTypeDef exti_cfg = {0};

  scale_init();

  /* KEY1(PB4) 配 EXTI4 上升沿：平时读电平做按键，休眠时作唤醒源 */
  exti_cfg.Pin = GPIO_PIN_4;
  exti_cfg.Mode = GPIO_MODE_IT_RISING;
  exti_cfg.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &exti_cfg);
  HAL_NVIC_SetPriority(EXTI4_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(EXTI4_IRQn);

  tim3_init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* TIM3 采样节拍（50ms）驱动，主循环非阻塞 */
    if (g_sample_flag)
    {
      g_sample_flag = 0;
      if (scale_update())
      {
        /* KEY2(PB5) = 去皮：当前读数置零并存 Flash */
        if (key_stable_pressed(GPIOB, GPIO_PIN_5))
        {
          scale_tare();
        }

        /* 串口命令（TARE / CAL <g> / SLEEP） */
        uart_cmd_poll();

        /* 发送一帧 weight,temp */
        uart_send_frame(scale_weight_centi(), scale_temp_centi());

        /* 启动下一轮转换 */
        scale_start();
      }
    }

    /* KEY1(PB4) = 电源键：按下进入休眠（休眠中再按唤醒） */
    if (key_stable_pressed(GPIOB, GPIO_PIN_4))
    {
      system_enter_stop();
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_7, GPIO_PIN_RESET);

  /*Configure GPIO pins : PA3 PA4 PA7 */
  GPIO_InitStruct.Pin = GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PA5 PA6 */
  GPIO_InitStruct.Pin = GPIO_PIN_5|GPIO_PIN_6;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PB2 */
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PB4 PB5 */
  GPIO_InitStruct.Pin = GPIO_PIN_4|GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PB6 PB7 */
  GPIO_InitStruct.Pin = GPIO_PIN_6|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
