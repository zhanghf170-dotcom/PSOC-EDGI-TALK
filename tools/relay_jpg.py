#!/usr/bin/env python3
"""
relay_jpg.py — 接收 M55 通过 WiFi TCP 发送的 JPG 文件，存入指定目录或 Z:\\

用法: python relay_jpg.py [--port 8888] [--out ./received_jpg]

协议 (每文件):
  [4B big-endian 文件名长度 N] [N bytes 文件名(含\\0)] [4B big-endian 文件大小 S] [S bytes 文件数据]
"""

import argparse
import json
import os
import socket
import struct
import threading
import time
from datetime import datetime
from urllib.parse import urlencode
from urllib.request import urlopen


WATCH_INFO_PORT = 8889
WEATHER_CITIES = {
    "shanghai": (0, 31.2304, 121.4737),
    "beijing": (1, 39.9042, 116.4074),
    "shenzhen": (2, 22.5431, 114.0579),
}
NO_WEATHER = (-1, 0, 0, 0, -1, 0)


def fetch_weather(city, latitude, longitude):
    query = urlencode({
        "latitude": latitude, "longitude": longitude,
        "current": "temperature_2m,weather_code,apparent_temperature,relative_humidity_2m",
        "timeformat": "unixtime",
    })
    with urlopen(f"https://api.open-meteo.com/v1/forecast?{query}", timeout=5) as response:
        current = json.load(response)["current"]
    return (int(current["weather_code"]), round(10 * current["temperature_2m"]),
            int(current["time"]), round(10 * current["apparent_temperature"]),
            round(current["relative_humidity_2m"]), city)


def serve_watch_info(service, location):
    weather = NO_WEATHER
    refresh_at = 0.0
    with service:
        while True:
            request, peer = service.recvfrom(32)
            if request not in (b"WATCH1", b"WATCH2"):
                continue

            now = int(time.time())
            report = weather if 0 <= now - weather[2] <= 1800 else NO_WEATHER
            fields = report[:3] if request == b"WATCH1" else report
            service.sendto(request + b" " + " ".join(str(value) for value in (now, *fields)).encode(), peer)

            if location and time.monotonic() >= refresh_at:
                try:
                    weather = fetch_weather(*location)
                    refresh_at = time.monotonic() + 600
                except (OSError, ValueError, KeyError, TypeError) as error:
                    print(f"[天气] 暂时无法更新 ({type(error).__name__})")
                    refresh_at = time.monotonic() + 60


def sync_watch_serial(port, location, interval):
    try:
        import serial
    except ImportError as error:
        raise SystemExit("USB 天气同步需要 pyserial，请运行 python -m pip install pyserial") from error

    while True:
        try:
            report = fetch_weather(*location)
            now = int(time.time())
            if not 0 <= now - report[2] <= 1800:
                raise ValueError("天气源没有返回近期实况")
            fields = " ".join(str(value) for value in (now, *report))
            with serial.Serial(port, 115200, timeout=0.2, write_timeout=2) as board:
                board.reset_input_buffer()
                board.write(f"\rwatch_weather {fields}\r".encode("ascii"))
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    response = board.readline()
                    if b"WEATHER OK" in response:
                        print(response.decode("ascii", errors="replace").strip(), flush=True)
                        break
                else:
                    raise OSError(f"{port} 未返回 WEATHER OK，请确认串口可用并已烧录天气固件")
        except (OSError, ValueError, KeyError, TypeError, serial.SerialException) as error:
            if not interval:
                raise SystemExit(f"天气同步失败：{error}") from error
            print(f"天气同步失败：{error}", flush=True)
        if not interval:
            return
        time.sleep(interval)


def default_out_dir():
    if os.path.exists("Z:\\"):
        return "Z:\\"
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "received_jpg")


def save_file(out_dir, fname, data):
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    safe_name = fname.replace("/", "_").replace("\\", "_")
    if not safe_name:
        safe_name = "capture.jpg"
    out_name = f"{ts}_{safe_name}"
    os.makedirs(out_dir, exist_ok=True)
    local_path = os.path.join(out_dir, out_name)
    with open(local_path, "wb") as f:
        f.write(data)
    return out_name


def recv_exact(conn, length):
    buf = bytearray()
    while len(buf) < length:
        chunk = conn.recv(length - len(buf))
        if not chunk:
            raise ConnectionError("EOF")
        buf.extend(chunk)
    return bytes(buf)


def handle_client(conn, addr, out_dir):
    print(f"\n[连接] {addr[0]}:{addr[1]}")
    file_count = 0
    total_bytes = 0

    while True:
        try:
            fname_len = struct.unpack(">I", recv_exact(conn, 4))[0]
            if fname_len == 0 or fname_len > 1024:
                raise ConnectionError("Invalid filename length")

            fname = recv_exact(conn, fname_len).rstrip(b"\x00").decode("utf-8", errors="replace")
            fsize = struct.unpack(">I", recv_exact(conn, 4))[0]
            if fsize == 0 or fsize > (16 * 1024 * 1024):
                raise ConnectionError("Invalid file size")

            data = recv_exact(conn, fsize)
            result = save_file(out_dir, fname, data)
            file_count += 1
            total_bytes += fsize
            print(f"[{file_count:04d}] {fname} ({fsize / 1024:.1f} KB) -> {result}")
        except (ConnectionError, BrokenPipeError, OSError):
            print(f"[断开] {addr[0]}:{addr[1]} (本次共接收 {file_count} 个文件, {total_bytes / 1024:.1f} KB)")
            break


def main():
    parser = argparse.ArgumentParser(description="M55 Wi-Fi TCP JPEG Relay Receiver")
    parser.add_argument("--host", default="0.0.0.0", help="Listen bind IP (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8888, help="Listen TCP port (default 8888)")
    parser.add_argument("--out", default=default_out_dir(), help="Output directory")
    parser.add_argument("--watch-info", action="store_true", help="Serve time and optional weather on UDP 8889")
    parser.add_argument("--watch-serial", metavar="PORT", help="Send fresh time and weather over the USB console")
    parser.add_argument("--watch-interval", type=int, default=0,
                        help="Repeat USB sync every N seconds (0 runs once)")
    parser.add_argument("--weather-city", default="shanghai", choices=WEATHER_CITIES,
                        help="Weather city preset (default shanghai)")
    parser.add_argument("--weather-location", nargs=2, type=float, metavar=("LAT", "LON"),
                        help="Request weather for these coordinates via Open-Meteo")
    args = parser.parse_args()
    location = WEATHER_CITIES[args.weather_city]
    if args.weather_location:
        lat, lon = args.weather_location
        if not (args.watch_info or args.watch_serial) or not (-90 <= lat <= 90 and -180 <= lon <= 180):
            parser.error("--weather-location requires watch mode and valid coordinates")
        location = (3, lat, lon)
    if args.watch_interval and (not args.watch_serial or args.watch_interval < 60):
        parser.error("--watch-interval requires --watch-serial and must be at least 60 seconds")
    if args.watch_serial:
        if args.watch_info:
            parser.error("Choose USB sync or LAN relay mode")
        try:
            sync_watch_serial(args.watch_serial, location, args.watch_interval)
        except KeyboardInterrupt:
            pass
        return

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(4)

    if args.watch_info:
        service = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        service.bind((args.host, WATCH_INFO_PORT))
        threading.Thread(target=serve_watch_info, args=(service, location), daemon=True).start()
        print(f"[表盘] 时间/天气 {args.host}:{WATCH_INFO_PORT}/UDP")

    try:
        ip = socket.gethostbyname(socket.gethostname())
    except OSError:
        ip = "127.0.0.1"

    print("========================================")
    print("  M55 JPG 接收器")
    print("========================================")
    print(f"  监听: {args.host}:{args.port}")
    print(f"  PC IP: {ip}")
    print(f"  存储: {out_dir}")
    print("========================================")
    print("  等待 M55 连接... (Ctrl+C 退出)")

    try:
        while True:
            conn, addr = srv.accept()
            try:
                handle_client(conn, addr, out_dir)
            finally:
                conn.close()
    except KeyboardInterrupt:
        print("\n中断退出")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
