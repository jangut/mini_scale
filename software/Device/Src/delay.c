/**
 **********************************************************************************
 * @file   delay.c
 * @brief  Timer based delay functions using TIM2
 *
 * TIM2 is configured by CubeMX (MX_TIM2_Init) as a 16-bit up-counter.
 * Here it is re-tuned to count at 1MHz (APB1 timer clock 72MHz, prescaler 71),
 * so one tick equals 1us. Delays are implemented by polling the counter,
 * no interrupt is required.
 *
 * @note   Call MX_TIM2_Init() before using any delay function.
 *         TIM2 must not be used for other purposes while delays are used.
 **********************************************************************************
 **/
#include "delay.h"
#include "tim.h"

#define DELAY_TIM_PRESCALER      71u   /* 72MHz / 72 = 1MHz -> 1us per tick   */
#define DELAY_TIM_MAX_US         60000u/* single shot below 16-bit overflow    */
                                       /* (65535us), longer delays are split  */

static uint8_t s_timer_started = 0;

static void Delay_Init(void)
{
  if (s_timer_started)
  {
    return;
  }
  if (htim2.Instance == NULL)
  {
    return; /* MX_TIM2_Init() must be called first */
  }

  __HAL_TIM_SET_PRESCALER(&htim2, DELAY_TIM_PRESCALER);
  __HAL_TIM_SET_AUTORELOAD(&htim2, 0xFFFFu);
  __HAL_TIM_SET_COUNTER(&htim2, 0u);
  /* generate an update event to load PSC/ARR shadow registers immediately */
  htim2.Instance->EGR = TIM_EVENTSOURCE_UPDATE;

  HAL_TIM_Base_Start(&htim2);
  s_timer_started = 1;
}

/**
 * @brief  Delay in microseconds (blocking, TIM2 polling)
 * @param  us: delay length in microseconds
 */
void Delay_us(uint32_t us)
{
  Delay_Init();

  /* split long delays so the 16-bit counter never overflows (max 60000us) */
  while (us > DELAY_TIM_MAX_US)
  {
    Delay_us(DELAY_TIM_MAX_US);
    us -= DELAY_TIM_MAX_US;
  }

  __HAL_TIM_SET_COUNTER(&htim2, 0u);
  while (__HAL_TIM_GET_COUNTER(&htim2) < us)
  {
  }
}

/**
 * @brief  Stop the delay timer (low-power support).
 * @note   Call before entering STOP mode: TIM2 clock stops in STOP mode,
 *         and without this the polling loop in Delay_us() would hang after
 *         wake-up. Delay_us()/Delay_ms() re-initialize the timer on the
 *         next call, so no explicit re-init is needed after wake-up.
 */
void Delay_Deinit(void)
{
  if (!s_timer_started)
  {
    return;
  }
  HAL_TIM_Base_Stop(&htim2);
  s_timer_started = 0;
}

/**
 * @brief  Delay in milliseconds (blocking)
 * @param  ms: delay length in milliseconds
 */
void Delay_ms(uint32_t ms)
{
  while (ms--)
  {
    Delay_us(1000u);
  }
}

/**
 * @brief  Millisecond delay, kept for compatibility with jdy_slave module
 * @param  delay_ms: delay length in milliseconds
 */
void delay_function(uint32_t delay_ms)
{
  Delay_ms(delay_ms);
}
