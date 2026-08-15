# Troubleshooting

[中文](TROUBLESHOOTING.zh-CN.md) · [Back to README](../README.md)

## The COM port is missing or cannot be opened

- Refresh after connecting USB and confirm the board appears in Windows Device Manager.
- Close STM32CubeIDE serial consoles, other terminals, and old instances of the desktop application.
- Confirm 115200 baud and reconnect the board.
- A Python `PermissionError` usually means another process owns the port.

## `UNKNOWN_ID` or the old “unknown address” symptom

Current firmware reports `UNKNOWN_ID` when a known address responds but the identity register is rejected. Run, in order:

```text
I2CSCAN
DIAG
AS CONFIG
```

Save the communication log. Important fields are the responding address, `ID_RAW`, `ID_CODE`, `SIG92`, `SIG5A`, `SIGCFG0`, `SIGD6`, SCL/SDA levels, and HAL I²C error.

- `0x39` ACK plus AS7341 identity: likely AS7341-family silicon.
- `0x39` ACK plus `0x5A=0x81`: AS7343 digital family.
- `0x59` ACK plus `0x5A=0x81`: strongly consistent with TCS3448.
- ACK with a different stable ID: do not add an identity guess until the marking and data sheet are confirmed.
- Unstable values: inspect solder joints, supply decoupling, pull-ups, and I²C signal integrity.

## No I²C address is found

Check, in this order:

1. sensor supply and ground at the package;
2. package orientation and pin-1 marking;
3. SCL/SDA continuity and shorts;
4. pull-up resistors and idle-high levels;
5. reset/enable pins required by the fitted part;
6. package pinout compatibility;
7. solder bridges, lifted pads, or heat damage.

An AS7341 working on the board proves the shared `0x39` bus path and some footprint connections, but does not by itself prove that every AS7343-specific pin requirement is satisfied.

## Detection works but acquisition fails

- Run `AS FORCEINIT`, then `AS SAMPLE FORCE`.
- Check `INIT_BUSY_WARN`, `STATUSX`, `I2CERR`, and saturation flags in `INFO`/`DIAG`.
- Power-cycle after raw-register experiments.
- Return the profile to `AUTO` unless the exact optical variant is independently known.
- Confirm the host received a new `CHANNELS` frame after changing profile.

## Values are zero, negative after subtraction, or inconsistent

The UI clamps negative `light − dark` results to zero. Check the raw lit and dark columns in the all-channel table or CSV.

- Ensure only the intended LED is physically on during its lit frame.
- Confirm LED active polarity in `board_config.h`.
- Increase optical shielding if ambient light changes between the dark and lit frames.
- Confirm the 50 ms settling intervals were not reduced.
- Use fixed gain temporarily to separate automatic-gain behavior from optical behavior.

## Saturation flags appear

Reduce fixed gain, ATIME/ASTEP, LED current, or optical coupling. Auto gain makes at most four probes and cannot recover data if even the minimum practical gain is saturated. Saturated samples should not be compared quantitatively.

## A full measurement feels slow

Every LED requires gain probing, a dark integration, a lit integration, and two 50 ms settling delays. This is intentional. For faster work:

- disable auto gain after determining safe per-experiment settings;
- reduce integration time while keeping enough counts;
- avoid reducing settle delays until a repeatability test shows it is safe for the actual LED driver and optical chamber.

## The English/Chinese UI does not update

Select a different language once; selecting the already active entry does nothing. If chart text shows boxes, install a CJK-capable font such as Microsoft YaHei or Noto Sans CJK SC and restart. English text can use the Matplotlib fallback font.

## Firmware does not build

- Import `firmware/stm32g030`, not the repository root.
- Confirm the project appears as `ams_spectral_sensor_firmware`.
- Select STM32CubeIDE’s bundled GNU Tools for STM32 toolchain.
- Clean the project if generated dependency files refer to an old absolute path.
- Make sure the linker script `STM32G030C8TX_FLASH.ld` remains in the project root.

