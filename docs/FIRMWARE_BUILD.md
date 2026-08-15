# Firmware build and flash guide

[中文](FIRMWARE_BUILD.zh-CN.md) · [Back to README](../README.md)

## Requirements

- STM32CubeIDE 2.2.0 or a compatible newer release.
- ST-LINK connected to SWDIO, SWCLK, GND, and the target supply/reference as required by the probe.
- The board powered normally.

The checked-in project targets `STM32G030C8T6`. It includes generated HAL/CMSIS sources, so STM32CubeMX is not required for an ordinary build.

## Import into STM32CubeIDE

1. Start STM32CubeIDE and choose a workspace outside this repository.
2. Select **File → Import → General → Existing Projects into Workspace**.
3. Choose `firmware/stm32g030` as the root directory.
4. Import `ams_spectral_sensor_firmware`. Do not select “Copy projects into workspace” if you want edits to remain in the repository.
5. Select the **Debug** configuration and run **Project → Build Project**.

The expected artifact is:

```text
firmware/stm32g030/Debug/ams_spectral_sensor_firmware.elf
```

The current source was rebuilt with GNU Tools for STM32 14.3.rel1. A typical Debug build uses about 54.8 KB Flash and 2.5 KB RAM; small differences between toolchain releases are normal.

## Flash with STM32CubeIDE

1. Disconnect any serial terminal that might reset or hold the target unexpectedly.
2. Connect ST-LINK and power the board.
3. Select the project, then **Run → Debug Configurations**.
4. Create an **STM32 C/C++ Application** configuration if one does not exist.
5. Select the generated ELF, use the SWD interface, and click **Debug** or **Run**.
6. Let the programmer complete download and verification, then resume or reset the target.
7. Open the desktop application at 115200 baud and check the firmware version in the device summary.

## Use the `.ioc` file

`ams_spectral_sensor_firmware.ioc` can be opened in STM32CubeMX or directly in STM32CubeIDE. The current board assignments are:

| Function | Peripheral / pin |
|---|---|
| Spectral sensor I²C | I²C1, PB6 SCL / PB7 SDA |
| Console serial | USART1, 115200 baud |
| Status LED | PA8 |
| 405 nm LED | PA4 |
| White LED | PA6 |
| 850 nm LED | PA7 |
| 940 nm LED | PB0 |
| NTC | ADC1 |

Application code lives in `Core/Src/as734x.c`, `Core/Src/spectral_app.c`, and their headers. Board aliases and settling times live in `Core/Inc/board_config.h`.

Before generating code from CubeMX, commit or back up local changes. Generated files such as `main.c` use CubeMX user-code sections, but the standalone driver and application files should still be checked in the post-generation diff.

## Change board pins safely

1. Change the pins/peripherals in the `.ioc` file and regenerate.
2. Update handle and GPIO aliases in `Core/Inc/board_config.h`.
3. Build with all warnings enabled.
4. Test `BOARD`, `LED CYCLE 500`, `TEMP`, `I2CSCAN`, `DETECT`, and finally `MEASURE`.

Keep LED active polarity correct. An incorrect active level can leave a source on during the supposed dark frame.

## Firmware versioning

Change `Core/Inc/project_version.h` for a release. Keep `PROJECT_PROTOCOL_VERSION` at `2.1` unless frame syntax or semantics change incompatibly. Record user-visible changes in `CHANGELOG.md` and `VERSION.txt`.

## Clean builds

`Debug/` and `Release/` are intentionally ignored. If generated build state is stale, use **Project → Clean**, then rebuild. Do not commit ELF, HEX, BIN, map, listing, object, dependency, stack-usage, or cyclomatic-complexity files.

