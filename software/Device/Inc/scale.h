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

/* ---- Calibration ---------------------------------------------------------
   weight(g) = (raw_filtered - zero_raw) * SCALE_LSB_TO_G
   Linear part calibrated 2026-08-16 with a 0-200 g weight set (10 g steps,
   tare first): fit gave display = 0.8386 x true -> corrected the original
   placeholder (10 g -> 14250 LSB) to 10 g -> 11950 LSB.
   A 0-500 g capture (same day) then showed residual nonlinearity
   (-0.2% low end, -0.47% at 500 g); Scale_GetWeight applies a quadratic
   correction on top (SCALE_NL_A/B in scale.c), residual < 0.25 g. */
#define SCALE_LSB_TO_G       (10.0f / 11950.0f)   /* calibrated: 10g weight -> 11950 LSB raw delta */

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
void        Scale_Sleep(void);             /* power down ADS1220 before MCU STOP */
void        Scale_Wakeup(void);            /* re-init ADC + quick warm-up (keeps tare) */
void        Scale_SetTempC(float temp_c);  /* feed the current temperature for drift compensation */
float       Scale_GetWeight(void);         /* weight in gram */
int32_t     Scale_GetRaw(void);            /* latest raw sample */
int32_t     Scale_GetRawFiltered(void);    /* filtered raw value */
Scale_State_t Scale_GetState(void);

#endif /* __SCALE_H */
