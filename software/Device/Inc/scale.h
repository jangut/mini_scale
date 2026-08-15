/**
 **********************************************************************************
 * @file   scale.h
 * @brief  Weighing scale core: ADS1220 sampling, filtering, tare, conversion
 *
 * Hardware (from netlist):
 *   ADS1220: CS=PA3, SCLK=PA4, DRDY=PA5, MISO=PA6, MOSI=PA7 (software SPI)
 *   Signal  : INA826 output -> ADS1220 AIN1 (single-ended vs AVSS)
 *   VREF    : REF5025 2.5V -> REFP0 (REFN0 = GND)
 **********************************************************************************
 **/
#ifndef __SCALE_H
#define __SCALE_H

#include <stdint.h>

/* Full scale and overload limits (gram) */
#define SCALE_FULL_SCALE_G   500.0f
#define SCALE_OVERLOAD_G     600.0f   /* 120% FS */

/* ---- Calibration (TBD by host-PC tuning) --------------------------------
   weight(g) = (raw_filtered - zero_raw) * SCALE_LSB_TO_G
   SCALE_LSB_TO_G is a placeholder: connect the board to the host GUI, put
   known weights, measure the error, then fix this value (or use a 2-point
   calibration routine). Same for the tare zero. */
#define SCALE_LSB_TO_G       (10.0f / 14250.0f)   /* calibrated: 10g weight -> 14250 LSB raw delta */

typedef enum
{
  SCALE_STATE_INIT = 0,
  SCALE_STATE_READY,      /* measuring, normal */
  SCALE_STATE_STABLE,     /* value settled (delta < threshold) */
  SCALE_STATE_OVERLOAD,   /* over 120% FS */
  SCALE_STATE_FAULT       /* sensor/signal fault (bad reads) */
} Scale_State_t;

void        Scale_Init(void);
void        Scale_Update(void);            /* call periodically (~20Hz) */
void        Scale_Tare(void);              /* set current value as zero */
float       Scale_GetWeight(void);         /* weight in gram */
int32_t     Scale_GetRaw(void);            /* latest raw sample */
int32_t     Scale_GetRawFiltered(void);    /* filtered raw value */
Scale_State_t Scale_GetState(void);

#endif /* __SCALE_H */
