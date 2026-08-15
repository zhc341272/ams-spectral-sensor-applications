# Sensor identification and channels

[中文](SENSOR_SUPPORT.zh-CN.md) · [Back to README](../README.md)

## Detection

The firmware checks the I²C address and identity registers, then reports the register family, candidate parts, and channel profile.

| Result | Evidence used | Default profile |
|---|---|---|
| AS7341 family | ACK at `0x39`; register `0x92 >> 2 == 0x09` | AS7341 |
| AS7343 family | ACK at `0x39`; banked register `0x5A == 0x81` | AS7343 |
| TCS3448 | ACK at `0x59`; banked register `0x5A == 0x81` | TCS3448 |
| `UNKNOWN_ID` | ACK at a known address, identity value rejected | None |
| `NONE` / I²C error | No valid ACK at either supported address | None |

At `0x39`, the AS7341 ID is checked before the AS7343 banked registers because register `0xBF` has different meanings in the two families.

## Part identity and channel profile

AS7341/AS7341L and AS7343/AS7343L may share addresses and digital IDs while using different channel maps. The firmware stores these separately:

- **Candidates:** result from the address and identity registers.
- **Channel profile:** mapping from ADC slots to channel names.

`AUTO` uses the standard AS7341 or AS7343 map. Select an `L` profile manually when the package marking, reel label, or purchasing record identifies that variant.

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

Flicker-engine data is not part of this array.

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

TCS3448 uses the AS7343 register protocol with different channel peaks.

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

The desktop application rebuilds tables and plots from the firmware `CHANNELS` frame. Wavelength plots exclude `CLEAR` and `FD_RAW`; the heatmap, value table, and CSV retain all channels.
