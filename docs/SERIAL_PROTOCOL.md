# Serial protocol 2.1

[中文](SERIAL_PROTOCOL.zh-CN.md) · [Back to README](../README.md)

## Transport and framing

- UART: 115200 baud, 8 data bits, no parity, 1 stop bit.
- Host commands: case-insensitive ASCII lines terminated by CR, LF, or CRLF.
- Device output: UTF-8-compatible ASCII payload wrapped as `$<payload>*<CRC16>\r\n`.
- CRC: CRC-16/CCITT-FALSE over payload bytes only; polynomial `0x1021`, initial value `0xFFFF`, no reflection, no final XOR. The hexadecimal field is four uppercase digits.

The CRC for `PONG,1234` covers those nine bytes only.

## Startup sequence

After reset the device emits:

1. `BOOT` with firmware, protocol, sensor status, family, and protocol name;
2. `INFO` with identity and acquisition settings;
3. an error and `DIAG` if initialization failed;
4. `HELP` with the command list.

## Identity and metadata

### `INFO`

Key/value frame containing versions, MCU, sensor identity, I²C address, acquisition settings, channel count, and diagnostic state.

### `SENSOR`

Detailed identity result, including `SIG92`, `SIG5A`, `SIGCFG0`, and `SIGD6`. A manual profile never replaces `FAMILY` or `CANDIDATES`.

### `CHANNELS`

```text
CHANNELS,<name0>,<name1>,...,<nameN-1>
```

Defines the channel order for subsequent `AMBIENT` and `MEAS` frames.

## Measurement frames

### Ambient snapshot

```text
AMBIENT,<tick_ms>,<gain_index>,<gain_x1000>,<atime>,<astep>,<tint_us>,<flags>,<ch0>...<chN-1>
```

Values are unsigned 16-bit ADC counts. `flags` is hexadecimal.

### Four-source sequence

```text
BEGIN,<sequence>
MEAS,<sequence>,<source>,<gain_index>,<gain_x1000>,<atime>,<astep>,<tint_us>,<temperature_x10>,<light_flags>,<dark_flags>,<light0>...<lightN-1>,<dark0>...<darkN-1>
MEAS,... one frame for each remaining source ...
END,<sequence>
```

`source` is `405`, `WHITE`, `850`, or `940`. Net signal is computed as `max(0, light - dark)` by the desktop application. Each source can use a different gain when automatic gain is enabled.

Flag bits:

| Bit | Meaning |
|---:|---|
| 0 | ASTATUS saturation |
| 1 | Digital saturation |
| 2 | Analog saturation |

`temperature_x10` is signed tenths of a degree Celsius. `gain_x1000` represents 0.5× as `500`, 16× as `16000`, and so on.

## Other common frames

- `TEMP,STATUS=OK,RAW=...,MV=...,R_OHM=...,T_X10=...`
- `LEDSTAT,MASK=...,405=...,WHITE=...,850=...,940=...`
- `ACK,<operation>,...` for accepted operations.
- `ERR,<reason>,...` for rejected commands or runtime failures.
- `BOARD`, `I2CSUM`, `I2CSCAN`, `DIAG`, `ASCFG`, `ASREG`, and `ASRWTEST` for diagnostics.

## Command reference

| Command | Purpose |
|---|---|
| `PING` | Link test |
| `INFO` | Current identity and acquisition configuration |
| `DETECT` | Turn LEDs off and run complete sensor detection/init |
| `MEASURE` | One four-source measurement sequence |
| `READ` | One all-LED-off ambient snapshot |
| `STREAM <ms>` | Repeated full measurements; minimum 1000 ms |
| `STOP` | Stop streaming and turn all LEDs off |
| `TEMP` | Read NTC details |
| `BOARD` | Board clock, pins, LED mask, ADC and temperature status |
| `I2CSCAN` | Scan 7-bit I²C addresses |
| `DIAG` | Full bus and sensor diagnostic |
| `LED STATUS` | Return LED mask |
| `LED <405\|WHITE\|850\|940> <ON\|OFF>` | Manual source control |
| `LED ALL OFF` | Turn off every source |
| `LED CYCLE <ms>` | Sequential LED wiring test |
| `SET PROFILE <AUTO\|AS7341\|AS7341L\|AS7343\|AS7343L\|TCS3448>` | Select channel interpretation |
| `SET AUTOGAIN <0\|1>` | Disable/enable automatic gain |
| `SET GAIN <index>` | Set fixed gain and all remembered source gains |
| `SET ATIME <0..255>` | Change ATIME |
| `SET ASTEP <1..65534>` | Change ASTEP |
| `AS CONFIG` | Read register configuration summary |
| `AS REG READ <bank> <addr>` | Read one register |
| `AS REG WRITE <bank> <addr> <value>` | Write and read back one register |
| `AS DUMP <bank> <start> <count>` | Read up to 64 registers |
| `AS RWTEST` | Change and restore timing/gain as a write test |
| `AS FORCEINIT` | Reconfigure the detected sensor |
| `AS RESET` | Sensor reset followed by configuration |

Integer arguments accept decimal and `0x` hexadecimal notation.

## Receiver compatibility

Receivers ignore unknown key/value fields and use `CHANNELS` plus `CHANNEL_COUNT` as the active channel definition.
