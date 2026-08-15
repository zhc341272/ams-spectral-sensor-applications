#ifndef AS734X_H
#define AS734X_H

#include "stm32g0xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AS734X_I2C_ADDRESS_7BIT       0x39U
#define TCS3448_I2C_ADDRESS_7BIT       0x59U
#define AS734X_MAX_CHANNELS           14U
#define AS734X_GAIN_COUNT             13U

/*
 * 仅靠 I²C 地址和器件 ID 无法区分所有光学版本。
 * 驱动先确认寄存器协议族，再把共用同一数字接口的候选型号一并上报。
 */
typedef enum
{
    AS734X_MODEL_NONE = 0,
    AS734X_MODEL_AS7341_FAMILY,
    AS734X_MODEL_AS7343_FAMILY,
    AS734X_MODEL_TCS3448,
    AS734X_MODEL_UNKNOWN_ADDRESS
} AS734X_Model_t;

typedef enum
{
    AS734X_PROTOCOL_NONE = 0,
    AS734X_PROTOCOL_AS7341_SMUX,
    AS734X_PROTOCOL_AS7343_AUTO_SMUX,
    AS734X_PROTOCOL_RAW_I2C
} AS734X_Protocol_t;

/*
 * 同系列器件可能共用地址、ID 和寄存器表，profile 只决定原始 ADC 槽位如何解释。
 * AUTO 不改变硬件识别结果，并以普通 AS7341 或 AS7343 的通道顺序作为默认值。
 */
typedef enum
{
    AS734X_PROFILE_AUTO = 0,
    AS734X_PROFILE_AS7341,
    AS734X_PROFILE_AS7341L,
    AS734X_PROFILE_AS7343,
    AS734X_PROFILE_AS7343L,
    AS734X_PROFILE_TCS3448
} AS734X_Profile_t;

typedef enum
{
    AS734X_OK = 0,
    AS734X_ERROR_ARGUMENT,
    AS734X_ERROR_I2C,
    AS734X_ERROR_ID,
    AS734X_ERROR_TIMEOUT,
    AS734X_ERROR_BUS_DATA,
    AS734X_ERROR_VERIFY,
    AS734X_ERROR_UNSUPPORTED
} AS734X_Status_t;

typedef enum
{
    AS734X_GAIN_0_5X = 0,
    AS734X_GAIN_1X,
    AS734X_GAIN_2X,
    AS734X_GAIN_4X,
    AS734X_GAIN_8X,
    AS734X_GAIN_16X,
    AS734X_GAIN_32X,
    AS734X_GAIN_64X,
    AS734X_GAIN_128X,
    AS734X_GAIN_256X,
    AS734X_GAIN_512X,
    AS734X_GAIN_1024X,
    AS734X_GAIN_2048X
} AS734X_Gain_t;

#define AS734X_FLAG_ASTATUS_SAT       (1U << 0)
#define AS734X_FLAG_DIGITAL_SAT       (1U << 1)
#define AS734X_FLAG_ANALOG_SAT        (1U << 2)

#define AS734X_CONFIDENCE_NONE        0U
#define AS734X_CONFIDENCE_LOW         1U
#define AS734X_CONFIDENCE_MEDIUM      2U
#define AS734X_CONFIDENCE_HIGH        3U

typedef struct
{
    I2C_HandleTypeDef *i2c;
    uint8_t address_7bit;
    AS734X_Model_t model;
    AS734X_Protocol_t protocol;
    AS734X_Profile_t profile;
    uint8_t profile_ambiguous;
    uint8_t confidence;

    uint8_t id_raw;
    uint8_t id_code;
    uint8_t revision;
    uint8_t auxiliary_id;

    uint8_t signature_92;
    uint8_t signature_5a;
    uint8_t signature_cfg0;
    uint8_t signature_cfg20;

    uint8_t atime;
    uint16_t astep;
    AS734X_Gain_t gain;
    uint8_t channel_count;

    uint8_t init_busy_warning;
    uint8_t last_enable;
    uint8_t last_status2;
    uint8_t last_statusx;
    uint32_t last_hal_i2c_error;
} AS734X_Device_t;

typedef struct
{
    uint16_t channels[AS734X_MAX_CHANNELS];
    uint8_t channel_count;
    uint8_t astat;
    uint8_t status2;
    uint8_t flags;
} AS734X_Sample_t;

typedef struct
{
    uint8_t address_ready;
    uint8_t address_7bit;
    uint8_t ack_count;
    uint8_t ack_1;
    uint8_t ack_2;
    uint8_t ack_3;
    uint8_t ack_4;
    uint8_t scl_level;
    uint8_t sda_level;
    uint32_t hal_i2c_error;

    AS734X_Model_t model;
    AS734X_Protocol_t protocol;
    AS734X_Profile_t profile;
    uint8_t profile_ambiguous;
    uint8_t confidence;
    uint8_t id_raw;
    uint8_t id_code;
    uint8_t revision;
    uint8_t auxiliary_id;
    uint8_t signature_92;
    uint8_t signature_5a;
    uint8_t signature_cfg0;
    uint8_t signature_cfg20;

    uint8_t enable;
    uint8_t atime;
    uint16_t astep;
    uint8_t cfg1;
    uint8_t status2;
    uint8_t statusx;
    uint8_t init_ready;
    uint8_t protocol_ok;
} AS734X_Diagnostic_t;

AS734X_Status_t AS734X_Detect(AS734X_Device_t *device,
                              I2C_HandleTypeDef *i2c);
AS734X_Status_t AS734X_Init(AS734X_Device_t *device,
                            I2C_HandleTypeDef *i2c);
AS734X_Status_t AS734X_Reinitialize(AS734X_Device_t *device);
AS734X_Status_t AS734X_SetProfile(AS734X_Device_t *device,
                                    AS734X_Profile_t profile);
AS734X_Profile_t AS734X_GetEffectiveProfile(const AS734X_Device_t *device);
AS734X_Status_t AS734X_SetTiming(AS734X_Device_t *device,
                                 uint8_t atime,
                                 uint16_t astep);
AS734X_Status_t AS734X_SetGain(AS734X_Device_t *device,
                               AS734X_Gain_t gain);
AS734X_Status_t AS734X_ReadOne(AS734X_Device_t *device,
                               AS734X_Sample_t *sample);
AS734X_Status_t AS734X_Reset(AS734X_Device_t *device);
AS734X_Status_t AS734X_Diagnose(AS734X_Device_t *device,
                                AS734X_Diagnostic_t *diagnostic);

AS734X_Status_t AS734X_ReadRegister(AS734X_Device_t *device,
                                    uint8_t bank,
                                    uint8_t reg,
                                    uint8_t *value);
AS734X_Status_t AS734X_WriteRegister(AS734X_Device_t *device,
                                     uint8_t bank,
                                     uint8_t reg,
                                     uint8_t value);

uint8_t AS734X_GetChannelCount(const AS734X_Device_t *device);
const char *AS734X_GetChannelName(const AS734X_Device_t *device,
                                  uint8_t channel);
uint16_t AS734X_GetChannelWavelengthNm(const AS734X_Device_t *device,
                                       uint8_t channel);
uint32_t AS734X_GetIntegrationTimeUs(const AS734X_Device_t *device);
uint32_t AS734X_GetGainX1000(AS734X_Gain_t gain);
uint8_t AS734X_GetMaximumGainIndex(const AS734X_Device_t *device);
uint16_t AS734X_GetUsefulMaximum(const AS734X_Sample_t *sample);
AS734X_Gain_t AS734X_RecommendGain(const AS734X_Device_t *device,
                                   AS734X_Gain_t current_gain,
                                   const AS734X_Sample_t *sample);

const char *AS734X_ModelString(AS734X_Model_t model);
const char *AS734X_ModelCandidatesString(AS734X_Model_t model);
const char *AS734X_ProtocolString(AS734X_Protocol_t protocol);
const char *AS734X_ProfileString(AS734X_Profile_t profile);
const char *AS734X_EffectiveProfileString(const AS734X_Device_t *device);
const char *AS734X_ConfidenceString(uint8_t confidence);
const char *AS734X_StatusString(AS734X_Status_t status);

#ifdef __cplusplus
}
#endif

#endif
