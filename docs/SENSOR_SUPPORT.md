# Sensor identification and channel mapping

[中文](SENSOR_SUPPORT.zh-CN.md) · [Back to README](../README.md)

## Detection strategy

The driver never identifies a part from an ACK alone. It checks the expected address and read-only identity registers, restores bank selection after probing, and reports both the register-map family and candidate orderable parts.

| Result | Evidence used | Default profile |
|---|---|---|
| AS7341 family | ACK at `0x39`; register `0x92 >> 2 == 0x09` | AS7341 |
| AS7343 family | ACK at `0x39`; banked register `0x5A == 0x81` | AS7343 |
| TCS3448 | ACK at `0x59`; banked register `0x5A == 0x81` | TCS3448 |
| `UNKNOWN_ID` | ACK at a known address, identity value rejected | None |
| `NONE` / I²C error | No valid ACK at either supported address | None |

AS7341 is tested before the AS7343-style banked probe at `0x39`, avoiding an unsafe interpretation of register `0xBF` on an AS7341.

## Why the exact package can remain ambiguous

AS7341 and AS7341L share the same digital identity family. AS7343 and AS7343L likewise share address/ID evidence while their optical interpretation can differ. Firmware therefore keeps two independent ideas:

- **Detected family/candidates:** evidence from the physical device.
- **Effective profile:** mapping used to label raw ADC slots.

`AUTO` selects the normal AS7341 or AS7343 mapping. A manually selected `L` profile is useful only when package marking, reel label, or purchasing records establish that part. The UI deliberately continues to show the candidate family after a manual selection.

## Channel maps

### AS7341 — 10 reported values

| Index | Label | Nominal peak |
|---:|---|---:|
| 0 | `F1_415` | 415 nm |
| 1 | `F2_445` | 445 nm |
| 2 | `F3_480` | 480 nm |
| 3 | `F4_515` | 515 nm |
| 4 | `F5_555` | 555 nm |
| 5 | `F6_590` | 590 nm |
| 6 | `F7_630` | 630 nm |
| 7 | `F8_680` | 680 nm |
| 8 | `CLEAR` | Not a single center wavelength |
| 9 | `NIR_910` | 910 nm |

The dedicated flicker engine is not mixed into this ordinary spectral array.

### AS7343 — 14 reported values

| Index | Label | Nominal peak |
|---:|---|---:|
| 0 | `F1_405` | 405 nm |
| 1 | `F2_425` | 425 nm |
| 2 | `FZ_450` | 450 nm |
| 3 | `F3_475` | 475 nm |
| 4 | `F4_515` | 515 nm |
| 5 | `F5_550` | 550 nm |
| 6 | `FY_555` | 555 nm |
| 7 | `FXL_600` | 600 nm |
| 8 | `F6_640` | 640 nm |
| 9 | `F7_690` | 690 nm |
| 10 | `F8_745` | 745 nm |
| 11 | `NIR_855` | 855 nm |
| 12 | `CLEAR` | Not a single center wavelength |
| 13 | `FD_RAW` | Raw flicker-photodiode count, not frequency |

### TCS3448 — 14 reported values

TCS3448 uses an AS7343-compatible register protocol, but its production-characterized filter peaks are not labeled as AS7343 values.

| Index | Label | Nominal peak |
|---:|---|---:|
| 0 | `F1_407` | 407 nm |
| 1 | `F2_424` | 424 nm |
| 2 | `FZ_450` | 450 nm |
| 3 | `F3_473` | 473 nm |
| 4 | `F4_516` | 516 nm |
| 5 | `F5_546` | 546 nm |
| 6 | `FY_560` | 560 nm |
| 7 | `FXL_596` | 596 nm |
| 8 | `F6_636` | 636 nm |
| 9 | `F7_687` | 687 nm |
| 10 | `F8_748` | 748 nm |
| 11 | `NIR_855` | 855 nm |
| 12 | `CLEAR` | Not a single center wavelength |
| 13 | `FD_RAW` | Raw flicker-photodiode count, not frequency |

The desktop application does not hard-code the active table after connection. It rebuilds tables and plots from the firmware `CHANNELS` frame. Nominal wavelength plots intentionally exclude channels without one center wavelength, while the heatmap, value matrix, and CSV retain every channel.

## Interpreting an unexpected chip

- ACK at `0x39` with AS7341 ID evidence means the board wiring supports the shared `0x39` footprint, but does not prove AS7343 pin-level operation by itself.
- ACK at `0x39` with `0x5A == 0x81` supports the AS7343 digital family, not necessarily AS7343 versus AS7343L.
- ACK at `0x59` with `0x5A == 0x81` strongly supports TCS3448 in the implemented set.
- `UNKNOWN_ID` means the address is responding but the known identity pattern is absent. Save `DIAG` output before changing firmware assumptions.
- No ACK suggests power, reset, pull-up, soldering, orientation, pinout, or bus-level trouble before it suggests a new identity code.

