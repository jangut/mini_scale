/**
 **********************************************************************************
 * @file   delay.h
 * @brief  Timer based delay functions (TIM2, 1MHz -> 1us resolution)
 *
 * @note   Requires MX_TIM2_Init() to be called before any delay function is used.
 *         TIM2 is used as a free-running 1MHz counter (prescaler 71 @ 72MHz),
 *         see delay.c for details.
 **********************************************************************************
 **/
#ifndef __DELAY_H
#define __DELAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void Delay_us(uint32_t us);
void Delay_ms(uint32_t ms);

/* kept for compatibility with jdy_slave (millisecond delay) */
void delay_function(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif

#endif /* __DELAY_H */
