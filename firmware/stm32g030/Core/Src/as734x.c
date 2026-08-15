#include "as734x.h"
#include "board_config.h"

#include <string.h>

/*
 * 传感器驱动只处理寄存器协议和通道整理，不直接控制板上的光源。
 * AS7341 使用手动 SMUX，AS7343 与 TCS3448 使用自动 SMUX；二者的原始数据
 * 最终都整理成按波段命名的连续通道，应用层无需了解 ADC 槽位顺序。
 */

#define ADDRESS_HAL(address_7bit)       ((uint16_t)((address_7bit) << 1))
#define I2C_TIMEOUT_MS                  150U
#define DEVICE_READY_RETRIES            20U
#define POR_GUARD_MS                     20U
#define PON_SETTLE_MS                    3U
#define INIT_BUSY_TIMEOUT_MS             15U
#define SMUX_TIMEOUT_MS                 100U
#define MEASUREMENT_MARGIN_MS           250U
#define MEASUREMENT_MAX_MS             2500U

#define REG_ENABLE                       0x80U
#define REG_ATIME                        0x81U
#define REG_STATUS                       0x93U
#define REG_ASTATUS                      0x94U
#define REG_DATA0_L                      0x95U
#define REG_CONTROL                      0xFAU

#define ENABLE_FDEN                      0x40U
#define ENABLE_SMUXEN                    0x10U
#define ENABLE_WEN                       0x08U
#define ENABLE_SP_EN                     0x02U
#define ENABLE_PON                       0x01U

#define STATUS2_AVALID                   0x40U
#define STATUS2_DIGITAL_SAT              0x10U
#define STATUS2_ANALOG_SAT               0x08U
#define ASTATUS_SAT                      0x80U
#define REG_BANK_BIT                     0x10U

/* AS7341 / AS7341L 寄存器表。 */
#define A1_REG_AUXID                     0x90U
#define A1_REG_REVID                     0x91U
#define A1_REG_ID                        0x92U
#define A1_REG_STATUS2                   0xA3U
#define A1_REG_STATUS6                   0xA7U
#define A1_REG_CFG0                      0xA9U
#define A1_REG_CFG1                      0xAAU
#define A1_REG_CFG6                      0xAFU
#define A1_REG_ASTEP_L                   0xCAU
#define A1_REG_AZ_CONFIG                 0xD6U
#define A1_ID_CODE                       0x09U
#define A1_CFG6_SMUX_CMD_MASK            0x18U
#define A1_CFG6_SMUX_CMD_WRITE           0x10U
#define A1_MAX_GAIN_INDEX                10U

/* AS7343 / AS7343L / TCS3448 兼容寄存器表。 */
#define A3_REG_AUXID                     0x58U
#define A3_REG_REVID                     0x59U
#define A3_REG_ID                        0x5AU
#define A3_REG_STATUS2                   0x90U
#define A3_REG_STATUS4                   0xBCU
#define A3_REG_CFG0                      0xBFU
#define A3_REG_CFG1                      0xC6U
#define A3_REG_ASTEP_L                   0xD4U
#define A3_REG_CFG20                     0xD6U
#define A3_ID_RAW                        0x81U
#define A3_CFG20_AUTO_SMUX_MASK          0x60U
#define A3_CFG20_AUTO_SMUX_18            0x60U
#define A3_CONTROL_SW_RESET              0x08U
#define A3_MAX_GAIN_INDEX                12U

static const uint32_t k_gain_x1000[AS734X_GAIN_COUNT] =
{
    500U, 1000U, 2000U, 4000U, 8000U, 16000U, 32000U,
    64000U, 128000U, 256000U, 512000U, 1024000U, 2048000U
};

static const char *const k_a1_channel_names[10] =
{
    "F1_415", "F2_445", "F3_480", "F4_515", "F5_555",
    "F6_590", "F7_630", "F8_680", "CLEAR", "NIR_910"
};

static const uint16_t k_a1_channel_nm[10] =
{
    415U, 445U, 480U, 515U, 555U,
    590U, 630U, 680U, 0U, 910U
};

static const char *const k_a3_channel_names[14] =
{
    "F1_405", "F2_425", "FZ_450", "F3_475", "F4_515",
    "F5_550", "FY_555", "FXL_600", "F6_640", "F7_690",
    "F8_745", "NIR_855", "CLEAR", "FD_RAW"
};

static const uint16_t k_a3_channel_nm[14] =
{
    405U, 425U, 450U, 475U, 515U, 550U, 555U,
    600U, 640U, 690U, 745U, 855U, 0U, 0U
};

/*
 * TCS3448 的寄存器协议与 AS7343 兼容，但量产标定的滤光峰值不同。
 * 单独保留通道元数据，避免上位机把 TCS3448 数据误标成 AS7343 波长。
 */
static const char *const k_tcs3448_channel_names[14] =
{
    "F1_407", "F2_424", "FZ_450", "F3_473", "F4_516",
    "F5_546", "FY_560", "FXL_596", "F6_636", "F7_687",
    "F8_748", "NIR_855", "CLEAR", "FD_RAW"
};

static const uint16_t k_tcs3448_channel_nm[14] =
{
    407U, 424U, 450U, 473U, 516U, 546U, 560U,
    596U, 636U, 687U, 748U, 855U, 0U, 0U
};

static const char *const k_a3l_channel_names[13] =
{
    "F1_405", "F2_425", "FZ_450", "F3_475", "F4_515",
    "F5_550", "FY_555", "FXL_600", "F6_640", "F7_690",
    "F8_745", "NIR_855", "CLEAR"
};

static const uint16_t k_a3l_channel_nm[13] =
{
    405U, 425U, 450U, 475U, 515U, 550U, 555U,
    600U, 640U, 690U, 745U, 855U, 0U
};

/*
 * AS7341 标准参考 SMUX 配置。每轮把四个滤光通道以及 CLEAR、NIR 接到六路 ADC。
 */
static const uint8_t k_a1_smux_f1_f4[20] =
{
    0x30U, 0x01U, 0x00U, 0x00U, 0x00U,
    0x42U, 0x00U, 0x00U, 0x50U, 0x00U,
    0x00U, 0x00U, 0x20U, 0x04U, 0x00U,
    0x30U, 0x01U, 0x50U, 0x00U, 0x06U
};

static const uint8_t k_a1_smux_f5_f8[20] =
{
    0x00U, 0x00U, 0x00U, 0x40U, 0x02U,
    0x00U, 0x10U, 0x03U, 0x50U, 0x10U,
    0x03U, 0x00U, 0x00U, 0x00U, 0x24U,
    0x00U, 0x00U, 0x50U, 0x00U, 0x06U
};

static void CaptureI2cError(AS734X_Device_t *device)
{
    if ((device != NULL) && (device->i2c != NULL))
    {
        device->last_hal_i2c_error = HAL_I2C_GetError(device->i2c);
    }
}

static AS734X_Status_t WaitDeviceReady(AS734X_Device_t *device)
{
    uint8_t attempt;

    if ((device == NULL) || (device->i2c == NULL))
    {
        return AS734X_ERROR_ARGUMENT;
    }

    for (attempt = 0U; attempt < DEVICE_READY_RETRIES; ++attempt)
    {
        if (HAL_I2C_IsDeviceReady(device->i2c,
                                  ADDRESS_HAL(device->address_7bit),
                                  2U,
                                  I2C_TIMEOUT_MS) == HAL_OK)
        {
            device->last_hal_i2c_error = HAL_I2C_ERROR_NONE;
            return AS734X_OK;
        }
        CaptureI2cError(device);
        HAL_Delay(5U);
    }

    return AS734X_ERROR_I2C;
}

static AS734X_Status_t RawWrite(AS734X_Device_t *device,
                                uint8_t reg,
                                const uint8_t *data,
                                uint16_t length)
{
    if ((device == NULL) || (device->i2c == NULL) ||
        (data == NULL) || (length == 0U))
    {
        return AS734X_ERROR_ARGUMENT;
    }

    if (HAL_I2C_Mem_Write(device->i2c,
                          ADDRESS_HAL(device->address_7bit),
                          reg,
                          I2C_MEMADD_SIZE_8BIT,
                          (uint8_t *)data,
                          length,
                          I2C_TIMEOUT_MS) != HAL_OK)
    {
        CaptureI2cError(device);
        return AS734X_ERROR_I2C;
    }

    device->last_hal_i2c_error = HAL_I2C_ERROR_NONE;
    return AS734X_OK;
}

static AS734X_Status_t RawRead(AS734X_Device_t *device,
                               uint8_t reg,
                               uint8_t *data,
                               uint16_t length)
{
    if ((device == NULL) || (device->i2c == NULL) ||
        (data == NULL) || (length == 0U))
    {
        return AS734X_ERROR_ARGUMENT;
    }

    /* HAL 的内存读取先写寄存器地址，再用重复起始条件切换到读方向。 */
    if (HAL_I2C_Mem_Read(device->i2c,
                         ADDRESS_HAL(device->address_7bit),
                         reg,
                         I2C_MEMADD_SIZE_8BIT,
                         data,
                         length,
                         I2C_TIMEOUT_MS) != HAL_OK)
    {
        CaptureI2cError(device);
        return AS734X_ERROR_I2C;
    }

    device->last_hal_i2c_error = HAL_I2C_ERROR_NONE;
    return AS734X_OK;
}

static AS734X_Status_t RawWriteByte(AS734X_Device_t *device,
                                    uint8_t reg,
                                    uint8_t value)
{
    return RawWrite(device, reg, &value, 1U);
}

static AS734X_Status_t RawReadByte(AS734X_Device_t *device,
                                   uint8_t reg,
                                   uint8_t *value)
{
    return RawRead(device, reg, value, 1U);
}

static AS734X_Status_t ReadModifyWriteRaw(AS734X_Device_t *device,
                                          uint8_t reg,
                                          uint8_t mask,
                                          uint8_t value)
{
    uint8_t current = 0U;
    AS734X_Status_t status = RawReadByte(device, reg, &current);

    if (status != AS734X_OK)
    {
        return status;
    }

    current = (uint8_t)((current & (uint8_t)(~mask)) | (value & mask));
    return RawWriteByte(device, reg, current);
}

static uint8_t FamilyCfg0Register(const AS734X_Device_t *device)
{
    if ((device != NULL) &&
        (device->protocol == AS734X_PROTOCOL_AS7341_SMUX))
    {
        return A1_REG_CFG0;
    }
    return A3_REG_CFG0;
}

static AS734X_Status_t SelectBank(AS734X_Device_t *device,
                                  uint8_t bank,
                                  uint8_t *saved_cfg0)
{
    uint8_t cfg0_reg;
    uint8_t current = 0U;
    uint8_t requested;
    uint8_t verify = 0U;
    AS734X_Status_t status;

    if ((device == NULL) ||
        ((device->protocol != AS734X_PROTOCOL_AS7341_SMUX) &&
         (device->protocol != AS734X_PROTOCOL_AS7343_AUTO_SMUX)))
    {
        return AS734X_ERROR_UNSUPPORTED;
    }

    cfg0_reg = FamilyCfg0Register(device);
    status = RawReadByte(device, cfg0_reg, &current);
    if (status != AS734X_OK)
    {
        return status;
    }

    if (saved_cfg0 != NULL)
    {
        *saved_cfg0 = current;
    }

    requested = (bank != 0U) ?
                (uint8_t)(current | REG_BANK_BIT) :
                (uint8_t)(current & (uint8_t)(~REG_BANK_BIT));

    if (requested != current)
    {
        status = RawWriteByte(device, cfg0_reg, requested);
        if (status != AS734X_OK)
        {
            return status;
        }
        HAL_Delay(1U);
    }

    status = RawReadByte(device, cfg0_reg, &verify);
    if (status != AS734X_OK)
    {
        return status;
    }

    if ((((verify & REG_BANK_BIT) != 0U) ? 1U : 0U) !=
        ((bank != 0U) ? 1U : 0U))
    {
        return AS734X_ERROR_VERIFY;
    }

    return AS734X_OK;
}

static AS734X_Status_t RestoreBank0(AS734X_Device_t *device)
{
    return SelectBank(device, 0U, NULL);
}

static AS734X_Status_t ReadStableByte(AS734X_Device_t *device,
                                      uint8_t reg,
                                      uint8_t *value)
{
    uint8_t a = 0U;
    uint8_t b = 0U;
    uint8_t c = 0U;
    AS734X_Status_t status;

    status = RawReadByte(device, reg, &a);
    if (status != AS734X_OK) return status;
    HAL_Delay(1U);
    status = RawReadByte(device, reg, &b);
    if (status != AS734X_OK) return status;
    HAL_Delay(1U);
    status = RawReadByte(device, reg, &c);
    if (status != AS734X_OK) return status;

    if ((a == b) || (a == c))
    {
        *value = a;
        return AS734X_OK;
    }
    if (b == c)
    {
        *value = b;
        return AS734X_OK;
    }

    *value = c;
    return AS734X_ERROR_BUS_DATA;
}

static AS734X_Status_t DetectAs7341Family(AS734X_Device_t *device)
{
    uint8_t id = 0U;
    AS734X_Status_t status;

    status = ReadStableByte(device, A1_REG_ID, &id);
    device->signature_92 = id;
    if ((status != AS734X_OK) && (status != AS734X_ERROR_BUS_DATA))
    {
        return status;
    }

    if ((uint8_t)(id >> 2) != A1_ID_CODE)
    {
        return AS734X_ERROR_ID;
    }

    device->model = AS734X_MODEL_AS7341_FAMILY;
    device->protocol = AS734X_PROTOCOL_AS7341_SMUX;
    device->profile = AS734X_PROFILE_AUTO;
    device->profile_ambiguous = 1U;
    device->confidence = AS734X_CONFIDENCE_HIGH;
    device->id_raw = id;
    device->id_code = (uint8_t)(id >> 2);
    device->channel_count = 10U;

    (void)RawReadByte(device, A1_REG_REVID, &device->revision);
    device->revision &= 0x07U;
    (void)RawReadByte(device, A1_REG_AUXID, &device->auxiliary_id);
    (void)RawReadByte(device, A1_REG_CFG0, &device->signature_cfg0);
    (void)RawReadByte(device, A1_REG_AZ_CONFIG, &device->signature_cfg20);
    return AS734X_OK;
}

static AS734X_Status_t DetectAs7343Family(AS734X_Device_t *device)
{
    uint8_t cfg0 = 0U;
    uint8_t bank1_cfg0 = 0U;
    uint8_t id = 0U;
    AS734X_Status_t status;
    AS734X_Status_t restore_status;

    device->protocol = AS734X_PROTOCOL_AS7343_AUTO_SMUX;
    status = RawReadByte(device, A3_REG_CFG0, &cfg0);
    if (status != AS734X_OK)
    {
        return status;
    }
    device->signature_cfg0 = cfg0;

    bank1_cfg0 = (uint8_t)(cfg0 | REG_BANK_BIT);
    status = RawWriteByte(device, A3_REG_CFG0, bank1_cfg0);
    if (status != AS734X_OK)
    {
        return status;
    }
    HAL_Delay(1U);

    status = ReadStableByte(device, A3_REG_ID, &id);
    device->signature_5a = id;
    if ((status == AS734X_OK) || (status == AS734X_ERROR_BUS_DATA))
    {
        (void)RawReadByte(device, A3_REG_REVID, &device->revision);
        device->revision &= 0x07U;
        (void)RawReadByte(device, A3_REG_AUXID, &device->auxiliary_id);
        device->auxiliary_id &= 0x0FU;
    }

    restore_status = RawWriteByte(device,
                                  A3_REG_CFG0,
                                  (uint8_t)(cfg0 &
                                            (uint8_t)(~REG_BANK_BIT)));
    HAL_Delay(1U);

    if ((status != AS734X_OK) && (status != AS734X_ERROR_BUS_DATA))
    {
        return status;
    }
    if (restore_status != AS734X_OK)
    {
        return restore_status;
    }
    if (id != A3_ID_RAW)
    {
        return AS734X_ERROR_ID;
    }

    device->model = AS734X_MODEL_AS7343_FAMILY;
    device->protocol = AS734X_PROTOCOL_AS7343_AUTO_SMUX;
    device->profile = AS734X_PROFILE_AUTO;
    device->profile_ambiguous = 1U;
    device->confidence = AS734X_CONFIDENCE_HIGH;
    device->id_raw = id;
    device->id_code = id;
    device->channel_count = 14U;
    (void)RawReadByte(device, A3_REG_CFG20, &device->signature_cfg20);
    return AS734X_OK;
}

AS734X_Status_t AS734X_Detect(AS734X_Device_t *device,
                              I2C_HandleTypeDef *i2c)
{
    AS734X_Status_t status_a1 = AS734X_ERROR_I2C;
    AS734X_Status_t status_a3 = AS734X_ERROR_I2C;
    uint8_t address_39_ready = 0U;
    uint8_t address_59_ready = 0U;
    uint8_t probe_92 = 0U;
    uint8_t probe_5a = 0U;
    uint8_t probe_cfg0 = 0U;
    uint8_t probe_cfg20 = 0U;

    if ((device == NULL) || (i2c == NULL))
    {
        return AS734X_ERROR_ARGUMENT;
    }

    memset(device, 0, sizeof(*device));
    device->i2c = i2c;
    device->model = AS734X_MODEL_NONE;
    device->protocol = AS734X_PROTOCOL_NONE;

    HAL_Delay(POR_GUARD_MS);

    /* AS7341/AS7341L 与 AS7343/AS7343L 都使用 0x39。 */
    device->address_7bit = AS734X_I2C_ADDRESS_7BIT;
    if (WaitDeviceReady(device) == AS734X_OK)
    {
        address_39_ready = 1U;

        /*
         * 先检查 AS7341 的只读身份寄存器，避免在 AS7341 上试写 0xBF；
         * 该地址在两套寄存器表中的含义不同。
         */
        status_a1 = DetectAs7341Family(device);
        probe_92 = device->signature_92;
        if (status_a1 == AS734X_OK)
        {
            return AS734X_OK;
        }

        /* 清除协议族暂存字段，但保留刚读到的 0x92 签名供诊断使用。 */
        device->model = AS734X_MODEL_NONE;
        device->protocol = AS734X_PROTOCOL_NONE;
        device->confidence = AS734X_CONFIDENCE_NONE;
        device->id_raw = 0U;
        device->id_code = 0U;
        device->revision = 0U;
        device->auxiliary_id = 0U;

        status_a3 = DetectAs7343Family(device);
        probe_5a = device->signature_5a;
        probe_cfg0 = device->signature_cfg0;
        probe_cfg20 = device->signature_cfg20;
        if (status_a3 == AS734X_OK)
        {
            device->model = AS734X_MODEL_AS7343_FAMILY;
            return AS734X_OK;
        }
    }

    /* TCS3448 使用 AS7343 寄存器协议，但器件地址为 0x59。 */
    memset(device, 0, sizeof(*device));
    device->i2c = i2c;
    device->address_7bit = TCS3448_I2C_ADDRESS_7BIT;
    device->model = AS734X_MODEL_NONE;
    device->protocol = AS734X_PROTOCOL_NONE;
    if (WaitDeviceReady(device) == AS734X_OK)
    {
        address_59_ready = 1U;
        status_a3 = DetectAs7343Family(device);
        if (status_a3 == AS734X_OK)
        {
            device->model = AS734X_MODEL_TCS3448;
            device->profile = AS734X_PROFILE_TCS3448;
            device->profile_ambiguous = 0U;
            device->confidence = AS734X_CONFIDENCE_HIGH;
            return AS734X_OK;
        }
    }

    memset(device, 0, sizeof(*device));
    device->i2c = i2c;
    device->address_7bit = (address_39_ready != 0U) ?
                           AS734X_I2C_ADDRESS_7BIT :
                           ((address_59_ready != 0U) ?
                            TCS3448_I2C_ADDRESS_7BIT :
                            AS734X_I2C_ADDRESS_7BIT);
    device->model = ((address_39_ready != 0U) ||
                     (address_59_ready != 0U)) ?
                    AS734X_MODEL_UNKNOWN_ADDRESS : AS734X_MODEL_NONE;
    device->protocol = AS734X_PROTOCOL_RAW_I2C;
    device->profile = AS734X_PROFILE_AUTO;
    device->profile_ambiguous = (device->model == AS734X_MODEL_UNKNOWN_ADDRESS) ? 1U : 0U;
    device->confidence = (device->model == AS734X_MODEL_UNKNOWN_ADDRESS) ?
                         AS734X_CONFIDENCE_LOW : AS734X_CONFIDENCE_NONE;
    device->channel_count = 0U;

    /*
     * 身份校验失败时仍保留探测值。现场诊断需要用这些值区分“地址有应答但 ID
     * 不认识”和“连线或地址不对”，不能被上面的清零操作一并抹掉。
     */
    if (address_39_ready != 0U)
    {
        device->signature_92 = probe_92;
        device->signature_5a = probe_5a;
        device->signature_cfg0 = probe_cfg0;
        device->signature_cfg20 = probe_cfg20;
    }

    return ((address_39_ready != 0U) || (address_59_ready != 0U)) ?
           AS734X_ERROR_ID : AS734X_ERROR_I2C;
}

AS734X_Profile_t AS734X_GetEffectiveProfile(const AS734X_Device_t *device)
{
    if (device == NULL) return AS734X_PROFILE_AUTO;
    if (device->profile != AS734X_PROFILE_AUTO) return device->profile;

    switch (device->model)
    {
        case AS734X_MODEL_AS7341_FAMILY: return AS734X_PROFILE_AS7341;
        case AS734X_MODEL_AS7343_FAMILY: return AS734X_PROFILE_AS7343;
        case AS734X_MODEL_TCS3448:       return AS734X_PROFILE_TCS3448;
        default:                         return AS734X_PROFILE_AUTO;
    }
}

AS734X_Status_t AS734X_SetProfile(AS734X_Device_t *device,
                                    AS734X_Profile_t profile)
{
    if (device == NULL) return AS734X_ERROR_ARGUMENT;

    switch (device->model)
    {
        case AS734X_MODEL_AS7341_FAMILY:
            if ((profile != AS734X_PROFILE_AUTO) &&
                (profile != AS734X_PROFILE_AS7341) &&
                (profile != AS734X_PROFILE_AS7341L))
            {
                return AS734X_ERROR_UNSUPPORTED;
            }
            device->profile = profile;
            device->profile_ambiguous =
                (profile == AS734X_PROFILE_AUTO) ? 1U : 0U;
            device->channel_count = 10U;
            return AS734X_OK;

        case AS734X_MODEL_AS7343_FAMILY:
            if ((profile != AS734X_PROFILE_AUTO) &&
                (profile != AS734X_PROFILE_AS7343) &&
                (profile != AS734X_PROFILE_AS7343L))
            {
                return AS734X_ERROR_UNSUPPORTED;
            }
            device->profile = profile;
            device->profile_ambiguous =
                (profile == AS734X_PROFILE_AUTO) ? 1U : 0U;
            device->channel_count =
                (AS734X_GetEffectiveProfile(device) ==
                 AS734X_PROFILE_AS7343L) ? 13U : 14U;
            return AS734X_OK;

        case AS734X_MODEL_TCS3448:
            if ((profile != AS734X_PROFILE_AUTO) &&
                (profile != AS734X_PROFILE_TCS3448))
            {
                return AS734X_ERROR_UNSUPPORTED;
            }
            device->profile =
                (profile == AS734X_PROFILE_AUTO) ?
                AS734X_PROFILE_TCS3448 : profile;
            device->profile_ambiguous = 0U;
            device->channel_count = 14U;
            return AS734X_OK;

        default:
            return AS734X_ERROR_UNSUPPORTED;
    }
}

static AS734X_Status_t EnsureIdlePowered(AS734X_Device_t *device)
{
    uint8_t enable = 0U;
    uint8_t requested;
    uint8_t verify = 0U;
    uint8_t was_off;
    AS734X_Status_t status;

    if ((device == NULL) ||
        ((device->protocol != AS734X_PROTOCOL_AS7341_SMUX) &&
         (device->protocol != AS734X_PROTOCOL_AS7343_AUTO_SMUX)))
    {
        return AS734X_ERROR_UNSUPPORTED;
    }

    status = RestoreBank0(device);
    if (status != AS734X_OK) return status;

    status = RawReadByte(device, REG_ENABLE, &enable);
    if (status != AS734X_OK) return status;

    was_off = ((enable & ENABLE_PON) == 0U) ? 1U : 0U;
    requested = (uint8_t)((enable | ENABLE_PON) &
                          (uint8_t)(~(ENABLE_SP_EN | ENABLE_SMUXEN |
                                      ENABLE_WEN | ENABLE_FDEN)));
    if (requested != enable)
    {
        status = RawWriteByte(device, REG_ENABLE, requested);
        if (status != AS734X_OK) return status;
    }

    if (was_off != 0U)
    {
        HAL_Delay(PON_SETTLE_MS);
    }

    status = RawReadByte(device, REG_ENABLE, &verify);
    if (status != AS734X_OK) return status;
    device->last_enable = verify;

    if (((verify & ENABLE_PON) == 0U) ||
        ((verify & (ENABLE_SP_EN | ENABLE_SMUXEN)) != 0U))
    {
        return AS734X_ERROR_VERIFY;
    }

    return AS734X_OK;
}

static AS734X_Status_t WaitInitBusy(AS734X_Device_t *device,
                                    uint32_t timeout_ms)
{
    uint8_t reg;
    uint8_t statusx = 1U;
    uint32_t start = HAL_GetTick();
    AS734X_Status_t status;

    if (device->protocol == AS734X_PROTOCOL_AS7341_SMUX)
    {
        reg = A1_REG_STATUS6;
    }
    else if (device->protocol == AS734X_PROTOCOL_AS7343_AUTO_SMUX)
    {
        reg = A3_REG_STATUS4;
    }
    else
    {
        return AS734X_ERROR_UNSUPPORTED;
    }

    do
    {
        status = RawReadByte(device, reg, &statusx);
        if (status != AS734X_OK) return status;
        device->last_statusx = statusx;
        if ((statusx & 0x01U) == 0U) return AS734X_OK;
        HAL_Delay(1U);
    }
    while ((HAL_GetTick() - start) < timeout_ms);

    return AS734X_ERROR_TIMEOUT;
}

AS734X_Status_t AS734X_SetTiming(AS734X_Device_t *device,
                                 uint8_t atime,
                                 uint16_t astep)
{
    uint8_t astep_reg;
    uint8_t bytes[2];
    uint8_t verify[2] = {0U, 0U};
    uint8_t verify_atime = 0U;
    AS734X_Status_t status;

    if ((device == NULL) || ((atime == 0U) && (astep == 0U)) ||
        (astep == 0xFFFFU))
    {
        return AS734X_ERROR_ARGUMENT;
    }

    status = EnsureIdlePowered(device);
    if (status != AS734X_OK) return status;

    astep_reg = (device->protocol == AS734X_PROTOCOL_AS7341_SMUX) ?
                A1_REG_ASTEP_L : A3_REG_ASTEP_L;

    status = RawWriteByte(device, REG_ATIME, atime);
    if (status != AS734X_OK) return status;

    bytes[0] = (uint8_t)(astep & 0xFFU);
    bytes[1] = (uint8_t)(astep >> 8);
    status = RawWrite(device, astep_reg, bytes, 2U);
    if (status != AS734X_OK) return status;

    status = RawReadByte(device, REG_ATIME, &verify_atime);
    if (status != AS734X_OK) return status;
    status = RawRead(device, astep_reg, verify, 2U);
    if (status != AS734X_OK) return status;

    if ((verify_atime != atime) ||
        (verify[0] != bytes[0]) || (verify[1] != bytes[1]))
    {
        return AS734X_ERROR_VERIFY;
    }

    device->atime = atime;
    device->astep = astep;
    return AS734X_OK;
}

AS734X_Status_t AS734X_SetGain(AS734X_Device_t *device,
                               AS734X_Gain_t gain)
{
    uint8_t cfg1_reg;
    uint8_t verify = 0U;
    AS734X_Status_t status;

    if ((device == NULL) || ((uint8_t)gain >= AS734X_GAIN_COUNT) ||
        ((uint8_t)gain > AS734X_GetMaximumGainIndex(device)))
    {
        return AS734X_ERROR_ARGUMENT;
    }

    status = EnsureIdlePowered(device);
    if (status != AS734X_OK) return status;

    cfg1_reg = (device->protocol == AS734X_PROTOCOL_AS7341_SMUX) ?
               A1_REG_CFG1 : A3_REG_CFG1;

    status = ReadModifyWriteRaw(device,
                                cfg1_reg,
                                0x1FU,
                                (uint8_t)gain);
    if (status != AS734X_OK) return status;

    status = RawReadByte(device, cfg1_reg, &verify);
    if (status != AS734X_OK) return status;
    if ((verify & 0x1FU) != (uint8_t)gain)
    {
        return AS734X_ERROR_VERIFY;
    }

    device->gain = gain;
    return AS734X_OK;
}

static AS734X_Status_t ConfigureAs7343AutoSmux(AS734X_Device_t *device)
{
    uint8_t verify = 0U;
    AS734X_Status_t status;

    status = ReadModifyWriteRaw(device,
                                A3_REG_CFG20,
                                A3_CFG20_AUTO_SMUX_MASK,
                                A3_CFG20_AUTO_SMUX_18);
    if (status != AS734X_OK) return status;

    status = RawReadByte(device, A3_REG_CFG20, &verify);
    if (status != AS734X_OK) return status;
    device->signature_cfg20 = verify;

    return ((verify & A3_CFG20_AUTO_SMUX_MASK) ==
            A3_CFG20_AUTO_SMUX_18) ? AS734X_OK : AS734X_ERROR_VERIFY;
}

static AS734X_Status_t ConfigureDetectedDevice(AS734X_Device_t *device)
{
    AS734X_Status_t status;
    AS734X_Status_t busy_status;

    status = EnsureIdlePowered(device);
    if (status != AS734X_OK) return status;

    busy_status = WaitInitBusy(device, INIT_BUSY_TIMEOUT_MS);
    if ((busy_status != AS734X_OK) &&
        (busy_status != AS734X_ERROR_TIMEOUT))
    {
        return busy_status;
    }
    device->init_busy_warning =
        (busy_status == AS734X_ERROR_TIMEOUT) ? 1U : 0U;

    status = AS734X_SetTiming(device, 29U, 599U);
    if (status != AS734X_OK) return status;

    status = AS734X_SetGain(device, AS734X_GAIN_16X);
    if (status != AS734X_OK) return status;

    if (device->protocol == AS734X_PROTOCOL_AS7343_AUTO_SMUX)
    {
        status = ConfigureAs7343AutoSmux(device);
        if (status != AS734X_OK) return status;
    }

    return AS734X_OK;
}

AS734X_Status_t AS734X_Init(AS734X_Device_t *device,
                            I2C_HandleTypeDef *i2c)
{
    AS734X_Status_t status = AS734X_Detect(device, i2c);

    if (status != AS734X_OK)
    {
        return status;
    }

    return ConfigureDetectedDevice(device);
}

AS734X_Status_t AS734X_Reinitialize(AS734X_Device_t *device)
{
    I2C_HandleTypeDef *i2c;
    AS734X_Profile_t requested_profile;
    AS734X_Status_t status;

    if ((device == NULL) || (device->i2c == NULL))
    {
        return AS734X_ERROR_ARGUMENT;
    }

    i2c = device->i2c;
    requested_profile = device->profile;
    status = AS734X_Init(device, i2c);
    if ((status == AS734X_OK) &&
        (requested_profile != AS734X_PROFILE_AUTO))
    {
        AS734X_Status_t profile_status =
            AS734X_SetProfile(device, requested_profile);
        if (profile_status != AS734X_OK) return profile_status;
    }
    return status;
}

static uint32_t MeasurementTimeoutMs(const AS734X_Device_t *device)
{
    uint32_t integration_us = AS734X_GetIntegrationTimeUs(device);
    uint32_t timeout_ms = (integration_us + 999U) / 1000U;

    timeout_ms += MEASUREMENT_MARGIN_MS;
    if (timeout_ms < 350U) timeout_ms = 350U;
    if (timeout_ms > MEASUREMENT_MAX_MS) timeout_ms = MEASUREMENT_MAX_MS;
    return timeout_ms;
}

static void ApplySampleFlags(AS734X_Sample_t *sample,
                             uint8_t astat,
                             uint8_t status2)
{
    sample->astat |= astat;
    sample->status2 |= status2;
    if ((astat & ASTATUS_SAT) != 0U)
        sample->flags |= AS734X_FLAG_ASTATUS_SAT;
    if ((status2 & STATUS2_DIGITAL_SAT) != 0U)
        sample->flags |= AS734X_FLAG_DIGITAL_SAT;
    if ((status2 & STATUS2_ANALOG_SAT) != 0U)
        sample->flags |= AS734X_FLAG_ANALOG_SAT;
}

static AS734X_Status_t WaitAvalid(AS734X_Device_t *device,
                                  uint8_t status2_reg,
                                  uint8_t *status2)
{
    uint32_t start = HAL_GetTick();
    uint32_t timeout_ms = MeasurementTimeoutMs(device);
    AS734X_Status_t status;

    do
    {
        status = RawReadByte(device, status2_reg, status2);
        if (status != AS734X_OK) return status;
        device->last_status2 = *status2;
        if ((*status2 & STATUS2_AVALID) != 0U) return AS734X_OK;
        HAL_Delay(1U);
    }
    while ((HAL_GetTick() - start) < timeout_ms);

    return AS734X_ERROR_TIMEOUT;
}

static AS734X_Status_t As7341LoadSmux(AS734X_Device_t *device,
                                      const uint8_t *configuration)
{
    uint8_t enable = 0U;
    uint8_t cfg6 = 0U;
    uint32_t start;
    AS734X_Status_t status;

    status = EnsureIdlePowered(device);
    if (status != AS734X_OK) return status;

    status = RawReadByte(device, A1_REG_CFG6, &cfg6);
    if (status != AS734X_OK) return status;
    cfg6 = (uint8_t)((cfg6 & (uint8_t)(~A1_CFG6_SMUX_CMD_MASK)) |
                     A1_CFG6_SMUX_CMD_WRITE);
    status = RawWriteByte(device, A1_REG_CFG6, cfg6);
    if (status != AS734X_OK) return status;

    status = RawWrite(device, 0x00U, configuration, 20U);
    if (status != AS734X_OK) return status;

    status = RawReadByte(device, REG_ENABLE, &enable);
    if (status != AS734X_OK) return status;
    status = RawWriteByte(device,
                          REG_ENABLE,
                          (uint8_t)(enable | ENABLE_SMUXEN));
    if (status != AS734X_OK) return status;

    start = HAL_GetTick();
    do
    {
        status = RawReadByte(device, REG_ENABLE, &enable);
        if (status != AS734X_OK) return status;
        if ((enable & ENABLE_SMUXEN) == 0U) return AS734X_OK;
        HAL_Delay(1U);
    }
    while ((HAL_GetTick() - start) < SMUX_TIMEOUT_MS);

    return AS734X_ERROR_TIMEOUT;
}

static AS734X_Status_t As7341ReadSix(AS734X_Device_t *device,
                                     const uint8_t *smux,
                                     uint16_t values[6],
                                     uint8_t *astat,
                                     uint8_t *status2)
{
    uint8_t enable = 0U;
    uint8_t latched[13];
    uint8_t index;
    AS734X_Status_t status;

    status = As7341LoadSmux(device, smux);
    if (status != AS734X_OK) return status;

    status = RawReadByte(device, REG_ENABLE, &enable);
    if (status != AS734X_OK) return status;
    enable = (uint8_t)((enable | ENABLE_PON) &
                       (uint8_t)(~ENABLE_SMUXEN));
    status = RawWriteByte(device,
                          REG_ENABLE,
                          (uint8_t)(enable | ENABLE_SP_EN));
    if (status != AS734X_OK) return status;

    status = WaitAvalid(device, A1_REG_STATUS2, status2);
    if (status != AS734X_OK)
    {
        (void)RawWriteByte(device,
                           REG_ENABLE,
                           (uint8_t)(enable & (uint8_t)(~ENABLE_SP_EN)));
        return status;
    }

    /* 从 ASTATUS 开始连续读取，可把状态和六路 ADC 锁存在同一个采样时刻。 */
    status = RawRead(device, REG_ASTATUS, latched, sizeof(latched));
    (void)RawWriteByte(device,
                       REG_ENABLE,
                       (uint8_t)(enable & (uint8_t)(~ENABLE_SP_EN)));
    if (status != AS734X_OK) return status;

    *astat = latched[0];
    for (index = 0U; index < 6U; ++index)
    {
        uint8_t offset = (uint8_t)(1U + index * 2U);
        values[index] = (uint16_t)((uint16_t)latched[offset] |
                                   ((uint16_t)latched[offset + 1U] << 8));
    }

    return AS734X_OK;
}

static AS734X_Status_t ReadAs7341(AS734X_Device_t *device,
                                  AS734X_Sample_t *sample)
{
    uint16_t low[6] = {0U};
    uint16_t high[6] = {0U};
    uint8_t astat_low = 0U;
    uint8_t astat_high = 0U;
    uint8_t status2_low = 0U;
    uint8_t status2_high = 0U;
    AS734X_Status_t status;

    memset(sample, 0, sizeof(*sample));
    sample->channel_count = 10U;

    status = As7341ReadSix(device,
                           k_a1_smux_f1_f4,
                           low,
                           &astat_low,
                           &status2_low);
    if (status != AS734X_OK) return status;

    status = As7341ReadSix(device,
                           k_a1_smux_f5_f8,
                           high,
                           &astat_high,
                           &status2_high);
    if (status != AS734X_OK) return status;

    sample->channels[0] = low[0];
    sample->channels[1] = low[1];
    sample->channels[2] = low[2];
    sample->channels[3] = low[3];
    sample->channels[4] = high[0];
    sample->channels[5] = high[1];
    sample->channels[6] = high[2];
    sample->channels[7] = high[3];
    sample->channels[8] = (uint16_t)(((uint32_t)low[4] + high[4] + 1U) / 2U);
    sample->channels[9] = (uint16_t)(((uint32_t)low[5] + high[5] + 1U) / 2U);

    ApplySampleFlags(sample, astat_low, status2_low);
    ApplySampleFlags(sample, astat_high, status2_high);
    return AS734X_OK;
}

static void MapAs7343Raw(AS734X_Sample_t *sample,
                         const uint16_t raw[18])
{
    uint32_t clear_sum;
    uint32_t fd_sum;

    sample->channels[2]  = raw[0];   /* FZ */
    sample->channels[6]  = raw[1];   /* FY */
    sample->channels[7]  = raw[2];   /* FXL */
    sample->channels[11] = raw[3];   /* NIR */

    sample->channels[1]  = raw[6];   /* F2 */
    sample->channels[3]  = raw[7];   /* F3 */
    sample->channels[4]  = raw[8];   /* F4 */
    sample->channels[8]  = raw[9];   /* F6 */

    sample->channels[0]  = raw[12];  /* F1 */
    sample->channels[9]  = raw[13];  /* F7 */
    sample->channels[10] = raw[14];  /* F8 */
    sample->channels[5]  = raw[15];  /* F5 */

    clear_sum = (uint32_t)raw[4] + raw[10] + raw[16];
    fd_sum = (uint32_t)raw[5] + raw[11] + raw[17];
    sample->channels[12] = (uint16_t)((clear_sum + 1U) / 3U);
    sample->channels[13] = (uint16_t)((fd_sum + 1U) / 3U);
}

static void MapAs7343LRaw(AS734X_Sample_t *sample,
                          const uint16_t raw[18])
{
    uint32_t clear_sum;

    sample->channels[2]  = raw[0];   /* FZ */
    sample->channels[6]  = raw[1];   /* FY */
    sample->channels[7]  = raw[2];   /* FXL */
    sample->channels[11] = raw[3];   /* NIR */

    sample->channels[1]  = raw[6];   /* F2 */
    sample->channels[3]  = raw[7];   /* F3 */
    sample->channels[4]  = raw[8];   /* F4 */
    sample->channels[8]  = raw[9];   /* F6 */

    sample->channels[0]  = raw[12];  /* F1 */
    sample->channels[5]  = raw[13];  /* F5 */
    sample->channels[9]  = raw[14];  /* F7 */
    sample->channels[10] = raw[15];  /* F8 */

    /* AS7343L 每轮第 5 槽明确为 CLEAR，第 4 槽是 2xVIS。 */
    clear_sum = (uint32_t)raw[5] + raw[11] + raw[17];
    sample->channels[12] = (uint16_t)((clear_sum + 1U) / 3U);
}

static AS734X_Status_t ReadAs7343(AS734X_Device_t *device,
                                  AS734X_Sample_t *sample)
{
    uint8_t enable = 0U;
    uint8_t status2 = 0U;
    uint8_t latched[37];
    uint16_t raw[18];
    uint8_t index;
    AS734X_Status_t status;

    memset(sample, 0, sizeof(*sample));
    sample->channel_count =
        (AS734X_GetEffectiveProfile(device) == AS734X_PROFILE_AS7343L) ?
        13U : 14U;

    status = EnsureIdlePowered(device);
    if (status != AS734X_OK) return status;

    status = RawReadByte(device, REG_ENABLE, &enable);
    if (status != AS734X_OK) return status;
    enable = (uint8_t)((enable | ENABLE_PON) &
                       (uint8_t)(~(ENABLE_SMUXEN | ENABLE_SP_EN)));

    status = RawWriteByte(device,
                          REG_ENABLE,
                          (uint8_t)(enable | ENABLE_SP_EN));
    if (status != AS734X_OK) return status;

    status = WaitAvalid(device, A3_REG_STATUS2, &status2);
    if (status != AS734X_OK)
    {
        (void)RawReadByte(device, A3_REG_STATUS4, &device->last_statusx);
        (void)RawWriteByte(device, REG_ENABLE, enable);
        return status;
    }

    status = RawRead(device, REG_ASTATUS, latched, sizeof(latched));
    (void)RawWriteByte(device, REG_ENABLE, enable);
    if (status != AS734X_OK) return status;

    for (index = 0U; index < 18U; ++index)
    {
        uint8_t offset = (uint8_t)(1U + index * 2U);
        raw[index] = (uint16_t)((uint16_t)latched[offset] |
                                ((uint16_t)latched[offset + 1U] << 8));
    }

    ApplySampleFlags(sample, latched[0], status2);
    if (AS734X_GetEffectiveProfile(device) == AS734X_PROFILE_AS7343L)
        MapAs7343LRaw(sample, raw);
    else
        MapAs7343Raw(sample, raw);
    return AS734X_OK;
}

AS734X_Status_t AS734X_ReadOne(AS734X_Device_t *device,
                               AS734X_Sample_t *sample)
{
    if ((device == NULL) || (sample == NULL))
    {
        return AS734X_ERROR_ARGUMENT;
    }

    if (device->protocol == AS734X_PROTOCOL_AS7341_SMUX)
    {
        return ReadAs7341(device, sample);
    }
    if (device->protocol == AS734X_PROTOCOL_AS7343_AUTO_SMUX)
    {
        return ReadAs7343(device, sample);
    }

    return AS734X_ERROR_UNSUPPORTED;
}

AS734X_Status_t AS734X_Reset(AS734X_Device_t *device)
{
    I2C_HandleTypeDef *i2c;
    AS734X_Status_t status;

    if ((device == NULL) || (device->i2c == NULL))
    {
        return AS734X_ERROR_ARGUMENT;
    }
    i2c = device->i2c;

    if (device->protocol == AS734X_PROTOCOL_AS7343_AUTO_SMUX)
    {
        status = EnsureIdlePowered(device);
        if (status != AS734X_OK) return status;
        status = RawWriteByte(device, REG_CONTROL, A3_CONTROL_SW_RESET);
        if (status != AS734X_OK) return status;
        HAL_Delay(60U);
    }
    else if (device->protocol == AS734X_PROTOCOL_AS7341_SMUX)
    {
        (void)RestoreBank0(device);
        status = RawWriteByte(device, REG_ENABLE, 0x00U);
        if (status != AS734X_OK) return status;
        HAL_Delay(20U);
    }
    else
    {
        return AS734X_ERROR_UNSUPPORTED;
    }

    {
        AS734X_Profile_t requested_profile = device->profile;
        status = AS734X_Init(device, i2c);
        if ((status == AS734X_OK) &&
            (requested_profile != AS734X_PROFILE_AUTO))
        {
            AS734X_Status_t profile_status =
                AS734X_SetProfile(device, requested_profile);
            if (profile_status != AS734X_OK) return profile_status;
        }
        return status;
    }
}

AS734X_Status_t AS734X_ReadRegister(AS734X_Device_t *device,
                                    uint8_t bank,
                                    uint8_t reg,
                                    uint8_t *value)
{
    AS734X_Status_t status;

    if ((device == NULL) || (value == NULL))
    {
        return AS734X_ERROR_ARGUMENT;
    }

    if ((device->protocol == AS734X_PROTOCOL_AS7341_SMUX) ||
        (device->protocol == AS734X_PROTOCOL_AS7343_AUTO_SMUX))
    {
        status = SelectBank(device, bank, NULL);
        if (status != AS734X_OK) return status;
        status = RawReadByte(device, reg, value);
        (void)RestoreBank0(device);
        return status;
    }

    if (bank != 0U) return AS734X_ERROR_UNSUPPORTED;
    return RawReadByte(device, reg, value);
}

AS734X_Status_t AS734X_WriteRegister(AS734X_Device_t *device,
                                     uint8_t bank,
                                     uint8_t reg,
                                     uint8_t value)
{
    AS734X_Status_t status;

    if (device == NULL)
    {
        return AS734X_ERROR_ARGUMENT;
    }

    if ((device->protocol == AS734X_PROTOCOL_AS7341_SMUX) ||
        (device->protocol == AS734X_PROTOCOL_AS7343_AUTO_SMUX))
    {
        status = SelectBank(device, bank, NULL);
        if (status != AS734X_OK) return status;
        status = RawWriteByte(device, reg, value);
        (void)RestoreBank0(device);
        return status;
    }

    if (bank != 0U) return AS734X_ERROR_UNSUPPORTED;
    return RawWriteByte(device, reg, value);
}

AS734X_Status_t AS734X_Diagnose(AS734X_Device_t *device,
                                AS734X_Diagnostic_t *diagnostic)
{
    uint8_t address;
    uint8_t astep_reg = 0U;
    uint8_t cfg1_reg = 0U;
    uint8_t status2_reg = 0U;
    uint8_t statusx_reg = 0U;
    uint8_t astep_bytes[2] = {0U, 0U};
    AS734X_Status_t init_status;

    if ((device == NULL) || (device->i2c == NULL) ||
        (diagnostic == NULL))
    {
        return AS734X_ERROR_ARGUMENT;
    }

    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->address_7bit = device->address_7bit;
    diagnostic->scl_level =
        (HAL_GPIO_ReadPin(BOARD_I2C_SCL_PORT,
                          BOARD_I2C_SCL_PIN) == GPIO_PIN_SET) ? 1U : 0U;
    diagnostic->sda_level =
        (HAL_GPIO_ReadPin(BOARD_I2C_SDA_PORT,
                          BOARD_I2C_SDA_PIN) == GPIO_PIN_SET) ? 1U : 0U;

    for (address = 1U; address < 0x7FU; ++address)
    {
        if (HAL_I2C_IsDeviceReady(device->i2c,
                                  (uint16_t)(address << 1),
                                  1U,
                                  5U) == HAL_OK)
        {
            ++diagnostic->ack_count;
            if (address == device->address_7bit)
                diagnostic->address_ready = 1U;
            if (diagnostic->ack_1 == 0U) diagnostic->ack_1 = address;
            else if (diagnostic->ack_2 == 0U) diagnostic->ack_2 = address;
            else if (diagnostic->ack_3 == 0U) diagnostic->ack_3 = address;
            else if (diagnostic->ack_4 == 0U) diagnostic->ack_4 = address;
        }
    }

    diagnostic->model = device->model;
    diagnostic->protocol = device->protocol;
    diagnostic->profile = device->profile;
    diagnostic->profile_ambiguous = device->profile_ambiguous;
    diagnostic->confidence = device->confidence;
    diagnostic->id_raw = device->id_raw;
    diagnostic->id_code = device->id_code;
    diagnostic->revision = device->revision;
    diagnostic->auxiliary_id = device->auxiliary_id;
    diagnostic->signature_92 = device->signature_92;
    diagnostic->signature_5a = device->signature_5a;
    diagnostic->signature_cfg0 = device->signature_cfg0;
    diagnostic->signature_cfg20 = device->signature_cfg20;

    if ((device->protocol != AS734X_PROTOCOL_AS7341_SMUX) &&
        (device->protocol != AS734X_PROTOCOL_AS7343_AUTO_SMUX))
    {
        diagnostic->hal_i2c_error = HAL_I2C_GetError(device->i2c);
        return AS734X_ERROR_ID;
    }

    (void)RestoreBank0(device);
    (void)RawReadByte(device, REG_ENABLE, &diagnostic->enable);
    (void)RawReadByte(device, REG_ATIME, &diagnostic->atime);

    if (device->protocol == AS734X_PROTOCOL_AS7341_SMUX)
    {
        astep_reg = A1_REG_ASTEP_L;
        cfg1_reg = A1_REG_CFG1;
        status2_reg = A1_REG_STATUS2;
        statusx_reg = A1_REG_STATUS6;
    }
    else
    {
        astep_reg = A3_REG_ASTEP_L;
        cfg1_reg = A3_REG_CFG1;
        status2_reg = A3_REG_STATUS2;
        statusx_reg = A3_REG_STATUS4;
    }

    (void)RawRead(device, astep_reg, astep_bytes, 2U);
    diagnostic->astep = (uint16_t)((uint16_t)astep_bytes[0] |
                                   ((uint16_t)astep_bytes[1] << 8));
    (void)RawReadByte(device, cfg1_reg, &diagnostic->cfg1);
    (void)RawReadByte(device, status2_reg, &diagnostic->status2);
    (void)RawReadByte(device, statusx_reg, &diagnostic->statusx);

    init_status = WaitInitBusy(device, 5U);
    diagnostic->init_ready = (init_status == AS734X_OK) ? 1U : 0U;
    diagnostic->protocol_ok =
        ((diagnostic->address_ready != 0U) &&
         (device->confidence == AS734X_CONFIDENCE_HIGH)) ? 1U : 0U;
    diagnostic->hal_i2c_error = HAL_I2C_GetError(device->i2c);
    return AS734X_OK;
}

uint8_t AS734X_GetChannelCount(const AS734X_Device_t *device)
{
    return (device != NULL) ? device->channel_count : 0U;
}

const char *AS734X_GetChannelName(const AS734X_Device_t *device,
                                  uint8_t channel)
{
    AS734X_Profile_t profile;

    if (device == NULL) return "UNKNOWN";
    profile = AS734X_GetEffectiveProfile(device);
    if (((profile == AS734X_PROFILE_AS7341) ||
         (profile == AS734X_PROFILE_AS7341L)) &&
        (channel < 10U))
    {
        return k_a1_channel_names[channel];
    }
    if ((profile == AS734X_PROFILE_AS7343L) && (channel < 13U))
    {
        return k_a3l_channel_names[channel];
    }
    if ((profile == AS734X_PROFILE_TCS3448) && (channel < 14U))
    {
        return k_tcs3448_channel_names[channel];
    }
    if ((profile == AS734X_PROFILE_AS7343) && (channel < 14U))
    {
        return k_a3_channel_names[channel];
    }
    return "UNKNOWN";
}

uint16_t AS734X_GetChannelWavelengthNm(const AS734X_Device_t *device,
                                       uint8_t channel)
{
    AS734X_Profile_t profile;

    if (device == NULL) return 0U;
    profile = AS734X_GetEffectiveProfile(device);
    if (((profile == AS734X_PROFILE_AS7341) ||
         (profile == AS734X_PROFILE_AS7341L)) &&
        (channel < 10U))
    {
        return k_a1_channel_nm[channel];
    }
    if ((profile == AS734X_PROFILE_AS7343L) && (channel < 13U))
    {
        return k_a3l_channel_nm[channel];
    }
    if ((profile == AS734X_PROFILE_TCS3448) && (channel < 14U))
    {
        return k_tcs3448_channel_nm[channel];
    }
    if ((profile == AS734X_PROFILE_AS7343) && (channel < 14U))
    {
        return k_a3_channel_nm[channel];
    }
    return 0U;
}

uint32_t AS734X_GetIntegrationTimeUs(const AS734X_Device_t *device)
{
    uint64_t product;

    if (device == NULL) return 0U;
    product = (uint64_t)(device->atime + 1U) *
              (uint64_t)(device->astep + 1U) * 278ULL;
    return (uint32_t)((product + 50ULL) / 100ULL);
}

uint32_t AS734X_GetGainX1000(AS734X_Gain_t gain)
{
    return ((uint8_t)gain < AS734X_GAIN_COUNT) ?
           k_gain_x1000[(uint8_t)gain] : 0U;
}

uint8_t AS734X_GetMaximumGainIndex(const AS734X_Device_t *device)
{
    if ((device != NULL) &&
        (device->protocol == AS734X_PROTOCOL_AS7341_SMUX))
    {
        return A1_MAX_GAIN_INDEX;
    }
    if ((device != NULL) &&
        (device->protocol == AS734X_PROTOCOL_AS7343_AUTO_SMUX))
    {
        return A3_MAX_GAIN_INDEX;
    }
    return 0U;
}

uint16_t AS734X_GetUsefulMaximum(const AS734X_Sample_t *sample)
{
    uint16_t maximum = 0U;
    uint8_t index;

    if (sample == NULL) return 0U;
    for (index = 0U; index < sample->channel_count; ++index)
    {
        if (sample->channels[index] > maximum)
            maximum = sample->channels[index];
    }
    return maximum;
}

AS734X_Gain_t AS734X_RecommendGain(const AS734X_Device_t *device,
                                   AS734X_Gain_t current_gain,
                                   const AS734X_Sample_t *sample)
{
    uint16_t maximum;
    uint64_t desired_gain;
    uint32_t current_x1000;
    uint8_t candidate;
    uint8_t maximum_gain;
    uint64_t best_error = UINT64_MAX;
    AS734X_Gain_t best_gain = current_gain;
    const uint32_t target_count = 30000U;

    if ((device == NULL) || (sample == NULL) ||
        ((uint8_t)current_gain >= AS734X_GAIN_COUNT))
    {
        return current_gain;
    }

    maximum_gain = AS734X_GetMaximumGainIndex(device);
    maximum = AS734X_GetUsefulMaximum(sample);

    if ((sample->flags != 0U) || (maximum > 60000U))
    {
        if ((uint8_t)current_gain >= 2U)
            return (AS734X_Gain_t)((uint8_t)current_gain - 2U);
        if ((uint8_t)current_gain > 0U)
            return (AS734X_Gain_t)((uint8_t)current_gain - 1U);
        return current_gain;
    }

    if ((maximum >= 8000U) && (maximum <= 52000U))
        return current_gain;
    if (maximum == 0U) return (AS734X_Gain_t)maximum_gain;

    current_x1000 = AS734X_GetGainX1000(current_gain);
    desired_gain = ((uint64_t)current_x1000 * target_count) / maximum;

    for (candidate = 0U; candidate <= maximum_gain; ++candidate)
    {
        uint64_t candidate_gain = k_gain_x1000[candidate];
        uint64_t error = (candidate_gain > desired_gain) ?
                         (candidate_gain - desired_gain) :
                         (desired_gain - candidate_gain);
        if (error < best_error)
        {
            best_error = error;
            best_gain = (AS734X_Gain_t)candidate;
        }
    }
    return best_gain;
}

const char *AS734X_ModelString(AS734X_Model_t model)
{
    switch (model)
    {
        case AS734X_MODEL_AS7341_FAMILY: return "AS7341_FAMILY";
        case AS734X_MODEL_AS7343_FAMILY: return "AS7343_FAMILY";
        case AS734X_MODEL_TCS3448:        return "TCS3448";
        /* 地址已有应答（通常是 0x39 或 0x59），失败的是身份寄存器校验。 */
        case AS734X_MODEL_UNKNOWN_ADDRESS:return "UNKNOWN_ID";
        default:                         return "NONE";
    }
}

const char *AS734X_ModelCandidatesString(AS734X_Model_t model)
{
    switch (model)
    {
        case AS734X_MODEL_AS7341_FAMILY:
            return "AS7341_OR_AS7341L";
        case AS734X_MODEL_AS7343_FAMILY:
            return "AS7343_OR_AS7343L";
        case AS734X_MODEL_TCS3448:
            return "TCS3448";
        case AS734X_MODEL_UNKNOWN_ADDRESS:
            return "KNOWN_ADDRESS_UNKNOWN_ID";
        default:
            return "NONE";
    }
}

const char *AS734X_ProtocolString(AS734X_Protocol_t protocol)
{
    switch (protocol)
    {
        case AS734X_PROTOCOL_AS7341_SMUX: return "AS7341_MANUAL_SMUX";
        case AS734X_PROTOCOL_AS7343_AUTO_SMUX: return "AS7343_AUTO_SMUX";
        case AS734X_PROTOCOL_RAW_I2C: return "RAW_I2C";
        default: return "NONE";
    }
}

const char *AS734X_ProfileString(AS734X_Profile_t profile)
{
    switch (profile)
    {
        case AS734X_PROFILE_AUTO:    return "AUTO";
        case AS734X_PROFILE_AS7341:  return "AS7341";
        case AS734X_PROFILE_AS7341L: return "AS7341L";
        case AS734X_PROFILE_AS7343:  return "AS7343";
        case AS734X_PROFILE_AS7343L: return "AS7343L";
        case AS734X_PROFILE_TCS3448: return "TCS3448";
        default:                     return "UNKNOWN";
    }
}

const char *AS734X_EffectiveProfileString(const AS734X_Device_t *device)
{
    return AS734X_ProfileString(AS734X_GetEffectiveProfile(device));
}

const char *AS734X_ConfidenceString(uint8_t confidence)
{
    switch (confidence)
    {
        case AS734X_CONFIDENCE_HIGH: return "HIGH";
        case AS734X_CONFIDENCE_MEDIUM: return "MEDIUM";
        case AS734X_CONFIDENCE_LOW: return "LOW";
        default: return "NONE";
    }
}

const char *AS734X_StatusString(AS734X_Status_t status)
{
    switch (status)
    {
        case AS734X_OK:                return "OK";
        case AS734X_ERROR_ARGUMENT:    return "ARGUMENT";
        case AS734X_ERROR_I2C:         return "I2C";
        case AS734X_ERROR_ID:          return "ID";
        case AS734X_ERROR_TIMEOUT:     return "TIMEOUT";
        case AS734X_ERROR_BUS_DATA:    return "BUS_DATA";
        case AS734X_ERROR_VERIFY:      return "VERIFY";
        case AS734X_ERROR_UNSUPPORTED: return "UNSUPPORTED";
        default:                       return "UNKNOWN";
    }
}
