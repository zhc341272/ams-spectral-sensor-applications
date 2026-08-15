#ifndef SPECTRAL_APP_H
#define SPECTRAL_APP_H

#include "stm32g0xx_hal.h"

/* 应用层负责串口协议、四路光源时序、测温和光谱驱动调度。 */

#ifdef __cplusplus
extern "C" {
#endif

void SpectralApp_Init(void);
void SpectralApp_Process(void);
void SpectralApp_UartRxCpltCallback(UART_HandleTypeDef *huart);
void SpectralApp_UartErrorCallback(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif
