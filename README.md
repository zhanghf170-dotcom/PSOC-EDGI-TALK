# PSOC-EDGI-TALK

项目创作者 [zhanghf170-dotcom](https://github.com/zhanghf170-dotcom)

基于 Infineon PSoC Edge E84 Edgi-Talk 开发板的双核 RT-Thread 工程。功能包括 LVGL 表盘、图库、USB 相机预览、YOLOv5 眼部状态检测和局域网图传。相机和监测共用 USB 视频流，预览期间暂停 AI，主动选择监测后才启用检测。

## 编译

在 RT-Thread Studio 安装 **PSOC_E84-EDGI-TALK 1.4.0** BSP 和 **Arm GNU 13.3** 工具链。首次克隆后，在仓库目录运行以下命令。

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\prepare_sdk.ps1
.\scons.bat -j16
```

如 SDK 安装在其他目录，通过 `-SdkRoot` 指定。工具链路径可通过环境变量 `RTT_EXEC_PATH` 指定。双核构建使用 `build_dualcore.ps1`，生成 `Debug/rtthread.hex` 和 `Debug/cm33_combined.hex`。

仓库提供应用源码、BSP 改动和图库示例。初始化脚本从本机 SDK 补齐原版依赖，已提交的 BSP 改动优先。各依赖的版权和许可见其源码和随附许可文件。

默认安装 BSP 自带模型，监测页可显示摄像头画面。要启用闭眼检测，通过 `prepare_sdk.ps1 -ModelRoot <模型目录>` 导入项目眼部模型，目录应包含配套的 `model.c` 和 `model.h`。构建时可用环境变量 `EDGI_MEDIA_DIR` 选择本地媒体目录，目录结构与 `resources/` 一致。

[功能、烧录和串口命令](README_zh.md) · [双核架构](DUALCORE_ARCHITECTURE.md)
