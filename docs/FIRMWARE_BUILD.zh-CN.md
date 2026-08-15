# 固件编译与烧录

[English](FIRMWARE_BUILD.md) · [返回中文首页](../README.zh-CN.md)

## 准备软件和硬件

- STM32CubeIDE 2.2.0，或兼容的更新版本。
- ST-LINK 已接好 SWDIO、SWCLK、GND，以及烧录器要求的目标电压参考。
- 主板正常供电。

工程目标芯片是 `STM32G030C8T6`。仓库已经包含生成好的 HAL/CMSIS 源码，普通编译不需要先打开 STM32CubeMX。

## 导入 STM32CubeIDE

1. 启动 STM32CubeIDE，在仓库外选择一个工作区。
2. 选择 **File → Import → General → Existing Projects into Workspace**。
3. 根目录选择 `firmware/stm32g030`。
4. 导入 `ams_spectral_sensor_firmware`，不要勾选“Copy projects into workspace”。
5. 选择 **Debug** 配置，执行 **Project → Build Project**。

输出文件：

```text
firmware/stm32g030/Debug/ams_spectral_sensor_firmware.elf
```

使用 STM32CubeIDE 2.2.0 / GNU Tools for STM32 14.3.rel1 编译时，Debug 版本约占 54.8 KB Flash、2.5 KB RAM。

## 用 STM32CubeIDE 烧录

1. 关闭可能反复复位目标板的串口终端。
2. 插好 ST-LINK，并确认主板上电。
3. 选中工程，打开 **Run → Debug Configurations**。
4. 没有配置时，新建 **STM32 C/C++ Application**。
5. 选择刚生成的 ELF，接口使用 SWD，点击 **Debug** 或 **Run**。
6. 等待下载和校验完成，然后继续运行或复位芯片。
7. 打开上位机，以 115200 波特率连接，在设备摘要中检查固件版本。

## STM32CubeMX / `.ioc`

`ams_spectral_sensor_firmware.ioc` 可用 STM32CubeMX 或 STM32CubeIDE 编辑。板级分配如下：

| 功能 | 外设 / 引脚 |
|---|---|
| 光谱传感器 I²C | I²C1，PB6 SCL / PB7 SDA |
| 上位机串口 | USART1，115200 baud |
| 状态灯 | PA8 |
| 405 nm LED | PA4 |
| 白光 LED | PA6 |
| 850 nm LED | PA7 |
| 940 nm LED | PB0 |
| NTC | ADC1 |

应用代码位于 `Core/Src/as734x.c`、`Core/Src/spectral_app.c` 及对应头文件。外设别名、GPIO 和稳定延时位于 `Core/Inc/board_config.h`。

## 修改主板引脚

1. 在 `.ioc` 中调整引脚或外设并重新生成。
2. 同步修改 `Core/Inc/board_config.h` 中的外设句柄和 GPIO 别名。
3. 保持全部警告开启并重新编译。
4. 按 `BOARD`、`LED CYCLE 500`、`TEMP`、`I2CSCAN`、`DETECT`、`MEASURE` 的顺序测试。

LED 有效电平配置错误会使光源在暗场采集期间保持点亮。
