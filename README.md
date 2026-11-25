# PSOC-EDGI-TALK 0.2.0

项目创作者 [zhanghf170-dotcom](https://github.com/zhanghf170-dotcom)

基于 Infineon PSoC Edge E84 Edgi-Talk 的双核 RT-Thread 工程，提供 LVGL 表盘、图片图库、USB 相机预览、疲劳与钢球监测、局域网图传和小智语音界面。

- 相机预览与监测共用 USB 视频流。固件在连续传输出错时重试，并可重新枚举直连的摄像头端口。
- 钢球结果需连续两帧匹配才显示，默认置信度为 65%。钢球页隐藏疲劳报警控件。
- RTSP 使用端口 8554 和 TCP 传输。板和电脑接入同一局域网后，双击 [start_rtsp_viewer.bat](start_rtsp_viewer.bat)，输入板上「无线网络与图传」显示的 IPv4 地址。电脑需安装 FFplay，板端需打开相机预览或监测。

## 构建

在 RT-Thread Studio 安装 **PSOC_E84-EDGI-TALK 1.4.0** BSP 和 **Arm GNU 13.3** 工具链。从仓库根目录运行以下命令。

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\prepare_sdk.ps1
powershell -ExecutionPolicy Bypass -File .\build_dualcore.ps1
```

可用 `-SdkRoot` 指定 BSP 路径，通过 `RTT_EXEC_PATH` 指定工具链的 `bin` 目录。输出为 `Debug/rtthread.hex` 和 `Debug/cm33_combined.hex`。

标准构建提供相机预览和图传。使用 `prepare_sdk.ps1 -ModelRoot <模型目录>` 导入本地的匹配模型。疲劳检测需要眼状态模型 `model.c` 和 `model.h`。钢球检测还需要同一目录下的 `steel_ball_model.c` 和 `steel_ball_model.h`，并共用眼状态适配器的推理工作区。

公开示例素材随仓库构建。可通过 `EDGI_MEDIA_DIR` 指定本地媒体目录，其结构沿用 `resources/`。语音界面可选 `gemini/` 图像与语音包、`xiaozhi_zh_l1.ttf` 和 `xiaozhi_ding.wav`。Wi-Fi 可在板上配置。可选的 [电脑语音桥](start_gemini_voice.bat) 使用 Python、`edge-tts` 和 FFmpeg。各依赖的版权与许可证见对应源码和许可证文件。

[功能、烧录和串口命令](README_zh.md) · [双核架构](DUALCORE_ARCHITECTURE.md)
