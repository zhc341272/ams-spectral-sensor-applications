# AMS Spectral Sensor Applications

[中文说明](README.zh-CN.md)

STM32 firmware and a Python desktop application for **AS7341, AS7343, and TCS3448**. The firmware detects the sensor, and the desktop application uses the returned channel table.

![Chinese desktop application](docs/images/ui-zh.png)

## Features

- Detects AS7341/AS7343 at `0x39` and TCS3448 at `0x59`.
- Uses two manual-SMUX passes for AS7341 and three automatic-SMUX cycles for AS7343/TCS3448.
- Measures 405 nm, white, 850 nm, and 940 nm sources with a separate dark frame for each source.
- Keeps independent automatic gain, lit/dark/net values, saturation flags, integration settings, and board temperature for each source.
- Chinese UI by default with an English option.
- Displays spectra, heatmaps, stability, temperature, and a source-by-channel value table.
- Uses CRC-protected serial frames and exports measurement or stability data to CSV.

## Quick start

1. Install Python 3.10 or newer with Tcl/Tk.
2. Open `host-software` and run `install_dependencies.bat`.
3. Connect the board over USB and run `run.bat`.
4. Select the COM port and click **Connect**.
5. Click **Measure all four LEDs**.

See [Firmware build and flash](docs/FIRMWARE_BUILD.md) and the [Desktop application guide](docs/USER_GUIDE.md) for details.

## Supported sensors

| Sensor | I²C | Acquisition | Channels | Verification |
|---|---:|---|---:|---|
| AS7341 | `0x39` | Two manual-SMUX passes | 10 | Hardware tested |
| AS7343 | `0x39` | Three automatic-SMUX cycles | 14 | Not hardware tested |
| TCS3448 | `0x59` | Three automatic-SMUX cycles | 14 | Compatible `0x59 / ID 0x81` device tested |

AS7341L and AS7343L remain available as manual channel profiles. See [Sensor identification and channels](docs/SENSOR_SUPPORT.md) for detection rules and channel order.

## Displayed data

- Dark-subtracted spectral response
- Per-source peak-normalized spectra
- Source-channel heatmap
- Net-to-lit signal ratio
- Repeated-measurement stability
- NTC temperature history
- Per-channel values for ambient and all four LEDs

![English desktop application](docs/images/ui-en.png)

## Layout

```text
firmware/stm32g030/       STM32CubeIDE project
host-software/            Python application and launch scripts
docs/                     Guides, protocol, and screenshots
CHANGELOG.md              Change history
VERSION.txt               Version information
```

## Documentation

- [Desktop application guide](docs/USER_GUIDE.md) · [中文](docs/USER_GUIDE.zh-CN.md)
- [Firmware build and flash](docs/FIRMWARE_BUILD.md) · [中文](docs/FIRMWARE_BUILD.zh-CN.md)
- [Sensor identification and channels](docs/SENSOR_SUPPORT.md) · [中文](docs/SENSOR_SUPPORT.zh-CN.md)
- [Serial protocol 2.1](docs/SERIAL_PROTOCOL.md) · [中文](docs/SERIAL_PROTOCOL.zh-CN.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md) · [中文](docs/TROUBLESHOOTING.zh-CN.md)

## Versions

- Firmware: `2.3.0-ams-spectral-application`
- Serial protocol: `2.1`
- Desktop application: `3.0.0`

