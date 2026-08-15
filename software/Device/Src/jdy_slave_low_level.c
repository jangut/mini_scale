/*task handler for jdy_31 bluetooth slave module*/
/*version 1.0*/
/*Reza Ebrahimi https://github.com/ebrezadev */
/*STM32 HAL port: low-level implementation using USART1 (PA9 TX / PA10 RX)*/
#include "jdy_slave.h"
#include "usart.h"
#include "delay.h"

/*setting serial port baud rate for bluetooth module (USART1)*/
void bluetooth_uart_set (uint32_t baud_rate)
{
  huart1.Init.BaudRate = baud_rate;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
}

/*returning the number of available bytes in serial buffer (0 or 1, polling RXNE flag)*/
uint8_t bluetooth_buffer_status ()
{
  return (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) != RESET) ? NOT_EMPTY : EMPTY;
}

/*reading the serial buffer into input_string[] with predefined length*/
void bluetooth_buffer_read (uint8_t *input_string, uint8_t string_length)
{
  HAL_UART_Receive(&huart1, input_string, string_length, HAL_MAX_DELAY);
}

/*function to send one byte through bluetooth serial port*/
void bluetooth_send_byte (uint8_t input_byte)
{
  HAL_UART_Transmit(&huart1, &input_byte, 1, HAL_MAX_DELAY);
}

/*delay function in milliseconds is implemented in delay.c (TIM2 based)*/
