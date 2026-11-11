# WT9932P4C61-TINY · FreeRTOS + LVGL 学习工程

基于 **WT9932P4C61-TINY** 开发板（ESP32-P4 主机 + 模组内 ESP32-C61 从机）的精简学习工程。
从原厂完整 Brookesia 手机 demo 裁剪而来，只保留基础驱动 + 一个干净的 LVGL 交互界面，
用来学习 **FreeRTOS + LVGL**。

> 详细开发约定和踩坑记录见 [AGENTS.md](AGENTS.md)。

## 保留的功能

- MIPI-DSI 显示（JD9165BA 1024×600，RGB565，60Hz）
- GT911 触摸（15Pin DSI 转接屏引脚，I2C 地址 0x14）
- LVGL 9（BSP 内置 lvgl_port 任务）
- Wi-Fi（ESP-Hosted，C61 走 SDIO）
- 板载 WS2812 RGB LED

已裁剪：相机、视频、音频、SD 卡、以太网、Brookesia 桌面及各类 app。

## 目录结构

| 路径 | 作用 |
|---|---|
| `main/main.cpp` | **学习入口**：LVGL 界面 + FreeRTOS 后台任务 |
| `components/wt99p461_s1_board/` | 精简 BSP：I2C / 显示 / 触摸 / 背光 / WS2812 |
| `components/esp_lcd_jd9165/` | JD9165BA 面板驱动 |
| `sdkconfig.defaults` | 板级配置真值（SDIO、C61、I2C、LVGL） |
| `firmware/` | 厂家预编译镜像 |
| `docs/` | 原理图、板卡手册、官方 BSP |

## 构建与烧录

环境：ESP-IDF 6.0.1

```bash
idf.py set-target esp32p4     # 首次
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

- P4 串口为 USB-Serial-JTAG（`/dev/ttyACM0`，VID:PID `303A:1001`）
- **C61 从机不能用 P4 的 USB-JTAG 烧录**，需要 `ESP32-C6 PROG` 排针 + 3.3V USB-UART
- `sdkconfig` 是生成文件；改动请提交到 `sdkconfig.defaults`

## 已验证的显示时序

| 参数 | 值 |
|---|---|
| DPI 时钟 | 60 MHz |
| HT / VT | 1376 / 723 |
| 帧率 | 60.31 Hz |
| 像素格式 | RGB565 |

## 踩坑记录

- 板载 C61 实际跑 **SDIO** 固件（README 原写 SPI），按 SPI 配会连不上 ESP-Hosted 从机
- 触摸引脚：SCL=GPIO8 / SDA=GPIO7 / GT911=0x14，RST/INT 为 NC
- 触摸坐标 `swap_xy=0 / mirror_x=0 / mirror_y=0`，不要照搬其它底板的双轴镜像
- 烧录前确认从机 chip id：C61=`0x14`、C5=`0x17`
- LVGL 非线程安全，跨任务操作 UI 必须 `bsp_display_lock()/unlock()` 包裹
