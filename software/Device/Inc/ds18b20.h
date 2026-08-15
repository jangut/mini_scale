#ifndef __DS18B20_H
#define __DS18B20_H 
#include "sys.h"   
//IO方向设置（CRL寄存器对应引脚0~7,CRH寄存器对应引脚8~15）
//DS18B20_GPIO_PORT->CRH&=0xFFFFFFF0为PA8引脚输出模式对应的寄存器清空
//DS18B20_GPIO_PORT->CRH|=0x00000008将(CNF8[1:0]设置为10:上拉/下拉输入模式，MODE8[1;0]设置为00:输入模式)
//DS18B20_GPIO_PORT->CRH|=0x00000003将(CNF8[1:0]设置为00:通用推挽输出模式 ，MODE8[1;0]设置为11:最大50MHZ)
//IO方向设置
#define DS18B20_IO_IN()  {GPIOB->CRL&=0xF0FFFFFF;GPIOB->CRL|=0x08000000;}
#define DS18B20_IO_OUT() {GPIOB->CRL&=0xF0FFFFFF;GPIOB->CRL|=0x03000000;}
////IO操作函数											   
#define	DS18B20_DQ_OUT PBout(6) //数据端口	PB6
#define	DS18B20_DQ_IN  PBin(6)  //数据端口	PB6

extern short temperature;//温度
extern u16 tempMax;//温度上限
extern u16 tempMin;//温度下限
   	
u8 DS18B20_Init(void);			//初始化DS18B20
short DS18B20_Get_Temp(void);	//获取温度
void DS18B20_Start(void);		//开始温度转换
void DS18B20_Write_Byte(u8 dat);//写入一个字节
u8 DS18B20_Read_Byte(void);		//读出一个字节
u8 DS18B20_Read_Bit(void);		//读出一个位
u8 DS18B20_Check(void);			//检测是否存在DS18B20
void DS18B20_Rst(void);			//复位DS18B20  
//void DisplayTemperature(void);//显示温度函数
#endif















