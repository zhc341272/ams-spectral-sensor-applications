# AMS 光谱传感器应用

[English](README.md)

这是面向通用 AMS 光谱传感器主板的 STM32 固件和 Python 上位机。当前正式支持 **AS7341、AS7343、TCS3448**，固件会自动选择对应的寄存器协议，上位机再按实际器件动态切换通道数、波长标签和图表。

![中文上位机](docs/images/ui-zh.png)

## 主要功能

- 自动识别 `0x39` 地址的 AS7341/AS7343，以及 `0x59` 地址的 TCS3448。
- AS7341 使用手动 SMUX 两轮采样；AS7343、TCS3448 使用自动 SMUX 三轮采样。
- 依次测量 405 nm、白光、850 nm、940 nm 四路光源，每一路都单独采集同增益、同积分时间的暗场。
- 光源点亮和熄灭后均等待 50 ms，再开始传感器积分，给 LED 电源和光学腔留出稳定时间。
- 每路光源独立自动增益，同时保留亮场、暗场、净值、饱和标志、积分参数和板载温度。
- 上位机默认中文，可切换英文；切换语言不会断开串口，也不会清除已采集数据。
- 通道表和全部图表以固件返回的 `CHANNELS` 报文为准，三种传感器分别使用正确的通道数和波长标签。
- 串口报文带 CRC；支持当前批次 CSV 和重复测量稳定性 CSV 导出。

## 五分钟开始使用

1. 安装 Python 3.10 或更新版本，安装时保留 **Tcl/Tk** 组件。
2. 进入 `host-software`，首次使用双击 `install_dependencies.bat`。
3. 用 USB 连接主板，双击 `run.bat`。
4. 选择对应 COM 口，点击“连接”，确认设备摘要中的型号和 I²C 地址。
5. 点击“四光源完整测量”。每一路完成后，图表和下方“光源 × 通道”实际值矩阵会同步更新。

固件导入、编译和烧录见[固件教程](docs/FIRMWARE_BUILD.zh-CN.md)，所有上位机功能见[上位机使用教程](docs/USER_GUIDE.zh-CN.md)。

## 支持范围

| 传感器 | I²C 地址 | 采样方式 | 上报值数量 | 验证状态 |
|---|---:|---|---:|---|
| AS7341 | `0x39` | 手动 SMUX 两轮 | 10 | 已实机验证 |
| AS7343 | `0x39` | 自动 SMUX 三轮 | 14 | 驱动路径已实现，等待确认料号的样品验证 |
| TCS3448 | `0x59` | 自动 SMUX 三轮 | 14 | 已实机验证 |

固件中还保留 AS7341L、AS7343L 的手动数据解析配置，但它们不计入本版三种正式支持器件。部分 `L`/非 `L` 型号共用地址和数字 ID，仅凭 I²C 通信不能证明完整订货料号，具体见[传感器识别与通道说明](docs/SENSOR_SUPPORT.zh-CN.md)。

## 图表和实际值

- 按典型峰值波长绘制的暗场扣除净响应。
- 每路光源独立峰值归一化后的谱形。
- 包含 `CLEAR`、`FD_RAW`（若器件提供）的光源–通道归一化热图。
- 有效净信号占亮场读数比例。
- 折算到 1× 增益的重复测量稳定性。
- NTC 温度变化曲线。
- 环境光与四路 LED 在每一个实际通道上的数值矩阵。

![英文上位机](docs/images/ui-en.png)

## 目录结构

```text
firmware/stm32g030/       STM32G030C8T6 的 STM32CubeIDE 工程
host-software/            Python/Tk 上位机、安装和启动脚本
docs/                     中英文教程、协议说明和界面截图
CHANGELOG.md              修改记录
VERSION.txt               当前固件、协议和上位机版本
```

仓库不会上传 PCB 设计、历史压缩包、日志、编译产物和已淘汰的上位机脚本。

## 文档

- [上位机使用教程](docs/USER_GUIDE.zh-CN.md) · [English](docs/USER_GUIDE.md)
- [固件编译与烧录](docs/FIRMWARE_BUILD.zh-CN.md) · [English](docs/FIRMWARE_BUILD.md)
- [传感器识别与通道](docs/SENSOR_SUPPORT.zh-CN.md) · [English](docs/SENSOR_SUPPORT.md)
- [串口协议 2.1](docs/SERIAL_PROTOCOL.zh-CN.md) · [English](docs/SERIAL_PROTOCOL.md)
- [故障排查](docs/TROUBLESHOOTING.zh-CN.md) · [English](docs/TROUBLESHOOTING.md)

## 当前版本

- 固件：`2.3.0-ams-spectral-application`
- 串口协议：`2.1`
- 上位机：`3.0.0`

固件已使用 STM32CubeIDE 2.2.0 / GNU Tools for STM32 14.3.rel1 重新编译。AS7341 与 TCS3448 的识别和采集路径已在当前主板上做过实机验证。

