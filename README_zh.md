# Edgi_Talk_M55_WIFISafety — 双核 LVGL 智能座舱与监测系统 0.2.0

[项目首页](./README.md) · 项目创作者 [zhanghf170-dotcom](https://github.com/zhanghf170-dotcom)

## 1. 系统总览与架构优化

基于 **Infineon PSoC Edge E84 (Edgi-Talk)** 的双核异步实时座舱与疲劳驾驶监测告警系统：

| 特性 | 说明 |
| :--- | :--- |
| **异构双核** | **CM55 @ 400MHz** (Ethos-U55 NPU + VG-Lite GPU + LVGL + Wi-Fi) + **CM33 @ 200MHz** (IPC1 心跳监控 & 安全协同，支持 `M55 SOLO` 独立运行回退) |
| **RTOS** | RT-Thread 5.0.2（UART2 CM55 控制台 + UART5 CM33 控制台，双串口零冲突） |
| **车载 UI** | **LVGL 9.2.0 + VG-Lite**，480×800 表盘与上滑应用列表，2D AXI-DMAC 局部刷新。用 `ui_fps` 分别测面板扫描与 LVGL 动画刷新。 |
| **AI 检测** | YOLOv5-N INT8 人眼睁闭检测（Ethos-U55 NPU，`.cy_ml_input_data` D-Cache 清洗优化，配套模型由本地导入） |
| **摄像头** | USB HS UVC 320×240 YUYV，经 `.cy_itcm` 加速转换后复制到非缓存 `m33_m55_shared`，供监测页预览。 |
| **Wi-Fi 图传** | **CYW55500 + WHD In-Memory** + lwIP 2.1.2：支持实时 **HTTP MJPEG (`:8080`)**、**RTSP (`:8554/camera`)** 和 **TCP JPEG Relay (`:8888`)** |
| **总线隔离** | I2C1 (`P9.3/P9.2`) 专供 AHT20 温湿度；独立软件 I2C2 (`P13.1/P13.2`) 专供 ST7102 触摸屏 |
| **异步告警与日志** | 固件 Flash 内置 1.28 秒合成告警，启动后以 ROMFS `/cockpit_alarm.wav` 提供给 ES8388 播放。SD 卡用于日志和配置。 |

---

## 2. 手表式座舱界面

1. **表盘**使用自定义壁纸，显示中文时间、日期、秒环、室外天气和 AHT20 实测温湿度。天气卡片可直接打开天气页，上滑或点「应用」打开应用列表。「疲劳监测」或按键 `P8.3` 进入监测。
2. **Fatigue（疲劳监测与参数页）**：实时 320×240（VG-Lite 双线性放大至 440×330）摄像头与检测框画面、闭眼置信度进度条、持续时间过滤条（默认 1.5s / 阈值 70% 可调）、喇叭音量调节以及 `/sdcard/danger_log.txt` 历史记录读取与清空。
3. **无线网络**提供附近网络扫描和手动输入网络名称，选中后在板上输入密码。路由器给电脑和开发板分配同一 IPv4 子网内地址后，可以访问图传地址。
4. **System（双核与系统页）**：显示 CM55/CM33 状态与 IPC1 RTT 延时、一键 `Ping CM33`、屏幕背光调节（PWM18）、报警音效测试与总线拓扑概览。
5. **应用**提供监测、天气、无线网络、系统、试听警报、抓拍、亮度、图库和相机入口，下滑返回表盘。
6. **天气**默认上海，显示当前气温、天气、体感温度、相对湿度、观测时间和同步来源。数据超过 30 分钟时显示为过期。中文字库取自 [Noto Sans CJK SC](https://github.com/notofonts/noto-cjk)，固件使用界面字形子集。
7. **图库**内置三张示例图片。点击缩略图可查看大图，使用底部按钮切换或返回缩略图。素材随固件写入 Flash，可离线浏览。
8. **相机**提供 USB 摄像头实时预览。相机与监测共用 USB 视频流，进入相机时暂停 AI。返回应用或表盘时结束预览，可主动进入疲劳或钢球监测。预览期间暂停 P8.3 监测快捷键。

布局参考 [InfiniTime](https://github.com/InfiniTimeOrg/InfiniTime) 和 [Smartwatch-ESP32](https://github.com/muki01/Smartwatch-ESP32) 的表盘与应用导航。

### 屏幕与动画帧率

旧版面板时序按 33.984 MHz 像素时钟折算约 52.3 Hz。新版使用 38.986 MHz，板上计数实测约 60 Hz。`ui_fps` 让表盘顶部的一条短标记移动，在 2.5 秒内测得 LVGL 局部完整刷新约 62.4 次/秒，摄像头与 NPU 并行时复测结果相同。摄像头采集约 24 FPS，推理约 59–66 ms。小区域动画的数据适用于表盘交互，整屏转场与视频预览的帧率由各自的负载决定。

---

## 3. Wi-Fi 图传与 `/sdcard/wifi.cfg` 配置

### RTSP 图传

打开相机预览或监测，在电脑端执行以下命令。

```powershell
ffplay -rtsp_transport tcp rtsp://<开发板IP>:8554/camera
```

也可双击根目录的 `start_rtsp_viewer.bat`，按提示输入开发板 IP。服务默认启用，可在串口使用 `wifi_rtsp start`、`wifi_rtsp stop` 查看和调整状态。单个 RTSP 播放端可和原有 HTTP 抓拍同时使用。

### 钢球监测

使用首页「钢球监测」进入。安装配套模型后，连续两帧命中的目标才会显示。默认置信度 65%，可在界面调整。钢球监测只展示检测结果，退出页面时停止采集。模型导入步骤见[项目首页](README.md)。


开发板以 STA 模式接入现有 Wi-Fi。DHCP 就绪后，图传只在 STA 地址监听并接收同子网客户端。抓拍也只发往同子网目标。当前 BSP 附带的 CLM 缺少 `CN` 地区条目，中国区实机联网需要与板载模组匹配的 CLM。

### SD 卡自动联网配置（可选）
在 `/sdcard/wifi.cfg` 中保存以下键值对（也可通过 UI 配网或 MSH `wifi_save` 自动生成）：
```ini
ssid=YourWiFiSSID
pwd=YourWiFiPassword
relay_ip=192.168.1.100
relay_port=8888
```

### PC 端查看实时画面与接收抓拍
1. **浏览器实时查看**：访问 `http://<设备IP>:8080/`（自带实时 MJPEG 与 AI 遥测卡片），或直接打开 `http://<设备IP>:8080/stream`、单帧 `http://<设备IP>:8080/snapshot.jpg`、JSON `http://<设备IP>:8080/api/status`。
2. **TCP Relay 抓拍接收端**：
   ```bash
   python tools/relay_jpg.py --port 8888 --out ./tools/received_jpg
   ```
   疲劳报警或手动抓拍可将 JPEG 通过 TCP `:8888` 发送到电脑。

### 表盘时间和天气

Wi-Fi 暂不可用时，在连接 KitProg3 USB 的电脑上运行一次同步。加入 `--watch-interval 600` 可每十分钟更新一次，每次同步结束即释放串口。

```powershell
python tools/relay_jpg.py --watch-serial COM3 --weather-city shanghai
```

Wi-Fi 联网后，将 `relay_ip` 配为同一子网内电脑的地址，并运行 `python tools/relay_jpg.py --watch-info --weather-city shanghai`。板卡通过 UDP `8889` 同步电脑时间和天气。城市可选 `shanghai`、`beijing` 或 `shenzhen`，也可用 `--weather-location <纬度> <经度>` 指定其他地点。表盘默认显示 UTC+8。[Open-Meteo API](https://open-meteo.com/en/docs)

离线可通过 `watch_time <UTC Unix时间戳> [时差分钟]` 校时。软件 RTC 可在运行期间计时，固件复位后需要重新同步。

---

## 4. 目录结构（核心新增与改造）

| 路径 | 说明 |
| :--- | :--- |
| `applications/ui/` | LVGL 表盘、应用列表、监测、局域网图传和系统界面 |
| `applications/wifi_stream.c/h` | WHD Wi-Fi 管理 + 异步 JPEG 双缓冲 + HTTP `:8080` + TCP Relay `:8888` |
| `applications/mini_jpeg_enc.c/h` | 零依赖重入式 YUYV/RGB565 基线 JPEG 编码器（`.cy_itcm` AAN DCT） |
| `applications/dualcore_bridge.c/h` | CM55 侧 IPC1 协同网桥（含 `M55 SOLO` 优雅降级） |
| `cm33_companion/` | 完整可独立编译的 Cortex-M33 RT-Thread 伴生固件工程 |
| `build_dualcore.ps1` | 一键构建 CM55 + CM33 并生成 `Debug/cm33_combined.hex` |
| `DUALCORE_ARCHITECTURE.md` | 内存分区、MPU Cache 策略、IPC1 协议与双核烧录详细文档 |

---

## 5. 编译与烧录

首次克隆后，运行 [README 中的 SDK 初始化步骤](README.md#编译)，从本机 Edgi-Talk 1.4.0 BSP 补齐依赖。

### 双核一键构建（推荐）
```powershell
powershell -ExecutionPolicy Bypass -File .\build_dualcore.ps1
```
构建产物：
- `Debug\rtthread.hex`：CM55 主座舱/AI/LVGL/Wi-Fi 固件
- `Debug\cm33_combined.hex`：CM33 安全启动映像 + CM33 Companion 非安全伴生固件

### 独立编译 CM55
```cmd
del /f /q .sconsign.dblite 2>nul
scons.bat -j16
```

### 烧录说明
- **双核模式（完整协同）**：烧录 `Debug\cm33_combined.hex` 与 `Debug\rtthread.hex`，主界面顶部将显示绿色 `DUAL`。
- **仅烧录 CM55**：若保留原厂 CM33 Bootloader 仅烧录 `Debug\rtthread.hex`，系统会自动识别为 `M55 SOLO` 模式并保持所有座舱、AI、音频与 Wi-Fi 功能正常。

---

## 6. 常用 MSH 调试命令

```bash
# UVC AI 检测
usbh_uvc_start 0 320 240
usbh_uvc_stop

# Wi-Fi 图传与配置
wifi_save <ssid> <pwd> [relay_ip] [relay_port]
wifi_relay <ip> [port]
wifi_stream_start
wifi_stream_stop

# 双核与存储调试
ipc_ping
sdlist
ui_fps
watch_time
ui_page weather
ui_page gallery
ui_page gallery 4
ui_page camera
alert_test_sound
poweroff
```
