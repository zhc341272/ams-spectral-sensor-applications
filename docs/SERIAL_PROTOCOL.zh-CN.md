# 串口协议 2.1

[English](SERIAL_PROTOCOL.md) · [返回中文首页](../README.zh-CN.md)

## 串口和帧格式

- UART：115200 baud、8 数据位、无校验、1 停止位。
- 上位机命令：不区分大小写的 ASCII 文本，以 CR、LF 或 CRLF 结束。
- 固件输出：`$<payload>*<CRC16>\r\n`，有效载荷使用兼容 UTF-8 的 ASCII 字符。
- CRC：只覆盖 payload 字节；CRC-16/CCITT-FALSE，多项式 `0x1021`，初值 `0xFFFF`，不反射，无末尾异或。校验字段为四位大写十六进制。

例如 `PONG,1234` 的 CRC 只计算这九个字节，不包含 `$`、`*` 和换行。

## 上电输出顺序

复位后固件依次输出：

1. `BOOT`：固件、协议、传感器状态、协议族；
2. `INFO`：身份和采集参数；
3. 初始化失败时输出错误和 `DIAG`；
4. `HELP`：支持的命令。

## 身份与通道元数据

### `INFO`

键值报文，包含固件版本、协议版本、MCU、传感器状态、协议族、候选型号、寄存器协议、请求/实际配置、歧义标志、置信度、I²C 地址、ID、增益、积分参数、通道数和最近诊断状态。

### `SENSOR`

详细识别结果，额外包含 `SIG92`、`SIG5A`、`SIGCFG0`、`SIGD6`。手动配置不会覆盖 `FAMILY` 或 `CANDIDATES`。

### `CHANNELS`

```text
CHANNELS,<name0>,<name1>,...,<nameN-1>
```

这条报文定义后续 `AMBIENT`、`MEAS` 的精确通道顺序。通道表变化时，上位机必须重建表格和图表。

## 测量报文

### 环境光快照

```text
AMBIENT,<tick_ms>,<gain_index>,<gain_x1000>,<atime>,<astep>,<tint_us>,<flags>,<ch0>...<chN-1>
```

通道值是无符号 16 位 ADC 计数，`flags` 为十六进制。

### 四光源完整测量

```text
BEGIN,<sequence>
MEAS,<sequence>,<source>,<gain_index>,<gain_x1000>,<atime>,<astep>,<tint_us>,<temperature_x10>,<light_flags>,<dark_flags>,<light0>...<lightN-1>,<dark0>...<darkN-1>
MEAS,... 其余光源各一条 ...
END,<sequence>
```

`source` 为 `405`、`WHITE`、`850`、`940`。上位机按 `max(0, light - dark)` 计算净值。开启自动增益时，四种光源允许使用不同增益。

状态位：

| 位 | 含义 |
|---:|---|
| 0 | ASTATUS 饱和 |
| 1 | 数字饱和 |
| 2 | 模拟饱和 |

`temperature_x10` 是带符号的 0.1 °C。`gain_x1000` 用 `500` 表示 0.5×，`16000` 表示 16×，以此类推。

## 常见其他报文

- `TEMP,STATUS=OK,RAW=...,MV=...,R_OHM=...,T_X10=...`
- `LEDSTAT,MASK=...,405=...,WHITE=...,850=...,940=...`
- `ACK,<operation>,...`：操作已接受。
- `ERR,<reason>,...`：命令参数或运行失败。
- `BOARD`、`I2CSUM`、`I2CSCAN`、`DIAG`、`ASCFG`、`ASREG`、`ASRWTEST`：诊断信息。

## 命令表

| 命令 | 作用 |
|---|---|
| `PING` | 检查通信 |
| `INFO` | 读取身份和当前采集配置 |
| `DETECT` | 关灯并重新完成传感器识别/初始化 |
| `MEASURE` | 执行一轮四光源测量 |
| `READ` | 读取一次全关灯环境光快照 |
| `STREAM <ms>` | 连续完整测量，最短 1000 ms |
| `STOP` | 停止连续测量并关灯 |
| `TEMP` | 读取 NTC 详细值 |
| `BOARD` | 读取主板时钟、引脚、LED、ADC 和温度状态 |
| `I2CSCAN` | 扫描 7 位 I²C 地址 |
| `DIAG` | 完整总线和传感器诊断 |
| `LED STATUS` | 读取光源掩码 |
| `LED <405\|WHITE\|850\|940> <ON\|OFF>` | 手动控制一路光源 |
| `LED ALL OFF` | 关闭全部光源 |
| `LED CYCLE <ms>` | 顺序亮灯检查 |
| `SET PROFILE <AUTO\|AS7341\|AS7341L\|AS7343\|AS7343L\|TCS3448>` | 选择通道解释 |
| `SET AUTOGAIN <0\|1>` | 关闭/开启自动增益 |
| `SET GAIN <index>` | 设置固定增益和四路记忆增益 |
| `SET ATIME <0..255>` | 设置 ATIME |
| `SET ASTEP <1..65534>` | 设置 ASTEP |
| `AS CONFIG` | 读取寄存器配置摘要 |
| `AS REG READ <bank> <addr>` | 读取一个寄存器 |
| `AS REG WRITE <bank> <addr> <value>` | 写入并回读一个寄存器 |
| `AS DUMP <bank> <start> <count>` | 连续读取，最多 64 个寄存器 |
| `AS RWTEST` | 修改并恢复积分/增益，检查写入功能 |
| `AS FORCEINIT` | 按已识别协议重新配置传感器 |
| `AS RESET` | 复位传感器后重新配置 |

数值解析支持的地方可以使用十进制或 `0x` 前缀。

## 兼容原则

协议 2.1 的上位机应忽略不认识的键值字段，并始终根据 `CHANNELS`、`CHANNEL_COUNT` 工作，不能假定固定传感器。若修改 `AMBIENT`/`MEAS` 字段顺序、CRC 帧格式或通道语义，必须升级协议版本。

