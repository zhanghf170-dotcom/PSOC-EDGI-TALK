# Edgi-Talk Dual-Core Architecture (CM33 + CM55)

## 1. Dual-Core Boot & Execution Model
PSoC Edge E84 boots **Cortex-M33 Secure** -> **Cortex-M33 Non-Secure (`cm33_companion`)** -> **`Cy_SysEnableCM55(MXCM55, CY_CM55_APP_BOOT_ADDR, 10)`** -> **Cortex-M55 App (`rt-thread.elf`)**.

In the default BSP template, CM33 calls `Cy_SysPm_CpuEnterDeepSleep` immediately after launching CM55, leaving CM33 asleep.
The `cm33_companion` project omits `SOC_Enable_CM33_DeepSleep`, allowing both cores to run RT-Thread concurrently:

- **Cortex-M55 (400 MHz + Ethos-U55 NPU + GFXSS VG-Lite 2.5D GPU)**
  - LVGL 9.2 Automotive Cockpit UI (`480x800`, partial double-buffer in `gfx_mem`)
  - USB HS UVC camera capture, `.cy_itcm` YUYV->RGB565 conversion & Ethos-U55 fatigue inference
  - Wi-Fi (`CYW55500` via WHD + lwIP), HTTP MJPEG `:8080`, TCP Relay `:8888`
  - Audio playback (`ES8388` I2S) & SD card persistent logging (`/sdcard`)
  - Console: **UART2** (`115200 8N1`)
- **Cortex-M33 Companion (200 MHz)**
  - Continuous IPC1 heartbeat, Ping/Pong RTT measurement, telemetry mirror & danger alert indicator (`LED_PIN_B` `P16[5]`)
  - Does **not** touch M55 power-rail GPIOs (`CTRL P7[2]`, `WIFI_OE`, `ES8388_CTRL`) at boot to avoid rail glitches
  - Console: **UART5** (`115200 8N1`)

---

## 2. Memory & Cache Partitioning

| Region | Address Range | Core / Usage | MPU Policy (CM55) |
| :--- | :--- | :--- | :--- |
| ITCM | `0x00000000..0x0003FFFF` (256 KB) | M55 hot YUYV / JPEG / ISR loops | Non-cached TCM (0-wait) |
| DTCM | `0x20000000..0x2003FFFF` (256 KB) | M55 `.data`, `.bss` (AI worker 64 KB stack), MSP | Non-cached TCM (0-wait) |
| `m33_code` / `m33_data` | `0x24058000..0x240FCFFF` | CM33 companion code/RAM | Isolated from M55 |
| `m33_allocatable_shared` | `0x240FE000..0x240FEFFF` (4 KB) | IPC TX frame pool (CM33) | Shared non-cacheable |
| `m55_allocatable_shared` | `0x240FF000..0x240FFFFF` (4 KB) | IPC TX frame pool (M55) | Region 1 Non-Cacheable |
| `m55_code_secondary` | `0x26000000..0x2605FFFF` (384 KB) | M55 `.cy_ml_input_data` (307,200 B) | **WB/WA D-Cacheable** |
| `m55_data_secondary` | `0x26060000..0x2636FFFF` (3.06 MB) | `.cy_socmem_data`, LVGL thread stack, `.heap` | **WB/WA D-Cacheable** |
| `m33_m55_shared` | `0x26370000..0x263AFFFF` (256 KB) | `.cy_uvc_rgb565_data` 320x240 stage (153,600 B), CM33/M55 shared | Region 2 **Non-Cacheable** |
| `gfx_mem` | `0x263B0000..0x264FFFFF` (1.31 MB) | `graphics_buffer` + VG-Lite heap + LVGL `disp_buf1/2` | Region 3 **Non-Cacheable** |
| `m55_hyperram` | `0x64200000..0x645FFFFF` (4 MB) | `hyperam` memheap (LVGL/GIF decode, JPEG/TLS/Opus buffers, stacks) | **WB/WA D-Cacheable** |
| `m33_m55_shared_hyperram` | `0x64600000..0x64FFFFFF` (10 MB) | UVC frame pools, Wi-Fi JPEG slots & ML arena | Region 0 **Non-Cacheable** |

---

## 3. IPC1 Dual-Pipe Protocol
`IPC0` channels `0..1` remain reserved for Infineon SRF secure services. Application IPC uses hardware **`IPC1`** channels `17..20`:

- **M33** writes on `"ipc1"` (`client_id = CM55_IPC_PIPE_CLIENT_ID`), reads on `"ipc0"`
- **M55** reads on `"ipc1"`, writes on `"ipc0"` (`client_id = CM33_IPC_PIPE_CLIENT_ID`)

| Opcode (`channel[0]`) | Direction | Payload |
| :--- | :--- | :--- |
| `0xA501 DC_MSG_HEARTBEAT` | M33 -> M55 (500 ms) | Heartbeat seq, uptime(s), free heap KB, LED state |
| `0xA502 DC_MSG_TELEMETRY` | M55 -> M33 (500 ms) | Risk level, closed-eye %, danger count, T/H x10, Wi-Fi status & FPS |
| `0xA503 DC_MSG_PING` | M55 -> M33 (on demand) | Ping sequence & M55 tick ms |
| `0xA504 DC_MSG_PONG` | M33 -> M55 (immediate) | Echoes payload for RTT measurement |
| `0xA505 DC_MSG_CMD` | M55 -> M33 | Danger active indicator (`LED_PIN_B` 150 ms flash) |

### Graceful Standalone Fallback (`M55 SOLO`)
If the stock CM33 bootloader (which sleeps CM33) is kept on flash, CM55 only sends gentle 3 s probes once offline (`timeout = 1500 ms`) and marks the cockpit UI pill as `M55 SOLO` without blocking LVGL, camera, AI, or Wi-Fi.

---

## 4. Building & Flashing

```powershell
# Build both CM55 and CM33 images + generate Debug\cm33_combined.hex
powershell -ExecutionPolicy Bypass -File .\build_dualcore.ps1
```

- **Dual-Core Full Flash**: Program `Debug\cm33_combined.hex` (CM33 Secure + `cm33_companion`) AND `Debug\rtthread.hex` (CM55 App).
- **Single-Core M55 Update**: Program `Debug\rtthread.hex` alone; CM55 falls back to `M55 SOLO` if the companion image is not present on CM33.
