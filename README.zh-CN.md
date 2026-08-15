# AMS 光谱传感器应用

[English](README.md)

STM32 固件和 Python 上位机，支持 **AS7341、AS7343、TCS3448**。固件自动识别传感器，上位机按固件返回的通道表显示数据。

![中文上位机](docs/images/ui-zh.png)

## 功能

- 自动探测 `0x39` 上的 AS7341/AS7343 和 `0x59` 上的 TCS3448。
- AS7341 使用两轮手动 SMUX；AS7343、TCS3448 使用三轮自动 SMUX。
- 依次采集 405 nm、白光、850 nm、940 nm，每路包含独立暗场。
- 每路光源独立自动增益，保留亮场、暗场、净值、饱和标志、积分参数和板载温度。
- 默认中文界面，可切换英文。
- 显示光谱图、热图、稳定性、温度曲线和“光源 × 通道”数值表。
- 串口报文带 CRC，支持测量数据和稳定性数据导出为 CSV。

## 快速使用

1. 安装 Python 3.10 或更新版本，并保留 Tcl/Tk 组件。
2. 进入 `host-software`，运行 `install_dependencies.bat`。
3. 连接主板 USB，运行 `run.bat`。
4. 选择 COM 口并点击“连接”。
5. 点击“四光源完整测量”。

固件操作见[固件编译与烧录](docs/FIRMWARE_BUILD.zh-CN.md)，上位机操作见[使用说明](docs/USER_GUIDE.zh-CN.md)。

## 支持的传感器

| 传感器 | I²C 地址 | 采样方式 | 通道数 | 验证状态 |
|---|---:|---|---:|---|
| AS7341 | `0x39` | 手动 SMUX，两轮 | 10 | 已实机验证 |
| AS7343 | `0x39` | 自动 SMUX，三轮 | 14 | 未实机验证 |
| TCS3448 | `0x59` | 自动 SMUX，三轮 | 14 | 已验证 `0x59 / ID 0x81` 兼容器件 |

AS7341L、AS7343L 作为手动通道配置保留。型号判定依据和通道顺序见[传感器识别与通道](docs/SENSOR_SUPPORT.zh-CN.md)。

## 界面数据

- 暗场扣除后的光谱响应
- 每路光源峰值归一化谱形
- 光源–通道热图
- 净信号占亮场比例
- 重复测量稳定性
- NTC 温度曲线
- 环境光及四路 LED 的逐通道数值

![英文上位机](docs/images/ui-en.png)

## 目录

```text
firmware/stm32g030/       STM32CubeIDE 工程
host-software/            Python 上位机和启动脚本
docs/                     使用说明、协议和截图
CHANGELOG.md              修改记录
VERSION.txt               版本信息
```

## 文档

- [上位机使用说明](docs/USER_GUIDE.zh-CN.md) · [English](docs/USER_GUIDE.md)
- [固件编译与烧录](docs/FIRMWARE_BUILD.zh-CN.md) · [English](docs/FIRMWARE_BUILD.md)
- [传感器识别与通道](docs/SENSOR_SUPPORT.zh-CN.md) · [English](docs/SENSOR_SUPPORT.md)
- [串口协议 2.1](docs/SERIAL_PROTOCOL.zh-CN.md) · [English](docs/SERIAL_PROTOCOL.md)
- [故障排查](docs/TROUBLESHOOTING.zh-CN.md) · [English](docs/TROUBLESHOOTING.md)

## 版本

- 固件：`2.3.0-ams-spectral-application`
- 串口协议：`2.1`
- 上位机：`3.0.0`

