# AGENTS.md · 开发约定（AI 助手必读）

本文件是给 AI 编码助手（以及人类协作者）的工作约定。开始任何改动前请先读完本文件。

## ⛔ 铁律：禁止未经批准编译 / 烧录

- **编译很慢**（ESP-IDF 全量/增量构建动辄数分钟，会占满 CPU，还会卡住整个会话）。
- **未经用户明确批准，禁止执行以下任何命令**：
  - `idf.py build`
  - `idf.py flash` / `idf.py monitor`
  - `idf.py fullclean` / `idf.py reconfigure` / `idf.py set-target`
  - 任何形式的 `cmake` / `ninja` / `make` 调用
- 正确做法：改完代码后**停下来**，告诉用户改了什么、建议跑什么命令，由用户自己决定是否编译。
- 如果确实需要编译验证，必须先问一句"要我现在编译吗？"，得到明确同意才能执行。
- 用户已经中断过的编译，说明用户不需要它——不要重试，不要换种方式再编。

## 项目概况

- **硬件**：WT9932P4C61-TINY 开发板（ESP32-P4 主机 + 模组内 ESP32-C61 从机）
- **框架**：ESP-IDF 6.0.1，目标芯片 `esp32p4`
- **界面**：LVGL 9（BSP 内置 `lvgl_port` 任务）
- **已占用 GPIO**（勿挪用，详见 `components/wt99p461_s1_board/include/bsp/wt99p461_s1_board.h`）：

| GPIO | 用途 |
|---|---|
| 7 / 8 | 触摸 I2C（GT911=0x14） |
| 20 | 背光 |
| 21 / 23 | 触摸 INT / RST |
| 22 | LCD 偏置电源使能（AP3012K） |
| 26 / 27 | LCD 扫描方向 UPDN / SHLR |
| 47 / 48 |（另一硬件版本的 I2C） |
| 51 | 板载 WS2812 |

应用层（`main/main.cpp`）另占用：**GPIO28**（翻转按钮）、**GPIO9**（LEDC PWM 输出）。

## 目录结构

| 路径 | 作用 |
|---|---|
| `main/main.cpp` | **学习入口**：LVGL 界面 + FreeRTOS 后台任务 |
| `components/wt99p461_s1_board/` | 精简 BSP：I2C / 显示 / 触摸 / 背光 / WS2812 |
| `components/esp_lcd_jd9165/` | JD9165BA 面板驱动 |
| `sdkconfig.defaults` | 板级配置真值（`sdkconfig` 是生成物，改动只提交到这里） |
| `firmware/` | 厂家预编译镜像（只读参考） |
| `docs/` | 原理图、板卡手册、官方 BSP |

## 代码约定

- 入口文件是 C++（`main.cpp`），但 ESP-IDF API 都是 C 接口，注意 `extern "C"` 边界。
- **LVGL 非线程安全**：任何跨任务操作 UI / WS2812 的代码必须用
  `bsp_display_lock()` / `bsp_display_unlock()` 包裹。
- 新增源文件要确认 `main/CMakeLists.txt` 的 `REQUIRES` 已包含所需组件
  （如用 GPIO 需要 `esp_driver_gpio`，目前已包含）。
- 注释用中文，风格对齐 `main/main.cpp` 现有写法（分区注释头 + 行内解释）。

## 已踩过的坑（勿重复）

- 板载 C61 跑的是 **SDIO** 固件，不是 SPI；按 SPI 配 ESP-Hosted 会连不上从机。
- 触摸坐标配置为 `swap_xy=0 / mirror_x=0 / mirror_y=0`，不要照搬其它底板的双轴镜像。
- 烧录 P4 用 USB-Serial-JTAG（`/dev/ttyACM0`）；**C61 从机必须走 `ESP32-C6 PROG` 排针 + USB-UART**。
- 烧录前确认从机 chip id：C61=`0x14`、C5=`0x17`。
- WS2812 会被 LVGL 回调和后台任务同时写，必须持同一把显示锁，否则 RMT 驱动会概率性跑飞。
- **LEDC 全局时钟源全芯片共享**：BSP 背光已用 `LEDC_AUTO_CLK` 占用（timer 1 / channel 1），
  应用层再用 LEDC 必须同样用 `LEDC_AUTO_CLK`，指定其他 `clk_cfg`（如 `LEDC_USE_PLL_DIV_CLK`）
  会在 `ledc_timer_config` 报 "timer clock conflict" 并 `ESP_FAIL`。
