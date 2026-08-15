#include "spectral_app.h"

#include "as734x.h"
#include "board_config.h"
#include "project_version.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 本文件是主板应用层：接收串口命令，安排光源和暗场采样，并把结果按协议 2.1
 * 发给上位机。传感器型号差异留在 as734x.c 内处理，这里的测量流程对三种器件一致。
 */

#define FW_VERSION                  PROJECT_FW_VERSION_STRING
#define PROTOCOL_VERSION            PROJECT_PROTOCOL_VERSION
#define DEFAULT_STREAM_MS           3000U
#define MIN_STREAM_MS               1000U
#define MAX_AUTO_GAIN_ITERATIONS    4U
#define AS_TEST_MAX_DUMP_COUNT      64U
#define ARRAY_SIZE(x)               (sizeof(x) / sizeof((x)[0]))

typedef enum
{
    LIGHT_405 = 0,
    LIGHT_WHITE,
    LIGHT_850,
    LIGHT_940,
    LIGHT_COUNT,
    LIGHT_OFF = 0xFF
} LightSource_t;

typedef struct
{
    const char *name;
    GPIO_TypeDef *port;
    uint16_t pin;
} LightDefinition_t;

typedef struct
{
    uint32_t sequence;
    LightSource_t source;
    AS734X_Gain_t gain;
    int16_t temperature_x10;
    AS734X_Sample_t light;
    AS734X_Sample_t dark;
} MeasurementRecord_t;

typedef struct
{
    int16_t temperature_c;
    uint32_t resistance_ohm;
} NtcPoint_t;

typedef struct
{
    HAL_StatusTypeDef status;
    uint16_t raw;
    uint32_t millivolts;
    uint32_t resistance_ohm;
    int16_t temperature_x10;
} BoardTemperature_t;

static const LightDefinition_t k_lights[LIGHT_COUNT] =
{
    {"405",   BOARD_LED_405_PORT,   BOARD_LED_405_PIN},
    {"WHITE", BOARD_LED_WHITE_PORT, BOARD_LED_WHITE_PIN},
    {"850",   BOARD_LED_850_PORT,   BOARD_LED_850_PIN},
    {"940",   BOARD_LED_940_PORT,   BOARD_LED_940_PIN}
};

static const NtcPoint_t k_ntc_table[] =
{
    {-20, 86048U}, {-15, 65281U}, {-10, 50049U}, { -5, 38753U},
    {  0, 30288U}, {  5, 23884U}, { 10, 18992U}, { 15, 15223U},
    { 20, 12294U}, { 25, 10000U}, { 30,  8190U}, { 35,  6751U},
    { 40,  5599U}, { 45,  4671U}, { 50,  3919U}, { 55,  3306U},
    { 60,  2803U}, { 65,  2388U}, { 70,  2044U}, { 75,  1757U},
    { 80,  1517U}
};

static AS734X_Device_t g_sensor;
static AS734X_Status_t g_sensor_status = AS734X_ERROR_I2C;
static AS734X_Gain_t g_source_gain[LIGHT_COUNT] =
{
    AS734X_GAIN_16X,
    AS734X_GAIN_16X,
    AS734X_GAIN_16X,
    AS734X_GAIN_16X
};

static uint8_t g_auto_gain_enabled = 1U;
static uint8_t g_stream_enabled = 0U;
static uint32_t g_stream_interval_ms = DEFAULT_STREAM_MS;
static uint32_t g_next_stream_ms = 0U;
static uint32_t g_sequence = 0U;
static uint32_t g_last_heartbeat_ms = 0U;
static uint8_t g_led_mask = 0U;

static uint8_t g_uart_rx_byte;
static volatile char g_uart_build_buffer[BOARD_UART_COMMAND_MAX];
static volatile uint16_t g_uart_build_length = 0U;
static volatile char g_uart_command_buffer[BOARD_UART_COMMAND_MAX];
static volatile uint8_t g_uart_command_ready = 0U;

/* 协议帧使用 CRC-16/CCITT-FALSE，初值 0xFFFF，多项式 0x1021。 */
static uint16_t Crc16Ccitt(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFFU;
    uint16_t index;

    for (index = 0U; index < length; ++index)
    {
        uint8_t bit;
        crc ^= (uint16_t)data[index] << 8;
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = ((crc & 0x8000U) != 0U) ?
                  (uint16_t)((crc << 1) ^ 0x1021U) :
                  (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void ConsoleRaw(const char *text)
{
    size_t length;

    if (text == NULL) return;
    length = strlen(text);
    if (length > UINT16_MAX) length = UINT16_MAX;
    (void)HAL_UART_Transmit(&BOARD_CONSOLE_UART_HANDLE,
                            (uint8_t *)text,
                            (uint16_t)length,
                            2000U);
}

static void ProtocolSendPrepared(const char *payload, uint16_t length)
{
    char frame[BOARD_UART_TX_MAX + 16U];
    uint16_t crc;
    int frame_length;

    if ((payload == NULL) || (length >= BOARD_UART_TX_MAX)) return;
    /* CRC 只覆盖 $ 和 * 之间的有效载荷。 */
    crc = Crc16Ccitt((const uint8_t *)payload, length);
    frame_length = snprintf(frame,
                            sizeof(frame),
                            "$%s*%04X\r\n",
                            payload,
                            crc);
    if ((frame_length > 0) && ((size_t)frame_length < sizeof(frame)))
    {
        (void)HAL_UART_Transmit(&BOARD_CONSOLE_UART_HANDLE,
                                (uint8_t *)frame,
                                (uint16_t)frame_length,
                                3000U);
    }
}

static void ProtocolSendPayload(const char *format, ...)
{
    char payload[BOARD_UART_TX_MAX];
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(payload, sizeof(payload), format, args);
    va_end(args);

    if ((length < 0) || ((size_t)length >= sizeof(payload)))
    {
        static const char overflow[] = "ERR,TX_OVERFLOW";
        ProtocolSendPrepared(overflow, (uint16_t)(sizeof(overflow) - 1U));
        return;
    }
    ProtocolSendPrepared(payload, (uint16_t)length);
}

static GPIO_PinState OpticalLedState(uint8_t enabled)
{
    if (enabled != 0U) return BOARD_OPTICAL_LED_ACTIVE;
    return (BOARD_OPTICAL_LED_ACTIVE == GPIO_PIN_SET) ?
           GPIO_PIN_RESET : GPIO_PIN_SET;
}

static void ApplyLightMask(void)
{
    LightSource_t source;
    for (source = LIGHT_405; source < LIGHT_COUNT; ++source)
    {
        HAL_GPIO_WritePin(k_lights[source].port,
                          k_lights[source].pin,
                          OpticalLedState(
                              (uint8_t)((g_led_mask >> source) & 0x01U)));
    }
}

static void AllLightsOff(void)
{
    g_led_mask = 0U;
    ApplyLightMask();
}

static void SetLightEnabled(LightSource_t source, uint8_t enabled)
{
    if (source >= LIGHT_COUNT) return;
    if (enabled != 0U)
        g_led_mask |= (uint8_t)(1U << source);
    else
        g_led_mask &= (uint8_t)~(1U << source);
    ApplyLightMask();
}

static void SetLight(LightSource_t source)
{
    AllLightsOff();
    if (source < LIGHT_COUNT) SetLightEnabled(source, 1U);
}

static void SendLedStatus(void)
{
    ProtocolSendPayload("LEDSTAT,MASK=%02X,405=%u,WHITE=%u,850=%u,940=%u",
                        (unsigned int)g_led_mask,
                        (unsigned int)((g_led_mask >> LIGHT_405) & 1U),
                        (unsigned int)((g_led_mask >> LIGHT_WHITE) & 1U),
                        (unsigned int)((g_led_mask >> LIGHT_850) & 1U),
                        (unsigned int)((g_led_mask >> LIGHT_940) & 1U));
}

static HAL_StatusTypeDef ReadAdcAverage(uint16_t *raw)
{
    uint32_t sum = 0U;
    uint8_t index;

    if (raw == NULL) return HAL_ERROR;
    /* NTC 变化很慢，多次平均可压低 ADC 抖动，不影响光谱测量节拍。 */
    for (index = 0U; index < BOARD_ADC_AVERAGE_SAMPLES; ++index)
    {
        if (HAL_ADC_Start(&BOARD_NTC_ADC_HANDLE) != HAL_OK) return HAL_ERROR;
        if (HAL_ADC_PollForConversion(&BOARD_NTC_ADC_HANDLE, 20U) != HAL_OK)
        {
            (void)HAL_ADC_Stop(&BOARD_NTC_ADC_HANDLE);
            return HAL_TIMEOUT;
        }
        sum += HAL_ADC_GetValue(&BOARD_NTC_ADC_HANDLE);
        (void)HAL_ADC_Stop(&BOARD_NTC_ADC_HANDLE);
        HAL_Delay(1U);
    }
    *raw = (uint16_t)(sum / BOARD_ADC_AVERAGE_SAMPLES);
    return HAL_OK;
}

static int16_t ResistanceToTemperatureX10(uint32_t resistance_ohm)
{
    uint32_t index;

    if (resistance_ohm >= k_ntc_table[0].resistance_ohm)
        return (int16_t)(k_ntc_table[0].temperature_c * 10);
    if (resistance_ohm <=
        k_ntc_table[ARRAY_SIZE(k_ntc_table) - 1U].resistance_ohm)
    {
        return (int16_t)(k_ntc_table[ARRAY_SIZE(k_ntc_table) - 1U]
                         .temperature_c * 10);
    }

    /* 查表后在相邻 5 °C 标定点之间做线性插值。 */
    for (index = 0U; index < ARRAY_SIZE(k_ntc_table) - 1U; ++index)
    {
        uint32_t r_high = k_ntc_table[index].resistance_ohm;
        uint32_t r_low = k_ntc_table[index + 1U].resistance_ohm;
        if ((resistance_ohm <= r_high) && (resistance_ohm >= r_low))
        {
            int32_t t_high = k_ntc_table[index].temperature_c * 10;
            int32_t delta_t =
                (k_ntc_table[index + 1U].temperature_c -
                 k_ntc_table[index].temperature_c) * 10;
            return (int16_t)(t_high +
                (int32_t)(((r_high - resistance_ohm) *
                           (uint32_t)delta_t) / (r_high - r_low)));
        }
    }
    return 250;
}

static BoardTemperature_t ReadTemperatureDetailed(void)
{
    BoardTemperature_t result;

    memset(&result, 0, sizeof(result));
    result.temperature_x10 = INT16_MIN;
    result.status = ReadAdcAverage(&result.raw);
    if (result.status != HAL_OK) return result;

    result.millivolts =
        ((uint32_t)result.raw * BOARD_ADC_REFERENCE_MV) /
        BOARD_ADC_FULL_SCALE;
    if ((result.raw == 0U) || (result.raw >= BOARD_ADC_FULL_SCALE))
    {
        result.status = HAL_ERROR;
        return result;
    }

    result.resistance_ohm =
        ((uint32_t)BOARD_NTC_FIXED_OHM * result.raw) /
        (BOARD_ADC_FULL_SCALE - result.raw);
    result.temperature_x10 =
        ResistanceToTemperatureX10(result.resistance_ohm);
    return result;
}

static int16_t ReadTemperatureX10(void)
{
    return ReadTemperatureDetailed().temperature_x10;
}

static void SendTemperature(void)
{
    BoardTemperature_t result = ReadTemperatureDetailed();
    if (result.status != HAL_OK)
    {
        ProtocolSendPayload("TEMP,STATUS=ERROR,RAW=%u,MV=%lu",
                            (unsigned int)result.raw,
                            (unsigned long)result.millivolts);
        return;
    }
    ProtocolSendPayload("TEMP,STATUS=OK,RAW=%u,MV=%lu,R_OHM=%lu,T_X10=%d",
                        (unsigned int)result.raw,
                        (unsigned long)result.millivolts,
                        (unsigned long)result.resistance_ohm,
                        (int)result.temperature_x10);
}

static void SendI2cScan(void)
{
    uint8_t address;
    uint8_t count = 0U;
    uint8_t found[8] = {0U};
    uint8_t stored = 0U;

    for (address = 1U; address < 0x7FU; ++address)
    {
        if (HAL_I2C_IsDeviceReady(&BOARD_SPECTRAL_I2C_HANDLE,
                                  (uint16_t)(address << 1),
                                  1U,
                                  8U) == HAL_OK)
        {
            ++count;
            if (stored < ARRAY_SIZE(found)) found[stored++] = address;
        }
    }

    ProtocolSendPayload("I2CSUM,COUNT=%u,HALERR=%08lX",
                        (unsigned int)count,
                        (unsigned long)HAL_I2C_GetError(
                            &BOARD_SPECTRAL_I2C_HANDLE));
    for (address = 0U; address < stored; ++address)
        ProtocolSendPayload("I2CSCAN,%02X", (unsigned int)found[address]);
}

static void SendBoardStatus(void)
{
    BoardTemperature_t temp = ReadTemperatureDetailed();
    ProtocolSendPayload("BOARD,UPTIME_MS=%lu,SYSCLK=%lu,HCLK=%lu,PCLK1=%lu,"
                        "STATUS_LED=%u,LED_MASK=%02X,SCL=%u,SDA=%u,"
                        "ADC_STATUS=%u,ADC_RAW=%u,TEMP_X10=%d",
                        (unsigned long)HAL_GetTick(),
                        (unsigned long)HAL_RCC_GetSysClockFreq(),
                        (unsigned long)HAL_RCC_GetHCLKFreq(),
                        (unsigned long)HAL_RCC_GetPCLK1Freq(),
                        (unsigned int)(HAL_GPIO_ReadPin(BOARD_STATUS_LED_PORT,
                                                       BOARD_STATUS_LED_PIN) ==
                                       GPIO_PIN_SET),
                        (unsigned int)g_led_mask,
                        (unsigned int)(HAL_GPIO_ReadPin(BOARD_I2C_SCL_PORT,
                                                       BOARD_I2C_SCL_PIN) ==
                                       GPIO_PIN_SET),
                        (unsigned int)(HAL_GPIO_ReadPin(BOARD_I2C_SDA_PORT,
                                                       BOARD_I2C_SDA_PIN) ==
                                       GPIO_PIN_SET),
                        (unsigned int)temp.status,
                        (unsigned int)temp.raw,
                        (int)temp.temperature_x10);
    SendLedStatus();
    SendTemperature();
}

static void SendChannels(void)
{
    char payload[BOARD_UART_TX_MAX];
    int length;
    uint8_t channel;
    uint8_t count = AS734X_GetChannelCount(&g_sensor);

    length = snprintf(payload, sizeof(payload), "CHANNELS");
    if (length < 0) return;
    for (channel = 0U; channel < count; ++channel)
    {
        int written = snprintf(&payload[length],
                               sizeof(payload) - (size_t)length,
                               ",%s",
                               AS734X_GetChannelName(&g_sensor, channel));
        if ((written < 0) ||
            ((size_t)written >= sizeof(payload) - (size_t)length))
            return;
        length += written;
    }
    ProtocolSendPrepared(payload, (uint16_t)length);
}

static void SendDetection(void)
{
    /* 候选型号与实际解析配置分开发送，避免把手动 profile 当成硬件证据。 */
    ProtocolSendPayload("SENSOR,STATUS=%s,FAMILY=%s,CANDIDATES=%s,"
                        "PROTOCOL=%s,PROFILE=%s,EFFECTIVE_PROFILE=%s,"
                        "PROFILE_AMBIGUOUS=%u,CONFIDENCE=%s,ADDR=%02X,"
                        "ID_RAW=%02X,ID_CODE=%02X,REV=%02X,AUX=%02X,"
                        "SIG92=%02X,SIG5A=%02X,SIGCFG0=%02X,SIGD6=%02X,"
                        "CHANNEL_COUNT=%u",
                        AS734X_StatusString(g_sensor_status),
                        AS734X_ModelString(g_sensor.model),
                        AS734X_ModelCandidatesString(g_sensor.model),
                        AS734X_ProtocolString(g_sensor.protocol),
                        AS734X_ProfileString(g_sensor.profile),
                        AS734X_EffectiveProfileString(&g_sensor),
                        (unsigned int)g_sensor.profile_ambiguous,
                        AS734X_ConfidenceString(g_sensor.confidence),
                        (unsigned int)g_sensor.address_7bit,
                        (unsigned int)g_sensor.id_raw,
                        (unsigned int)g_sensor.id_code,
                        (unsigned int)g_sensor.revision,
                        (unsigned int)g_sensor.auxiliary_id,
                        (unsigned int)g_sensor.signature_92,
                        (unsigned int)g_sensor.signature_5a,
                        (unsigned int)g_sensor.signature_cfg0,
                        (unsigned int)g_sensor.signature_cfg20,
                        (unsigned int)g_sensor.channel_count);
}

static void SendInfo(void)
{
    ProtocolSendPayload("INFO,FW=%s,PROTO=%s,MCU=STM32G030C8T6,"
                        "SENSOR_STATUS=%s,FAMILY=%s,CANDIDATES=%s,"
                        "SENSOR_PROTOCOL=%s,PROFILE=%s,EFFECTIVE_PROFILE=%s,"
                        "PROFILE_AMBIGUOUS=%u,CONFIDENCE=%s,ADDR=%02X,"
                        "ID=%02X,ID_CODE=%02X,REV=%02X,AUX=%02X,"
                        "AUTOGAIN=%u,GAIN=%u,GAIN_X1000=%lu,"
                        "ATIME=%u,ASTEP=%u,TINT_US=%lu,CHANNEL_COUNT=%u,"
                        "INIT_BUSY_WARN=%u,STATUSX=%02X,I2CERR=%08lX",
                        FW_VERSION,
                        PROTOCOL_VERSION,
                        AS734X_StatusString(g_sensor_status),
                        AS734X_ModelString(g_sensor.model),
                        AS734X_ModelCandidatesString(g_sensor.model),
                        AS734X_ProtocolString(g_sensor.protocol),
                        AS734X_ProfileString(g_sensor.profile),
                        AS734X_EffectiveProfileString(&g_sensor),
                        (unsigned int)g_sensor.profile_ambiguous,
                        AS734X_ConfidenceString(g_sensor.confidence),
                        (unsigned int)g_sensor.address_7bit,
                        (unsigned int)g_sensor.id_raw,
                        (unsigned int)g_sensor.id_code,
                        (unsigned int)g_sensor.revision,
                        (unsigned int)g_sensor.auxiliary_id,
                        (unsigned int)g_auto_gain_enabled,
                        (unsigned int)g_sensor.gain,
                        (unsigned long)AS734X_GetGainX1000(g_sensor.gain),
                        (unsigned int)g_sensor.atime,
                        (unsigned int)g_sensor.astep,
                        (unsigned long)AS734X_GetIntegrationTimeUs(&g_sensor),
                        (unsigned int)g_sensor.channel_count,
                        (unsigned int)g_sensor.init_busy_warning,
                        (unsigned int)g_sensor.last_statusx,
                        (unsigned long)g_sensor.last_hal_i2c_error);
    SendDetection();
    SendChannels();
}

static void SendDiagnostic(void)
{
    AS734X_Diagnostic_t d;
    AS734X_Status_t status = AS734X_Diagnose(&g_sensor, &d);

    ProtocolSendPayload("DIAG,STATUS=%s,FAMILY=%s,CANDIDATES=%s,"
                        "PROTOCOL=%s,PROFILE=%s,EFFECTIVE_PROFILE=%s,"
                        "PROFILE_AMBIGUOUS=%u,CONFIDENCE=%s,"
                        "I2C_ADDR=%02X,ADDR_READY=%u,ACKS=%u,"
                        "A1=%02X,A2=%02X,A3=%02X,A4=%02X,SCL=%u,SDA=%u,"
                        "HALERR=%08lX,ID_RAW=%02X,ID_CODE=%02X,REV=%02X,"
                        "AUX=%02X,SIG92=%02X,SIG5A=%02X,SIGCFG0=%02X,"
                        "SIGD6=%02X,ENABLE=%02X,ATIME=%02X,ASTEP=%04X,"
                        "CFG1=%02X,STATUS2=%02X,STATUSX=%02X,"
                        "INIT_READY=%u,PROTOCOL_OK=%u",
                        AS734X_StatusString(status),
                        AS734X_ModelString(d.model),
                        AS734X_ModelCandidatesString(d.model),
                        AS734X_ProtocolString(d.protocol),
                        AS734X_ProfileString(d.profile),
                        AS734X_EffectiveProfileString(&g_sensor),
                        (unsigned int)d.profile_ambiguous,
                        AS734X_ConfidenceString(d.confidence),
                        (unsigned int)d.address_7bit,
                        (unsigned int)d.address_ready,
                        (unsigned int)d.ack_count,
                        (unsigned int)d.ack_1,
                        (unsigned int)d.ack_2,
                        (unsigned int)d.ack_3,
                        (unsigned int)d.ack_4,
                        (unsigned int)d.scl_level,
                        (unsigned int)d.sda_level,
                        (unsigned long)d.hal_i2c_error,
                        (unsigned int)d.id_raw,
                        (unsigned int)d.id_code,
                        (unsigned int)d.revision,
                        (unsigned int)d.auxiliary_id,
                        (unsigned int)d.signature_92,
                        (unsigned int)d.signature_5a,
                        (unsigned int)d.signature_cfg0,
                        (unsigned int)d.signature_cfg20,
                        (unsigned int)d.enable,
                        (unsigned int)d.atime,
                        (unsigned int)d.astep,
                        (unsigned int)d.cfg1,
                        (unsigned int)d.status2,
                        (unsigned int)d.statusx,
                        (unsigned int)d.init_ready,
                        (unsigned int)d.protocol_ok);
}

static void SendAsConfig(void)
{
    AS734X_Diagnostic_t d;
    AS734X_Status_t status = AS734X_Diagnose(&g_sensor, &d);
    ProtocolSendPayload("ASCFG,STATUS=%s,FAMILY=%s,PROTOCOL=%s,"
                        "PROFILE=%s,EFFECTIVE_PROFILE=%s,"
                        "ENABLE=%02X,ATIME=%02X,ASTEP=%04X,CFG1=%02X,"
                        "STATUS2=%02X,STATUSX=%02X,ID=%02X,ID_CODE=%02X,"
                        "REV=%02X,AUX=%02X",
                        AS734X_StatusString(status),
                        AS734X_ModelString(g_sensor.model),
                        AS734X_ProtocolString(g_sensor.protocol),
                        AS734X_ProfileString(g_sensor.profile),
                        AS734X_EffectiveProfileString(&g_sensor),
                        (unsigned int)d.enable,
                        (unsigned int)d.atime,
                        (unsigned int)d.astep,
                        (unsigned int)d.cfg1,
                        (unsigned int)d.status2,
                        (unsigned int)d.statusx,
                        (unsigned int)d.id_raw,
                        (unsigned int)d.id_code,
                        (unsigned int)d.revision,
                        (unsigned int)d.auxiliary_id);
}

static void SendHelp(void)
{
    ProtocolSendPayload("HELP,PING|INFO|DETECT|BOARD|TEMP|I2CSCAN|DIAG|"
                        "MEASURE|READ|STREAM <ms>|STOP|REINIT|"
                        "LED STATUS|LED <405|WHITE|850|940> <ON|OFF>|"
                        "LED ALL OFF|LED CYCLE <ms>|"
                        "SET PROFILE <AUTO|AS7341|AS7341L|AS7343|AS7343L|TCS3448>|"
                        "SET AUTOGAIN <0|1>|SET GAIN <index>|"
                        "SET ATIME <0..255>|SET ASTEP <1..65534>|"
                        "AS INFO|AS CONFIG|AS REG READ <bank> <addr>|"
                        "AS REG WRITE <bank> <addr> <value>|"
                        "AS DUMP <bank> <start> <count>|AS RWTEST|"
                        "AS FORCEINIT|AS RESET|AS POWER <ON|OFF>|"
                        "AS SAMPLE FORCE");
}

static AS734X_Status_t SelectGainForSource(LightSource_t source,
                                            AS734X_Gain_t *gain)
{
    AS734X_Status_t status;
    AS734X_Sample_t probe;
    AS734X_Gain_t current;
    uint8_t iteration;
    uint8_t max_gain;

    if ((source >= LIGHT_COUNT) || (gain == NULL))
        return AS734X_ERROR_ARGUMENT;

    max_gain = AS734X_GetMaximumGainIndex(&g_sensor);
    current = g_source_gain[source];
    if ((uint8_t)current > max_gain) current = (AS734X_Gain_t)max_gain;

    if (g_auto_gain_enabled == 0U)
    {
        status = AS734X_SetGain(&g_sensor, current);
        if (status == AS734X_OK) *gain = current;
        return status;
    }

    /* 每种光源保存自己的增益；预采样最多调整四次，防止来回震荡拖慢整轮测量。 */
    SetLight(source);
    HAL_Delay(BOARD_LED_SETTLE_MS);
    for (iteration = 0U; iteration < MAX_AUTO_GAIN_ITERATIONS; ++iteration)
    {
        AS734X_Gain_t recommended;
        status = AS734X_SetGain(&g_sensor, current);
        if (status != AS734X_OK)
        {
            AllLightsOff();
            return status;
        }
        status = AS734X_ReadOne(&g_sensor, &probe);
        if (status != AS734X_OK)
        {
            AllLightsOff();
            return status;
        }
        recommended = AS734X_RecommendGain(&g_sensor, current, &probe);
        if (recommended == current) break;
        current = recommended;
    }

    AllLightsOff();
    g_source_gain[source] = current;
    *gain = current;
    return AS734X_SetGain(&g_sensor, current);
}

static AS734X_Status_t MeasureSource(LightSource_t source,
                                     MeasurementRecord_t *record)
{
    AS734X_Status_t status;
    AS734X_Gain_t selected_gain;

    if ((source >= LIGHT_COUNT) || (record == NULL))
        return AS734X_ERROR_ARGUMENT;

    memset(record, 0, sizeof(*record));
    record->source = source;
    record->sequence = g_sequence;

    status = SelectGainForSource(source, &selected_gain);
    if (status != AS734X_OK)
    {
        AllLightsOff();
        return status;
    }
    record->gain = selected_gain;

    /* 暗场和亮场必须使用同一积分时间、同一增益，净值才能直接相减。 */
    AllLightsOff();
    HAL_Delay(BOARD_DARK_SETTLE_MS);
    status = AS734X_ReadOne(&g_sensor, &record->dark);
    if (status != AS734X_OK) return status;

    /* 光源稳定延时与暗场稳定延时在 board_config.h 中统一配置。 */
    SetLight(source);
    HAL_Delay(BOARD_LED_SETTLE_MS);
    status = AS734X_ReadOne(&g_sensor, &record->light);
    AllLightsOff();
    if (status != AS734X_OK) return status;

    record->temperature_x10 = ReadTemperatureX10();
    return AS734X_OK;
}

static void SendMeasurement(const MeasurementRecord_t *record)
{
    char payload[BOARD_UART_TX_MAX];
    int length;
    uint8_t channel;
    uint8_t count;

    if ((record == NULL) || (record->source >= LIGHT_COUNT)) return;
    count = record->light.channel_count;

    length = snprintf(payload,
                      sizeof(payload),
                      "MEAS,%lu,%s,%u,%lu,%u,%u,%lu,%d,%02X,%02X",
                      (unsigned long)record->sequence,
                      k_lights[record->source].name,
                      (unsigned int)record->gain,
                      (unsigned long)AS734X_GetGainX1000(record->gain),
                      (unsigned int)g_sensor.atime,
                      (unsigned int)g_sensor.astep,
                      (unsigned long)AS734X_GetIntegrationTimeUs(&g_sensor),
                      (int)record->temperature_x10,
                      (unsigned int)record->light.flags,
                      (unsigned int)record->dark.flags);
    if ((length < 0) || ((size_t)length >= sizeof(payload))) return;

    /* 报文先放全部亮场，再放全部暗场；上位机按 CHANNELS 元数据解释顺序。 */
    for (channel = 0U; channel < count; ++channel)
    {
        int written = snprintf(&payload[length],
                               sizeof(payload) - (size_t)length,
                               ",%u",
                               (unsigned int)record->light.channels[channel]);
        if ((written < 0) ||
            ((size_t)written >= sizeof(payload) - (size_t)length)) return;
        length += written;
    }
    for (channel = 0U; channel < count; ++channel)
    {
        int written = snprintf(&payload[length],
                               sizeof(payload) - (size_t)length,
                               ",%u",
                               (unsigned int)record->dark.channels[channel]);
        if ((written < 0) ||
            ((size_t)written >= sizeof(payload) - (size_t)length)) return;
        length += written;
    }
    ProtocolSendPrepared(payload, (uint16_t)length);
}

static void RunFullMeasurement(void)
{
    LightSource_t source;

    if (g_sensor_status != AS734X_OK)
    {
        ProtocolSendPayload("ERR,SENSOR_NOT_READY,%s",
                            AS734X_StatusString(g_sensor_status));
        return;
    }

    /* 四路光源逐一完成“选增益－暗场－亮场”，任一路失败都立即关灯并终止本轮。 */
    ++g_sequence;
    ProtocolSendPayload("BEGIN,%lu", (unsigned long)g_sequence);
    for (source = LIGHT_405; source < LIGHT_COUNT; ++source)
    {
        MeasurementRecord_t record;
        AS734X_Status_t status = MeasureSource(source, &record);
        if (status != AS734X_OK)
        {
            AllLightsOff();
            ProtocolSendPayload("ERR,MEASURE,%lu,%s,%s",
                                (unsigned long)g_sequence,
                                k_lights[source].name,
                                AS734X_StatusString(status));
            return;
        }
        SendMeasurement(&record);
    }
    ProtocolSendPayload("END,%lu", (unsigned long)g_sequence);
}

static void ReadAmbientOnce(void)
{
    AS734X_Sample_t sample;
    AS734X_Status_t status;
    char payload[BOARD_UART_TX_MAX];
    int length;
    uint8_t channel;

    if (g_sensor_status != AS734X_OK)
    {
        ProtocolSendPayload("ERR,SENSOR_NOT_READY,%s",
                            AS734X_StatusString(g_sensor_status));
        return;
    }

    /* 环境光是独立快照，板载四路光源保持关闭。 */
    AllLightsOff();
    HAL_Delay(BOARD_DARK_SETTLE_MS);
    status = AS734X_ReadOne(&g_sensor, &sample);
    if (status != AS734X_OK)
    {
        ProtocolSendPayload("ERR,READ,%s", AS734X_StatusString(status));
        return;
    }

    length = snprintf(payload,
                      sizeof(payload),
                      "AMBIENT,%lu,%u,%lu,%u,%u,%lu,%02X",
                      (unsigned long)HAL_GetTick(),
                      (unsigned int)g_sensor.gain,
                      (unsigned long)AS734X_GetGainX1000(g_sensor.gain),
                      (unsigned int)g_sensor.atime,
                      (unsigned int)g_sensor.astep,
                      (unsigned long)AS734X_GetIntegrationTimeUs(&g_sensor),
                      (unsigned int)sample.flags);
    if ((length < 0) || ((size_t)length >= sizeof(payload))) return;

    for (channel = 0U; channel < sample.channel_count; ++channel)
    {
        int written = snprintf(&payload[length],
                               sizeof(payload) - (size_t)length,
                               ",%u",
                               (unsigned int)sample.channels[channel]);
        if ((written < 0) ||
            ((size_t)written >= sizeof(payload) - (size_t)length)) return;
        length += written;
    }
    ProtocolSendPrepared(payload, (uint16_t)length);
}

static char *NextToken(char **context)
{
    char *token;
    char *cursor;

    if ((context == NULL) || (*context == NULL)) return NULL;
    cursor = *context;
    while ((*cursor == ' ') || (*cursor == '\t')) ++cursor;
    if (*cursor == '\0')
    {
        *context = cursor;
        return NULL;
    }
    token = cursor;
    while ((*cursor != '\0') && (*cursor != ' ') && (*cursor != '\t'))
        ++cursor;
    if (*cursor != '\0') *cursor++ = '\0';
    *context = cursor;
    return token;
}

static void ToUpperAscii(char *text)
{
    if (text == NULL) return;
    while (*text != '\0')
    {
        *text = (char)toupper((unsigned char)*text);
        ++text;
    }
}

static uint8_t ParseUnsigned(const char *text,
                             unsigned long maximum,
                             unsigned long *value)
{
    char *end;
    unsigned long parsed;

    if ((text == NULL) || (value == NULL)) return 0U;
    parsed = strtoul(text, &end, 0);
    if ((end == text) || (*end != '\0') || (parsed > maximum)) return 0U;
    *value = parsed;
    return 1U;
}

static uint8_t ParseLightSource(const char *text, LightSource_t *source)
{
    uint8_t index;
    if ((text == NULL) || (source == NULL)) return 0U;
    for (index = 0U; index < LIGHT_COUNT; ++index)
    {
        if (strcmp(text, k_lights[index].name) == 0)
        {
            *source = (LightSource_t)index;
            return 1U;
        }
    }
    return 0U;
}

static uint8_t ParseSensorProfile(const char *text,
                                  AS734X_Profile_t *profile)
{
    if ((text == NULL) || (profile == NULL)) return 0U;
    if (strcmp(text, "AUTO") == 0) *profile = AS734X_PROFILE_AUTO;
    else if (strcmp(text, "AS7341") == 0) *profile = AS734X_PROFILE_AS7341;
    else if (strcmp(text, "AS7341L") == 0) *profile = AS734X_PROFILE_AS7341L;
    else if (strcmp(text, "AS7343") == 0) *profile = AS734X_PROFILE_AS7343;
    else if (strcmp(text, "AS7343L") == 0) *profile = AS734X_PROFILE_AS7343L;
    else if (strcmp(text, "TCS3448") == 0) *profile = AS734X_PROFILE_TCS3448;
    else return 0U;
    return 1U;
}

static void ProcessSetCommand(char *context)
{
    char *parameter = NextToken(&context);
    char *value_text = NextToken(&context);
    unsigned long value;
    uint8_t source;

    if ((parameter == NULL) || (value_text == NULL))
    {
        ProtocolSendPayload("ERR,SET_SYNTAX");
        return;
    }
    ToUpperAscii(parameter);

    if (strcmp(parameter, "PROFILE") == 0)
    {
        AS734X_Profile_t profile;
        AS734X_Status_t status;
        ToUpperAscii(value_text);
        if (!ParseSensorProfile(value_text, &profile))
        {
            ProtocolSendPayload("ERR,PROFILE_VALUE");
            return;
        }
        status = AS734X_SetProfile(&g_sensor, profile);
        ProtocolSendPayload("ACK,PROFILE,STATUS=%s,REQUEST=%s,"
                            "EFFECTIVE=%s,AMBIGUOUS=%u",
                            AS734X_StatusString(status),
                            AS734X_ProfileString(profile),
                            AS734X_EffectiveProfileString(&g_sensor),
                            (unsigned int)g_sensor.profile_ambiguous);
        if (status == AS734X_OK)
        {
            SendChannels();
            SendDetection();
        }
        return;
    }

    if (strcmp(parameter, "AUTOGAIN") == 0)
    {
        if (!ParseUnsigned(value_text, 1U, &value))
        {
            ProtocolSendPayload("ERR,AUTOGAIN_VALUE");
            return;
        }
        g_auto_gain_enabled = (uint8_t)value;
        ProtocolSendPayload("ACK,AUTOGAIN,%u",
                            (unsigned int)g_auto_gain_enabled);
        return;
    }

    if (strcmp(parameter, "GAIN") == 0)
    {
        if (!ParseUnsigned(value_text,
                           AS734X_GetMaximumGainIndex(&g_sensor),
                           &value))
        {
            ProtocolSendPayload("ERR,GAIN_VALUE,MAX=%u",
                                (unsigned int)AS734X_GetMaximumGainIndex(
                                    &g_sensor));
            return;
        }
        if (AS734X_SetGain(&g_sensor, (AS734X_Gain_t)value) != AS734X_OK)
        {
            ProtocolSendPayload("ERR,GAIN_SET");
            return;
        }
        for (source = 0U; source < LIGHT_COUNT; ++source)
            g_source_gain[source] = (AS734X_Gain_t)value;
        ProtocolSendPayload("ACK,GAIN,%lu", value);
        return;
    }

    if (strcmp(parameter, "ATIME") == 0)
    {
        AS734X_Status_t status;
        if (!ParseUnsigned(value_text, 255U, &value))
        {
            ProtocolSendPayload("ERR,ATIME_VALUE");
            return;
        }
        status = AS734X_SetTiming(&g_sensor,
                                  (uint8_t)value,
                                  g_sensor.astep);
        ProtocolSendPayload("ACK,ATIME,%lu,STATUS=%s",
                            value,
                            AS734X_StatusString(status));
        return;
    }

    if (strcmp(parameter, "ASTEP") == 0)
    {
        AS734X_Status_t status;
        if (!ParseUnsigned(value_text, 65534U, &value) || (value == 0U))
        {
            ProtocolSendPayload("ERR,ASTEP_VALUE");
            return;
        }
        status = AS734X_SetTiming(&g_sensor,
                                  g_sensor.atime,
                                  (uint16_t)value);
        ProtocolSendPayload("ACK,ASTEP,%lu,STATUS=%s",
                            value,
                            AS734X_StatusString(status));
        return;
    }

    ProtocolSendPayload("ERR,SET_UNKNOWN,%s", parameter);
}

static void ProcessLedCommand(char *context)
{
    char *target = NextToken(&context);
    char *state = NextToken(&context);
    LightSource_t source;

    if (target == NULL)
    {
        ProtocolSendPayload("ERR,LED_SYNTAX");
        return;
    }
    ToUpperAscii(target);

    if (strcmp(target, "STATUS") == 0)
    {
        SendLedStatus();
        return;
    }
    if (strcmp(target, "ALL") == 0)
    {
        if (state != NULL) ToUpperAscii(state);
        if ((state != NULL) && (strcmp(state, "OFF") == 0))
        {
            AllLightsOff();
            SendLedStatus();
        }
        else
            ProtocolSendPayload("ERR,LED_ALL_SYNTAX");
        return;
    }
    if (strcmp(target, "CYCLE") == 0)
    {
        unsigned long delay_ms = 500U;
        uint8_t index;
        if ((state != NULL) && !ParseUnsigned(state, 5000U, &delay_ms))
        {
            ProtocolSendPayload("ERR,LED_CYCLE_VALUE");
            return;
        }
        if (delay_ms < 50U) delay_ms = 50U;
        for (index = 0U; index < LIGHT_COUNT; ++index)
        {
            SetLight((LightSource_t)index);
            SendLedStatus();
            HAL_Delay((uint32_t)delay_ms);
        }
        AllLightsOff();
        SendLedStatus();
        ProtocolSendPayload("LEDTEST,STATUS=OK,DELAY_MS=%lu", delay_ms);
        return;
    }

    if (!ParseLightSource(target, &source) || (state == NULL))
    {
        ProtocolSendPayload("ERR,LED_SOURCE");
        return;
    }
    ToUpperAscii(state);
    if (strcmp(state, "ON") == 0) SetLightEnabled(source, 1U);
    else if (strcmp(state, "OFF") == 0) SetLightEnabled(source, 0U);
    else
    {
        ProtocolSendPayload("ERR,LED_STATE");
        return;
    }
    SendLedStatus();
}

static void SendAsRegister(uint8_t bank, uint8_t reg)
{
    uint8_t value = 0U;
    AS734X_Status_t status =
        AS734X_ReadRegister(&g_sensor, bank, reg, &value);
    ProtocolSendPayload("ASREG,OP=READ,STATUS=%s,FAMILY=%s,BANK=%u,"
                        "ADDR=%02X,VALUE=%02X",
                        AS734X_StatusString(status),
                        AS734X_ModelString(g_sensor.model),
                        (unsigned int)bank,
                        (unsigned int)reg,
                        (unsigned int)value);
}

static void WriteAsRegister(uint8_t bank, uint8_t reg, uint8_t value)
{
    uint8_t readback = 0U;
    AS734X_Status_t status =
        AS734X_WriteRegister(&g_sensor, bank, reg, value);
    if (status == AS734X_OK)
        status = AS734X_ReadRegister(&g_sensor, bank, reg, &readback);
    ProtocolSendPayload("ASREG,OP=WRITE,STATUS=%s,FAMILY=%s,BANK=%u,"
                        "ADDR=%02X,VALUE=%02X,READBACK=%02X",
                        AS734X_StatusString(status),
                        AS734X_ModelString(g_sensor.model),
                        (unsigned int)bank,
                        (unsigned int)reg,
                        (unsigned int)value,
                        (unsigned int)readback);
}

static void DumpAsRegisters(uint8_t bank, uint8_t start, uint8_t count)
{
    uint8_t offset;
    for (offset = 0U; offset < count; ++offset)
    {
        uint8_t address = (uint8_t)(start + offset);
        SendAsRegister(bank, address);
        if (address == 0xFFU) break;
    }
}

static void RunAsRwTest(void)
{
    uint8_t original_atime = g_sensor.atime;
    uint16_t original_astep = g_sensor.astep;
    AS734X_Gain_t original_gain = g_sensor.gain;
    AS734X_Status_t s1;
    AS734X_Status_t s2;
    AS734X_Status_t s3;
    AS734X_Status_t s4;
    AS734X_Gain_t test_gain =
        (original_gain == AS734X_GAIN_16X) ?
        AS734X_GAIN_8X : AS734X_GAIN_16X;

    s1 = AS734X_SetTiming(&g_sensor, 30U, 600U);
    s2 = AS734X_SetGain(&g_sensor, test_gain);
    s3 = AS734X_SetTiming(&g_sensor, original_atime, original_astep);
    s4 = AS734X_SetGain(&g_sensor, original_gain);
    ProtocolSendPayload("ASRWTEST,FAMILY=%s,TIMING_TEST=%s,GAIN_TEST=%s,"
                        "TIMING_RESTORE=%s,GAIN_RESTORE=%s",
                        AS734X_ModelString(g_sensor.model),
                        AS734X_StatusString(s1),
                        AS734X_StatusString(s2),
                        AS734X_StatusString(s3),
                        AS734X_StatusString(s4));
}

static void ProcessAsCommand(char *context)
{
    char *sub = NextToken(&context);

    if (sub == NULL)
    {
        ProtocolSendPayload("ERR,AS_SYNTAX");
        return;
    }
    ToUpperAscii(sub);

    if ((strcmp(sub, "INFO") == 0) || (strcmp(sub, "ID") == 0))
    {
        SendDetection();
        return;
    }
    if (strcmp(sub, "PROFILE") == 0)
    {
        char *value_text = NextToken(&context);
        if (value_text == NULL)
        {
            ProtocolSendPayload("ASPROFILE,REQUEST=%s,EFFECTIVE=%s,"
                                "AMBIGUOUS=%u",
                                AS734X_ProfileString(g_sensor.profile),
                                AS734X_EffectiveProfileString(&g_sensor),
                                (unsigned int)g_sensor.profile_ambiguous);
        }
        else
        {
            AS734X_Profile_t profile;
            AS734X_Status_t status;
            ToUpperAscii(value_text);
            if (!ParseSensorProfile(value_text, &profile))
            {
                ProtocolSendPayload("ERR,PROFILE_VALUE");
                return;
            }
            status = AS734X_SetProfile(&g_sensor, profile);
            ProtocolSendPayload("ASPROFILE,STATUS=%s,REQUEST=%s,"
                                "EFFECTIVE=%s,AMBIGUOUS=%u",
                                AS734X_StatusString(status),
                                AS734X_ProfileString(profile),
                                AS734X_EffectiveProfileString(&g_sensor),
                                (unsigned int)g_sensor.profile_ambiguous);
            if (status == AS734X_OK)
            {
                SendChannels();
                SendDetection();
            }
        }
        return;
    }
    if (strcmp(sub, "CONFIG") == 0)
    {
        SendAsConfig();
        return;
    }
    if (strcmp(sub, "REG") == 0)
    {
        char *operation = NextToken(&context);
        char *bank_text = NextToken(&context);
        char *reg_text = NextToken(&context);
        char *value_text = NextToken(&context);
        unsigned long bank;
        unsigned long reg;
        unsigned long value;
        if ((operation == NULL) || (bank_text == NULL) ||
            (reg_text == NULL) ||
            !ParseUnsigned(bank_text, 1U, &bank) ||
            !ParseUnsigned(reg_text, 255U, &reg))
        {
            ProtocolSendPayload("ERR,AS_REG_SYNTAX");
            return;
        }
        ToUpperAscii(operation);
        if (strcmp(operation, "READ") == 0)
            SendAsRegister((uint8_t)bank, (uint8_t)reg);
        else if ((strcmp(operation, "WRITE") == 0) &&
                 (value_text != NULL) &&
                 ParseUnsigned(value_text, 255U, &value))
            WriteAsRegister((uint8_t)bank, (uint8_t)reg, (uint8_t)value);
        else
            ProtocolSendPayload("ERR,AS_REG_OPERATION");
        return;
    }
    if (strcmp(sub, "DUMP") == 0)
    {
        char *bank_text = NextToken(&context);
        char *start_text = NextToken(&context);
        char *count_text = NextToken(&context);
        unsigned long bank;
        unsigned long start;
        unsigned long count;
        if (!ParseUnsigned(bank_text, 1U, &bank) ||
            !ParseUnsigned(start_text, 255U, &start) ||
            !ParseUnsigned(count_text, AS_TEST_MAX_DUMP_COUNT, &count) ||
            (count == 0U))
        {
            ProtocolSendPayload("ERR,AS_DUMP_SYNTAX");
            return;
        }
        DumpAsRegisters((uint8_t)bank, (uint8_t)start, (uint8_t)count);
        return;
    }
    if (strcmp(sub, "RWTEST") == 0)
    {
        RunAsRwTest();
        return;
    }
    if ((strcmp(sub, "FORCEINIT") == 0) ||
        (strcmp(sub, "REINIT") == 0))
    {
        g_sensor_status = AS734X_Reinitialize(&g_sensor);
        ProtocolSendPayload("ASFORCEINIT,STATUS=%s,FAMILY=%s,PROTOCOL=%s",
                            AS734X_StatusString(g_sensor_status),
                            AS734X_ModelString(g_sensor.model),
                            AS734X_ProtocolString(g_sensor.protocol));
        SendInfo();
        return;
    }
    if (strcmp(sub, "RESET") == 0)
    {
        g_sensor_status = AS734X_Reset(&g_sensor);
        ProtocolSendPayload("ASRESET,STATUS=%s,FAMILY=%s,PROTOCOL=%s",
                            AS734X_StatusString(g_sensor_status),
                            AS734X_ModelString(g_sensor.model),
                            AS734X_ProtocolString(g_sensor.protocol));
        SendInfo();
        return;
    }
    if (strcmp(sub, "SAMPLE") == 0)
    {
        ReadAmbientOnce();
        return;
    }
    if (strcmp(sub, "POWER") == 0)
    {
        char *state = NextToken(&context);
        uint8_t value;
        AS734X_Status_t status;
        if (state == NULL)
        {
            ProtocolSendPayload("ERR,AS_POWER_SYNTAX");
            return;
        }
        ToUpperAscii(state);
        if (strcmp(state, "ON") == 0) value = 0x01U;
        else if (strcmp(state, "OFF") == 0) value = 0x00U;
        else
        {
            ProtocolSendPayload("ERR,AS_POWER_VALUE");
            return;
        }
        status = AS734X_WriteRegister(&g_sensor, 0U, 0x80U, value);
        ProtocolSendPayload("ASPOWER,STATUS=%s,REQUEST=%s",
                            AS734X_StatusString(status), state);
        return;
    }

    ProtocolSendPayload("ERR,AS_UNKNOWN,%s", sub);
}

static void ProcessCommand(char *command_line)
{
    char *context = command_line;
    char *command = NextToken(&context);

    /* 命令名不区分大小写，参数边界在各子命令中再次检查。 */
    if (command == NULL) return;
    ToUpperAscii(command);

    if (strcmp(command, "PING") == 0)
        ProtocolSendPayload("PONG,%lu", (unsigned long)HAL_GetTick());
    else if (strcmp(command, "HELP") == 0)
        SendHelp();
    else if (strcmp(command, "INFO") == 0)
        SendInfo();
    else if (strcmp(command, "DETECT") == 0)
    {
        AllLightsOff();
        g_sensor_status = AS734X_Init(&g_sensor,
                                      &BOARD_SPECTRAL_I2C_HANDLE);
        ProtocolSendPayload("ACK,DETECT,%s",
                            AS734X_StatusString(g_sensor_status));
        SendInfo();
    }
    else if (strcmp(command, "BOARD") == 0)
        SendBoardStatus();
    else if ((strcmp(command, "TEMP") == 0) ||
             (strcmp(command, "ADC") == 0))
        SendTemperature();
    else if (strcmp(command, "I2CSCAN") == 0)
        SendI2cScan();
    else if ((strcmp(command, "DIAG") == 0) ||
             (strcmp(command, "BUS") == 0))
        SendDiagnostic();
    else if (strcmp(command, "MEASURE") == 0)
    {
        g_stream_enabled = 0U;
        RunFullMeasurement();
    }
    else if (strcmp(command, "READ") == 0)
        ReadAmbientOnce();
    else if (strcmp(command, "STREAM") == 0)
    {
        char *interval_text = NextToken(&context);
        unsigned long interval = DEFAULT_STREAM_MS;
        if ((interval_text != NULL) &&
            !ParseUnsigned(interval_text, 3600000U, &interval))
        {
            ProtocolSendPayload("ERR,STREAM_VALUE");
            return;
        }
        if (interval < MIN_STREAM_MS) interval = MIN_STREAM_MS;
        g_stream_interval_ms = (uint32_t)interval;
        g_stream_enabled = 1U;
        g_next_stream_ms = HAL_GetTick();
        ProtocolSendPayload("ACK,STREAM,%lu", interval);
    }
    else if (strcmp(command, "STOP") == 0)
    {
        g_stream_enabled = 0U;
        AllLightsOff();
        ProtocolSendPayload("ACK,STOP");
    }
    else if (strcmp(command, "REINIT") == 0)
    {
        AllLightsOff();
        g_sensor_status = AS734X_Reinitialize(&g_sensor);
        ProtocolSendPayload("ACK,REINIT,%s",
                            AS734X_StatusString(g_sensor_status));
        SendInfo();
    }
    else if (strcmp(command, "SET") == 0)
        ProcessSetCommand(context);
    else if (strcmp(command, "LED") == 0)
        ProcessLedCommand(context);
    else if (strcmp(command, "AS") == 0)
        ProcessAsCommand(context);
    else
        ProtocolSendPayload("ERR,UNKNOWN_COMMAND,%s", command);
}

void SpectralApp_UartRxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart != &BOARD_CONSOLE_UART_HANDLE) return;

    /* 中断中只拼接一行命令；解析和回包都放到主循环，避免阻塞串口中断。 */
    if ((g_uart_rx_byte == '\r') || (g_uart_rx_byte == '\n'))
    {
        if ((g_uart_build_length > 0U) && (g_uart_command_ready == 0U))
        {
            uint16_t index;
            for (index = 0U; index < g_uart_build_length; ++index)
                g_uart_command_buffer[index] = g_uart_build_buffer[index];
            g_uart_command_buffer[g_uart_build_length] = '\0';
            g_uart_command_ready = 1U;
        }
        g_uart_build_length = 0U;
    }
    else if (g_uart_build_length < (BOARD_UART_COMMAND_MAX - 1U))
        g_uart_build_buffer[g_uart_build_length++] = (char)g_uart_rx_byte;
    else
        g_uart_build_length = 0U;

    (void)HAL_UART_Receive_IT(&BOARD_CONSOLE_UART_HANDLE,
                              &g_uart_rx_byte,
                              1U);
}

void SpectralApp_UartErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart != &BOARD_CONSOLE_UART_HANDLE) return;
    __HAL_UART_CLEAR_OREFLAG(huart);
    (void)HAL_UART_Receive_IT(&BOARD_CONSOLE_UART_HANDLE,
                              &g_uart_rx_byte,
                              1U);
}

void SpectralApp_Init(void)
{
    /* 上电先确保四路光源关闭，再校准 ADC 和识别光谱传感器。 */
    AllLightsOff();
    (void)HAL_ADCEx_Calibration_Start(&BOARD_NTC_ADC_HANDLE);
    HAL_GPIO_WritePin(BOARD_STATUS_LED_PORT,
                      BOARD_STATUS_LED_PIN,
                      (BOARD_STATUS_LED_ACTIVE == GPIO_PIN_SET) ?
                      GPIO_PIN_RESET : GPIO_PIN_SET);

    HAL_Delay(300U);
    g_sensor_status = AS734X_Init(&g_sensor,
                                  &BOARD_SPECTRAL_I2C_HANDLE);

    HAL_NVIC_SetPriority(USART1_IRQn, 1U, 0U);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    __HAL_UART_CLEAR_OREFLAG(&BOARD_CONSOLE_UART_HANDLE);
    (void)HAL_UART_Receive_IT(&BOARD_CONSOLE_UART_HANDLE,
                              &g_uart_rx_byte,
                              1U);

    ConsoleRaw("\r\n");
    ProtocolSendPayload("BOOT,FW=%s,PROTO=%s,SENSOR_STATUS=%s,"
                        "FAMILY=%s,PROTOCOL_NAME=%s",
                        FW_VERSION,
                        PROTOCOL_VERSION,
                        AS734X_StatusString(g_sensor_status),
                        AS734X_ModelString(g_sensor.model),
                        AS734X_ProtocolString(g_sensor.protocol));
    SendInfo();
    if (g_sensor_status != AS734X_OK)
    {
        ProtocolSendPayload("ERR,SENSOR_INIT,%s",
                            AS734X_StatusString(g_sensor_status));
        SendDiagnostic();
    }
    SendHelp();
    g_last_heartbeat_ms = HAL_GetTick();
}

void SpectralApp_Process(void)
{
    uint32_t now = HAL_GetTick();

    if ((now - g_last_heartbeat_ms) >= 500U)
    {
        g_last_heartbeat_ms = now;
        HAL_GPIO_TogglePin(BOARD_STATUS_LED_PORT, BOARD_STATUS_LED_PIN);
    }

    /* 复制命令时短暂关中断，避免接收中断改写正在解析的缓冲区。 */
    if (g_uart_command_ready != 0U)
    {
        char local_command[BOARD_UART_COMMAND_MAX];
        uint16_t index;

        __disable_irq();
        for (index = 0U; index < BOARD_UART_COMMAND_MAX; ++index)
        {
            local_command[index] = g_uart_command_buffer[index];
            if (local_command[index] == '\0') break;
        }
        local_command[BOARD_UART_COMMAND_MAX - 1U] = '\0';
        g_uart_command_ready = 0U;
        __enable_irq();
        ProcessCommand(local_command);
    }

    now = HAL_GetTick();
    /* 使用有符号差值处理 HAL_GetTick() 回绕。 */
    if ((g_stream_enabled != 0U) &&
        ((int32_t)(now - g_next_stream_ms) >= 0))
    {
        RunFullMeasurement();
        g_next_stream_ms = HAL_GetTick() + g_stream_interval_ms;
    }
}
