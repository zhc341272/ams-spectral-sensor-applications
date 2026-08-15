# 修改记录 / Changelog

## 固件 2.3.0 / 上位机 3.0.0 - 2026-08-16

- 项目名称统一为“AMS 光谱传感器应用 / AMS Spectral Sensor Applications”，不再用 AS734X 代表整块主板。
- 正式支持范围整理为 AS7341、AS7343、TCS3448 三种传感器；AS7341L、AS7343L 保留为手动解析配置，不作为已验证器件宣传。
- 上位机增加界面语言选择，默认中文，可在不中断串口和不清除采样数据的情况下切换英文。
- 所有页签、操作按钮、设备摘要、提示框、状态文字和图表标题均支持中英文显示。
- 上位机版本升级为 3.0.0，固件版本升级为 2.3.0-ams-spectral-application，串口协议保持 2.1。
- 补充固件驱动层、测量时序、自动增益、串口帧和上位机数据处理的中文维护注释。
- 新建干净的发布目录，仅包含 STM32 固件、Python 上位机、双语说明、教程和界面截图。

### English summary

- Renamed the project to “AMS Spectral Sensor Applications” so the board is not tied to one AS734x family name.
- Defined AS7341, AS7343, and TCS3448 as the three officially supported sensors; AS7341L/AS7343L remain optional manual interpretation profiles.
- Added a Chinese-default desktop UI with live English switching that preserves the serial connection and measurements.
- Localized every tab, control, device summary, dialog, runtime status, and chart title.
- Bumped the desktop application to 3.0.0 and firmware to 2.3.0-ams-spectral-application; serial protocol remains 2.1.
- Added maintainable Chinese comments around the driver, measurement timing, automatic gain, serial framing, and host-side data flow.
- Added a clean publishing tree containing only firmware, host software, bilingual documentation, tutorials, and live screenshots.

## 上位机 2.4.1-chart-value-matrix - 2026-08-15

- 在主测量页增加常驻的“数据源 × 传感器波段”实际值矩阵，不再要求切换页签查看数值。
- 矩阵逐列显示当前传感器返回的全部通道，逐行显示环境光以及 405、白光、850、940 nm 四个 LED。
- LED 行显示与光谱折线完全一致的暗场扣除净值；尚未完成采集的 LED 使用 `—`，避免与真实零值混淆。
- 保留“全部通道数据”页签，用于按波段查看环境光、暗场和四路 LED 净值。

## 上位机 2.4.0-native-chart-layout - 2026-08-15

- 移除 `clam` 主题、定制颜色、字体和控件样式，恢复操作系统默认 Tk/ttk 外观。
- 主测量页改为可调宽度的左右布局：左侧仅保留采集操作和设备摘要，右侧图表占据主体空间。
- 当前批次光谱成为默认首屏；时间稳定性和全部通道数据作为同级分析页签。
- 完整设备身份、寄存器签名等低频信息移至“配置与测试”页，避免挤占图表区域。
- 导出操作和当前测量状态放到图表上方的紧凑工具栏。

## 2.2.2-channel-metadata-audit - 2026-08-15

- 对照 AS7341、AS7343、TCS3448 官方数据手册复核通道数量、自动/手动 SMUX 顺序和典型峰值波长。
- AS7341 的 NIR 元数据由无波长标记修正为典型峰值 910 nm，通道标签改为 `NIR_910`。
- AS7343、TCS3448 的 FD 光电二极管数据标签由 `FLICKER` 改为 `FD_RAW`；该值是原始计数，不是已经计算出的闪烁频率。
- `CLEAR` 与 `FD_RAW` 不赋单一中心波长；中心波长数组继续使用 0 表示“不适用”。
- 采样通道顺序和 ADC/SMUX 映射未发现错误，本次不改变测量数据排列。
- 上位机默认通道元数据同步为 `FD_RAW`；运行时仍以固件返回的 `CHANNELS` 为准。

## 上位机 2.3.1-channel-metadata-audit - 2026-08-15

- 默认 AS7343 通道表的最后一路同步改为 `FD_RAW`，避免在尚未收到固件元数据时把原始计数误认为闪烁频率。
- 运行时通道数、名称和顺序继续完全采用固件返回的 `CHANNELS` 报文。
- 两张光谱折线改用典型峰值波长作为真实数值横坐标，并排除无单一中心波长的 `CLEAR`、`FD_RAW`。
- 全通道热图和数据表仍保留 `CLEAR`、`FD_RAW`；谱形归一化、信号占比和稳定性积分仅采用有峰值波长的滤光通道。

## 上位机 2.3.0-scientific-dashboard - 2026-08-15

- 重新设计为低饱和度科研仪器风格，统一字体、页签、表格和功能区层级。
- 当前批次增加四视图：暗场扣除净响应、峰值归一化谱形、光源–通道热图、有效净信号占比。
- 增加时间稳定性页，保留最近 200 组完整测量，按光源绘制增益归一化积分趋势。
- 保留亮场、暗场、净信号、增益、积分时间、温度和饱和标志。
- 增加当前批次 CSV 与稳定性 CSV 两种导出。
- 图表和导出继续随 AS7341、AS7343、TCS3448 通道表自动切换。

## 2.2.1-stable-light-timing - 2026-08-15

- 亮灯后稳定等待由 20 ms 增加到 50 ms。
- 关灯后暗场稳定等待由 10 ms 增加到 50 ms。
- 降低 LED 电源建立、光学腔余光和暗场残留对测量的影响。
- 串口协议保持 2.1，上位机无需升级。

## 2.2.0-three-sensor-auto - 2026-08-15

### 固件

- 新增 AS7341、AS7343、TCS3448 三类器件自动探测。
- AS7341：检查 `0x39` 及 `0x92` 身份码，加载 10 通道手动 SMUX 采样。
- AS7343：检查 `0x39` 及 Bank 1 `0x5A=0x81`，加载 14 通道自动 SMUX 采样。
- TCS3448：检查 `0x59` 及 Bank 1 `0x5A=0x81`，加载独立型号和光谱元数据。
- TCS3448 通道标签使用官方典型中心波长：407、424、450、473、516、546、560、596、636、687、748、855 nm。
- 修正 `UNKNOWN_ADDRESS` 误报，有 ACK 但 ID 不匹配时改报 `UNKNOWN_ID`。
- 保留探测失败时的 `SIG92/SIG5A/SIGCFG0/SIGD6` 原始值。
- 修正总线无设备时的型号提示。
- 修正 TCS3448 `0x59` 诊断中 `ADDR_READY/PROTOCOL_OK` 错误为 0。
- 新增统一版本头 `project_version.h`。

### Python 上位机

- 默认启动程序改为多型号版 `as734x_pc_tool.py`。
- 按 `FAMILY` 自动切换型号、协议、增益范围和配置选项。
- 按 `CHANNELS` 动态重建通道表、曲线横坐标和 CSV 字段。
- TCS3448 与 AS7343 使用各自的中心波长标签。
- CSV 导出写入传感器家族、候选型号、协议、配置和 I2C 地址。

### 验证

- STM32CubeIDE 2.2.0 自带工具链编译通过。
- ST-Link 烧录和 Flash 校验通过。
- AS7341 实机：`0x39`、`ID=0x24`、初始化和 10 通道采样通过。
- TCS3448 兼容器件实机：`0x59`、`ID=0x81`、初始化和 14 通道采样通过。
- AS7343 未获得可通信样品，尚未做实机验证。

## 2.1.1-id-diagnostics - 2026-08-15

- 增强身份寄存器和 I2C 诊断输出。
- 修正未知地址/未知 ID 提示语义。

## 2.1.0-as734x-auto - 2026-08-05

- 初步增加 AS734x 家族自动识别和多配置驱动。
