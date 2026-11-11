# AGENTS.md — WT9932P4C61-TINY（精简学习工程）

面向想用这块板**学习 FreeRTOS + LVGL** 的开发者。工程已从原来的完整 Brookesia 手机 demo
裁剪成只保留基础驱动 + 一个干净的 LVGL 交互界面。改显示/触摸前先读第 1–3 节。

## 0. 这个工程是什么

- **ESP32-P4 主机 + 模组内 ESP32-C61 从机**（两颗 MCU）。P4 的 USB-JTAG 只能烧 P4，烧不到 C61。
- 保留：MIPI-DSI 显示（JD9165BA 1024×600）+ GT911 触摸 + LVGL + Wi-Fi（ESP-Hosted/SDIO）+ 板载 WS2812。
- 已裁剪：相机、视频、音频 codec、SD 卡、SPIFFS、以太网、Brookesia 桌面框架、各类 app。
- 学习入口：`main/main.cpp`（LVGL 界面 + 一个 FreeRTOS 后台任务）；驱动在 `components/`。

## 1. 硬件要点（只写会踩坑的）

- **实际板卡是 WT9932P4C61-TINY**（模组 WT0132P461-S1，ESP32-P4 + ESP32-C61）。
  早期误按 WT99P461_S1_1V1 命名，本工程已纠正；官方参考 BSP/手册在 `docs/TINY/`。

- **板载 C61 实际跑 SDIO 固件，不是 README 写的 SPI**。`sdkconfig.defaults` 已配 SDIO
  （CLK=18 / CMD=19 / D0..D3=14..17 / reset=13）。按 SPI 配会得到
  `Not able to connect with ESP-Hosted slave device`。
- **触摸用 15Pin DSI 转接屏的引脚**：SCL=GPIO8 / SDA=GPIO7 / GT911=0x14（`CONFIG_BSP_TOUCH_15PIN_DSI=y`），
  RST/INT 为 NC。仓库 PDF 里的 30Pin LCD + 6Pin CTP 是另一种底板，不能直接套用。
- 屏：JD9165BA 1024×600，MIPI DSI 2 lane，600 Mbps；GT911 触摸。
- P4 串口：`/dev/ttyACM0`（USB-Serial-JTAG，VID:PID `303A:1001`）。

## 2. 显示时序（已上板验证）

| 参数 | 值 |
|---|---|
| DPI 时钟 | PLL_F240M / 4 = **60 MHz** |
| HT | **1376** = 1024 + HBP 136 + HSW 24 + HFP 192 |
| VT | **723** = 600 + VBP 21 + VSW 2 + VFP 100 |
| 帧率 | **60.31 Hz** |
| 像素格式 | RGB565 |

代码位置：`components/wt99p461_s1_board/wt99p461_s1_board.c` 的 `bsp_display_new_with_handles()`。
面板驱动：`components/esp_lcd_jd9165/`。

## 3. 构建（IDF 6.0.1，工具在 WSL `/opt/esp`）

本机 IDF 6.0.1 在 **WSL Debian-13** 里。Windows 侧旧 build 目录已删。

```bash
# 在 WSL 内：
cd /mnt/e/esp32/pure/WT99P461_S1_1V1
bash /opt/esp/export-6.0.1.sh    # source esp-idf-6.0.1/export.sh 并设 IDF_TOOLS_PATH
idf.py -B build-wsl set-target esp32p4     # 首次
idf.py -B build-wsl build
```

- `sdkconfig` 是生成文件（gitignored），`sdkconfig.defaults` 才是仓库真值。
- `dependencies.lock` / `managed_components` 由组件管理器生成，已 gitignore。

## 4. 烧录

```bash
idf.py -B build-wsl -p /dev/ttyACM0 flash monitor
```

**C61 不能用 P4 的 USB-JTAG 烧**。需要 `ESP32-C6 PROG` 排针 + 3.3V USB-UART，或 P4 经 ESP-Hosted OTA。

## 5. 代码地图

| 路径 | 作用 |
|---|---|
| `main/main.cpp` | **学习入口**：LVGL 界面（计数器/按钮/LED 开关）+ FreeRTOS 后台任务 |
| `components/wt99p461_s1_board/` | 精简 BSP：I2C / MIPI-DSI 显示 / GT911 触摸 / 背光 / WS2812 |
| `components/esp_lcd_jd9165/` | JD9165BA 面板驱动（本地组件） |
| `sdkconfig.defaults` | 板级真值：SDIO、C61、I2C、LVGL、Wi-Fi |
| `firmware/` | 厂家预编译镜像 |
| `docs/` | 原理图、板卡图 |

## 6. 想继续精简 / 扩展

- 只要显示不要触摸：删掉 `bsp_display_start_with_config()` 里 `bsp_touch_new` 分支。
- 想学 LVGL 更多控件：在 `ui_create_ui()` 里加 `lv_chart` / `lv_table` / `lv_slider`。
- 想学 FreeRTOS：`board_task()` 是最小周期任务；可加队列/信号量。注意 **LVGL 非线程安全**，
  跨任务操作 UI 必须 `bsp_display_lock()/unlock()` 包裹。
- 想恢复音频/SD/以太网：原实现已移除，可按需从 git 历史取回；15Pin 下 GPIO7/8 被触摸占用，
  音频 I2S 不能再用这两脚。

## 7. 关键教训

- 烧录前确认板型/从机 chip id；C61=`0x14`，C5=`0x17`，别把别的板固件混烧。
- README 写 SPI，但这块板 C61 实际是 SDIO；以实测 transport/chip id 为准。
- 触摸坐标用 `swap_xy=0 / mirror_x=0 / mirror_y=0`；不要照搬其它底板的双轴镜像。