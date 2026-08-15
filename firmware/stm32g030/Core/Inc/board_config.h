#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "main.h"

/*
 * 当前 PCB 使用 STM32G030C8T6，板级外设和引脚统一写在这里。
 * CubeMX 重新分配外设句柄或 GPIO 后，优先只改本文件，驱动层不应出现板级引脚号。
 */

extern I2C_HandleTypeDef hi2c1;
extern UART_HandleTypeDef huart1;
extern ADC_HandleTypeDef hadc1;

#define BOARD_SPECTRAL_I2C_HANDLE    hi2c1
#define BOARD_AS7343_I2C_HANDLE      BOARD_SPECTRAL_I2C_HANDLE /* 兼容旧代码的别名 */
#define BOARD_CONSOLE_UART_HANDLE    huart1
#define BOARD_NTC_ADC_HANDLE         hadc1

#define BOARD_I2C_SCL_PORT            GPIOB
#define BOARD_I2C_SCL_PIN             GPIO_PIN_6
#define BOARD_I2C_SDA_PORT            GPIOB
#define BOARD_I2C_SDA_PIN             GPIO_PIN_7

#define BOARD_STATUS_LED_PORT        GPIOA
#define BOARD_STATUS_LED_PIN         GPIO_PIN_8
#define BOARD_STATUS_LED_ACTIVE      GPIO_PIN_SET

#define BOARD_LED_405_PORT           GPIOA
#define BOARD_LED_405_PIN            GPIO_PIN_4
#define BOARD_LED_WHITE_PORT         GPIOA
#define BOARD_LED_WHITE_PIN          GPIO_PIN_6
#define BOARD_LED_850_PORT           GPIOA
#define BOARD_LED_850_PIN            GPIO_PIN_7
#define BOARD_LED_940_PORT           GPIOB
#define BOARD_LED_940_PIN            GPIO_PIN_0
#define BOARD_OPTICAL_LED_ACTIVE     GPIO_PIN_SET

/* 点亮和熄灭光源后都留出稳定时间，再开始积分采样。 */
#define BOARD_LED_SETTLE_MS          50U
#define BOARD_DARK_SETTLE_MS         50U

/* NTC 分压：3.3V -> 10k 固定电阻 -> ADC 采样点 -> 10k NTC -> GND。 */
#define BOARD_ADC_FULL_SCALE         4095U
#define BOARD_ADC_REFERENCE_MV       3300U
#define BOARD_NTC_FIXED_OHM          10000U
#define BOARD_ADC_AVERAGE_SAMPLES    16U

/* 串口命令和单帧输出的缓冲区上限。 */
#define BOARD_UART_COMMAND_MAX       96U
#define BOARD_UART_TX_MAX            640U

#endif
