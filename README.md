# PSOC-EDGI-TALK 0.3.0

项目创作者 [zhanghf170-dotcom](https://github.com/zhanghf170-dotcom)

基于 Infineon PSoC Edge E84 Edgi-Talk 的双核 RT-Thread 工程，提供 LVGL 表盘、图片动图图库、日历与计时工具、TF 卡黑胶音乐播放器、USB 相机预览、疲劳与钢球监测、局域网图传（HTTP MJPEG / RTSP / TCP Relay）和小智语音界面。

## 0.3.0 主要更新

- **AI 检测防卡死重构与实时 `ai_fps`**：将 UVC AI 生命周期、模型切换、推理和钢球多帧跟踪（`sb_tracker`）收敛到 `uvc_aiwk` 单一工作线程；增加 2500 ms Ethos-U55 NPU 看门狗与中断清理防卡死机制，并在界面角标、指标栏、画面 OSD 及 `ai_status` 实时显示 2000 ms 滑动窗口真实推理帧率 `ai_fps`。
- **TF 卡动态资源与热插拔**：壁纸、图库（含 GIF 动图）、告警/提示音 WAV、小智语音素材与钢球模型（`/sdcard/models/steel_ball_model.bin`）迁移至 TF 卡按需加载，支持热插拔自动重挂载（`sd_status` / `sd_reinit`），`m55_hyperram` 可缓存堆扩展至 4 MiB。
- **日历工具与黑胶音乐播放器**：内置 1902–2099 年农历与节日日历（支持 `/sdcard/calendar.txt` 纪念日）、秒表与倒计时；新增 TF 卡 MP3（`minimp3`）/ WAV 音乐播放器，支持 UTF-8 中文文件名、ID3v2/同目录封面、`.lrc` 歌词滚动及告警/TTS 音频抢占。
- **图传、中文显示与 UI 优化**：修复双路 HTTP MJPEG 单帧 `send()` 并发稳定性；支持完整中文 Wi-Fi SSID 显示；内置 64 字节对齐 VG-Lite 预乘 Gemini 简化图标。
- **相容性说明**：相机预览与监测共用 `usbh_uvc_stream.c` 源码级 USB 视频流；钢球结果需连续多帧跟踪确认才显示，默认置信度为 55%（支持 `0.15..0.95` 调节），钢球页隐藏疲劳报警控件；RTSP 使用端口 8554 和 TCP 传输，双击 [start_rtsp_viewer.bat](start_rtsp_viewer.bat) 并输入板上「无线网络与图传」显示的 IPv4 地址即可播放（电脑需安装 FFplay，板端需打开相机预览或监测）。

## 构建

在 RT-Thread Studio 安装 **PSOC_E84-EDGI-TALK 1.4.0** BSP 和 **Arm GNU 13.3** 工具链。从仓库根目录运行以下命令：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\prepare_sdk.ps1
powershell -ExecutionPolicy Bypass -File .\build_dualcore.ps1
```

可用 `-SdkRoot` 指定 BSP 路径，通过 `RTT_EXEC_PATH` 指定工具链的 `bin` 目录。输出为 `Debug/rtthread.hex` 和 `Debug/cm33_combined.hex`。

- **视觉模型**：标准构建提供相机预览与图传占位桩（`uvc_ai_unavailable.c`）。使用 `prepare_sdk.ps1 -ModelRoot <模型目录>` 可导入本地眼状态模型 `model.c` 和 `model.h`（及可选 `steel_ball_model.h`）；钢球检测模型由 TF 卡 `/sdcard/models/steel_ball_model.bin` 运行时动态加载。
- **可选资源与本地 Wi-Fi 默认值**：公开示例素材位于 `resources/`（可复制到 TF 卡对应 `/sdcard/res/`、`/sdcard/gallery/`、`/sdcard/audio/` 目录）。若需将中文字库 `xiaozhi_zh_l1.ttf` 编入 Flash，可将其放入 `resources/` 或通过 `EDGI_MEDIA_DIR` 指定目录。可在工程根目录创建不被 Git 跟踪的 `rtconfig_local.h` 定义 `WIFI_DEFAULT_SSID` 与 `WIFI_DEFAULT_PWD`。可选的 [电脑语音桥](start_gemini_voice.bat) 使用 Python、`edge-tts` 和 FFmpeg。各依赖的版权与许可证见对应源码和许可证文件。

[功能、烧录和串口命令](README_zh.md) · [双核架构](DUALCORE_ARCHITECTURE.md)
