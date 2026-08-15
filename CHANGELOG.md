# 修改记录 / Changelog

## 固件 2.3.0 / 上位机 3.0.0 - 2026-08-16

- 项目更名为“AMS 光谱传感器应用 / AMS Spectral Sensor Applications”。
- 支持 AS7341、AS7343、TCS3448 自动识别和动态通道表。
- 保留 AS7341L、AS7343L 手动通道配置。
- 增加中文、英文界面切换；连接和测量数据在切换时保留。
- 固件版本更新为 `2.3.0-ams-spectral-application`，上位机版本更新为 `3.0.0`。

### English

- Renamed the project to “AMS Spectral Sensor Applications.”
- Added automatic detection and dynamic channel tables for AS7341, AS7343, and TCS3448.
- Kept AS7341L and AS7343L as manual channel profiles.
- Added Chinese/English UI switching without resetting the connection or measurements.
- Updated firmware to `2.3.0-ams-spectral-application` and the desktop application to `3.0.0`.

## 上位机 2.4.1 - 2026-08-15

- 增加“数据源 × 传感器通道”数值表。
- 显示环境光以及 405、白光、850、940 nm 的逐通道数据。
- 未采集数据用 `—` 表示。

## 上位机 2.4.0 - 2026-08-15

- 恢复系统默认 Tk/ttk 外观。
- 主页面改为左侧控制、右侧图表布局。
- 设备详情移至“配置与测试”页。

## 固件 2.2.2 - 2026-08-15

- 复核 AS7341、AS7343、TCS3448 的通道数、SMUX 顺序和峰值波长。
- AS7341 NIR 标签改为 `NIR_910`。
- AS7343、TCS3448 闪烁光电二极管标签改为 `FD_RAW`。
- `CLEAR`、`FD_RAW` 的中心波长设为 0。

## 上位机 2.3.1 - 2026-08-15

- 默认 AS7343 通道表使用 `FD_RAW`。
- 光谱折线使用实际峰值波长，排除 `CLEAR` 和 `FD_RAW`。
- 热图和数据表保留全部通道。

## 上位机 2.3.0 - 2026-08-15

- 增加净响应、归一化谱形、热图和净信号比例图。
- 增加最近 200 轮测量的稳定性曲线。
- 增加当前数据和稳定性 CSV 导出。
- 图表和导出按传感器通道表切换。

## 固件 2.2.1 - 2026-08-15

- 亮灯稳定延时从 20 ms 改为 50 ms。
- 暗场稳定延时从 10 ms 改为 50 ms。

## 固件 2.2.0 - 2026-08-15

### 固件

- 增加 AS7341、AS7343、TCS3448 自动探测。
- AS7341 使用 10 通道手动 SMUX 采样。
- AS7343、TCS3448 使用 14 通道自动 SMUX 采样。
- 增加 TCS3448 通道波长表。
- 将已知地址上的 ID 错误改报 `UNKNOWN_ID`。
- 增加 `project_version.h`。

### 上位机

- 按 `FAMILY` 切换协议、增益和配置选项。
- 按 `CHANNELS` 重建表格、曲线和 CSV 字段。
- CSV 增加传感器身份、配置和 I²C 地址。

### 验证

- STM32CubeIDE 2.2.0 编译通过。
- ST-LINK 烧录和校验通过。
- AS7341：`0x39`、`ID=0x24`、10 通道采样通过。
- TCS3448 兼容器件：`0x59`、`ID=0x81`、14 通道采样通过。
- AS7343 未实机验证。

## 固件 2.1.1 - 2026-08-15

- 增加身份寄存器和 I²C 诊断输出。
- 修正未知地址与未知 ID 的提示。

## 固件 2.1.0 - 2026-08-05

- 增加 AS734x 自动识别和多配置驱动。
