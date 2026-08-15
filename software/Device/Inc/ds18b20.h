#ifndef __DS18B20_H
#define __DS18B20_H 
#include "stm32f1xx_hal.h"

//DS18B20数据引脚：PB6
#define DS18B20_DQ_PORT      GPIOA
#define DS18B20_DQ_PIN       GPIO_PIN_1

//IO方向设置（输入/输出模式切换，HAL_GPIO_Init 重新配置）
#define DS18B20_IO_IN()  { GPIO_InitTypeDef g={0}; g.Pin=DS18B20_DQ_PIN; g.Mode=GPIO_MODE_INPUT; g.Pull=GPIO_PULLUP; HAL_GPIO_Init(DS18B20_DQ_PORT,&g); }
#define DS18B20_IO_OUT() { GPIO_InitTypeDef g={0}; g.Pin=DS18B20_DQ_PIN; g.Mode=GPIO_MODE_OUTPUT_PP; g.Speed=GPIO_SPEED_FREQ_HIGH; HAL_GPIO_Init(DS18B20_DQ_PORT,&g); }

//IO操作函数
#define DS18B20_DQ_OUT(x) HAL_GPIO_WritePin(DS18B20_DQ_PORT, DS18B20_DQ_PIN, (x)?GPIO_PIN_SET:GPIO_PIN_RESET) //数据端口 PA1 输出
#define DS18B20_DQ_IN()    HAL_GPIO_ReadPin(DS18B20_DQ_PORT, DS18B20_DQ_PIN)  //数据端口 PA1 输入

extern int16_t temperature;//温度
extern uint16_t tempMax;//温度上限
extern uint16_t tempMin;//温度下限

uint8_t DS18B20_Init(void);			//初始化DS18B20
int16_t DS18B20_Get_Temp(void);	//获取温度
void DS18B20_Start(void);		//开始温度转换
void DS18B20_Write_Byte(uint8_t dat);//写入一个字节
uint8_t DS18B20_Read_Byte(void);		//读出一个字节
uint8_t DS18B20_Read_Bit(void);		//读出一个位
uint8_t DS18B20_Check(void);			//检测是否存在DS18B20
void DS18B20_Rst(void);			//复位DS18B20  
//void DisplayTemperature(void);//显示温度函数
#endif
