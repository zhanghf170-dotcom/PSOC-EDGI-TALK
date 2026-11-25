#!/usr/bin/env python3
"""
Gemini-chan (Mocha) Cute Female Live TTS Bridge for Edgi-Talk XiaoZhi Cockpit
-----------------------------------------------------------------------------
Replaces XiaoZhi cloud TTS voice with Microsoft Neural TTS `zh-CN-XiaoyiNeural`
(cute, lively anime female voice, pitch +6Hz, rate +4%) streamed as 24 kHz mono
60 ms raw Opus packets.

Firewall-Free Architecture:
  - PC discovers the Edgi-Talk board on LAN via UDP probe (+ ARP/TCP fallback)
  - PC initiates an OUTBOUND persistent TCP connection to <board_ip>:8890
  - Works on Windows Private/Public networks with ZERO Firewall rules or Admin rights!

Usage:
    python tools/gemini_tts_bridge.py
    python tools/gemini_tts_bridge.py --board 192.168.1.100
    python tools/gemini_tts_bridge.py --voice zh-CN-XiaoyiNeural --pitch +6Hz --rate +4%
"""

import argparse
import asyncio
import collections
import concurrent.futures
import re
import socket
import struct
import subprocess
import sys
import threading
import time

try:
    import edge_tts
except ImportError:
    print("[ERROR] edge-tts not installed. Run: pip install -U edge-tts", file=sys.stderr)
    sys.exit(1)

DEFAULT_VOICE = "zh-CN-XiaoyiNeural"
DEFAULT_PITCH = "+6Hz"
DEFAULT_RATE  = "+4%"
DEFAULT_PORT  = 8890
CACHE_MAX     = 64

_RE_CONTROL_TAGS = re.compile(
    r"(?:%[A-Za-z_][A-Za-z0-9_.:/-]*(?:\([^)\r\n]*\)|\[[^\]\r\n]*\]|\{[^}\r\n]*\})?"
    r"|<tool_call\b.*?</tool_call>"
    r"|<function\b.*?</function>"
    r"|<\|.*?\|>)",
    re.DOTALL,
)


def sanitize_tts_text(raw: str) -> str:
    """Strip LLM/server control tags such as %getweather while preserving 46% percentages."""
    if not raw:
        return ""
    had_leading_tag = bool(_RE_CONTROL_TAGS.match(raw.lstrip()))
    cleaned = _RE_CONTROL_TAGS.sub(" ", raw).strip()
    if had_leading_tag:
        cleaned = cleaned.lstrip(":：,，;- \t")
    if not re.search(r"[0-9A-Za-z\u4e00-\u9fff]", cleaned):
        return ""
    return cleaned


def parse_ogg_opus_packets(ogg_bytes: bytes):
    """Extract raw Opus packets (skipping OpusHead and OpusTags) from Ogg stream."""
    packets = []
    pos = 0
    n = len(ogg_bytes)
    cur = bytearray()
    pkt_idx = 0
    while pos + 27 <= n:
        if ogg_bytes[pos : pos + 4] != b"OggS":
            break
        num_segs = ogg_bytes[pos + 26]
        if pos + 27 + num_segs > n:
            break
        seg_table = ogg_bytes[pos + 27 : pos + 27 + num_segs]
        data_pos = pos + 27 + num_segs
        for seg_len in seg_table:
            cur.extend(ogg_bytes[data_pos : data_pos + seg_len])
            data_pos += seg_len
            if seg_len < 255:
                if pkt_idx >= 2 and len(cur) > 0:
                    packets.append(bytes(cur))
                cur = bytearray()
                pkt_idx += 1
        pos = data_pos
    return packets


async def synthesize_to_framed_opus(text: str, voice: str, pitch: str, rate: str) -> bytes:
    """Synthesize UTF-8 text via edge-tts and transcode to framed 24kHz/60ms Opus packets."""
    t0 = time.perf_counter()
    mp3_chunks = bytearray()
    comm = edge_tts.Communicate(text, voice=voice, rate=rate, pitch=pitch)
    async for chunk in comm.stream():
        if chunk.get("type") == "audio":
            mp3_chunks.extend(chunk["data"])

    if not mp3_chunks:
        return struct.pack("<H", 0)

    proc = subprocess.run(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-f", "mp3", "-i", "pipe:0",
            "-af", "loudnorm=I=-15:TP=-1.5:LRA=7,aresample=24000",
            "-ac", "1", "-ar", "24000",
            "-c:a", "libopus", "-application", "voip",
            "-frame_duration", "60", "-b:a", "20k", "-vbr", "on",
            "-f", "ogg", "pipe:1",
        ],
        input=bytes(mp3_chunks),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    pkts = parse_ogg_opus_packets(proc.stdout)
    out = bytearray()
    for p in pkts:
        if len(p) <= 65535:
            out += struct.pack("<H", len(p))
            out += p
    out += struct.pack("<H", 0)  # end-of-sentence marker
    dt = (time.perf_counter() - t0) * 1000.0
    print(f"[TTS] '{text}' -> {len(pkts)} frames ({len(pkts)*0.06:.2f}s), {len(out)} B in {dt:.0f} ms", flush=True)
    return bytes(out)


class TtsCache:
    def __init__(self, cap=CACHE_MAX):
        self.cap = cap
        self.data = collections.OrderedDict()
        self.lock = threading.Lock()

    def get(self, key):
        with self.lock:
            v = self.data.get(key)
            if v is not None:
                self.data.move_to_end(key)
            return v

    def put(self, key, val):
        with self.lock:
            self.data[key] = val
            self.data.move_to_end(key)
            while len(self.data) > self.cap:
                self.data.popitem(last=False)


def is_private_lan_ip(ip: str) -> bool:
    if ip.startswith("192.168.") or ip.startswith("10."):
        return True
    if ip.startswith("172."):
        try:
            second = int(ip.split(".")[1])
            return 16 <= second <= 31
        except Exception:
            return False
    return False


def get_local_lan_ips():
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if is_private_lan_ip(ip):
                ips.add(ip)
    except Exception:
        pass
    return sorted(ips)


def udp_discover_board(port: int, timeout: float = 0.8):
    """Send outbound UDP probe to LAN broadcasts; Windows stateful firewall allows reply."""
    targets = {"255.255.255.255"}
    for ip in get_local_lan_ips():
        parts = ip.split(".")
        if len(parts) == 4:
            targets.add(f"{parts[0]}.{parts[1]}.{parts[2]}.255")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(timeout)
    try:
        for bcast in targets:
            try:
                sock.sendto(b"GEMTTS_PC_HELLO", (bcast, port))
            except Exception:
                pass
        t_end = time.monotonic() + timeout
        while time.monotonic() < t_end:
            try:
                data, addr = sock.recvfrom(128)
                if data.startswith(b"GEMTTS_BOARD") and is_private_lan_ip(addr[0]):
                    return addr[0]
            except socket.timeout:
                break
            except Exception:
                break
    finally:
        sock.close()
    return None


def probe_tcp_port(ip: str, port: int, timeout: float = 0.6):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((ip, port))
        banner = s.recv(64)
        s.close()
        if banner.startswith(b"GEMTTS_READY"):
            return ip
        return None
    except Exception:
        try:
            s.close()
        except Exception:
            pass
        return None


def arp_discover_board(port: int):
    """Fallback: scan private LAN ARP neighbors and verify GEMTTS_READY banner."""
    try:
        out = subprocess.check_output(["arp", "-a"], text=True, errors="ignore")
    except Exception:
        return None
    candidates = []
    for m in re.finditer(r"(\d+\.\d+\.\d+\.\d+)\s+([0-9a-fA-F-]{17})\s+dynamic", out):
        ip, mac = m.group(1), m.group(2).lower()
        if not is_private_lan_ip(ip) or ip.endswith(".255") or ip.endswith(".1"):
            continue
        if mac.startswith("9c-c7-d3"):
            candidates.insert(0, ip)
        else:
            candidates.append(ip)
    if not candidates:
        return None
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        futs = [pool.submit(probe_tcp_port, ip, port) for ip in candidates[:40]]
        for f in concurrent.futures.as_completed(futs):
            res = f.result()
            if res:
                return res
    return None


def discover_board(port: int, pinned_ip: str = ""):
    if pinned_ip:
        return pinned_ip
    ip = udp_discover_board(port, timeout=0.8)
    if ip:
        return ip
    return arp_discover_board(port)


def serve_board_connection(board_ip: str, args, cache: TtsCache):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(4.0)
    sock.connect((board_ip, args.port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    if hasattr(socket, "SIO_KEEPALIVE_VALS"):
        # Detect hard board reset within ~6s and auto-reconnect cleanly
        sock.ioctl(socket.SIO_KEEPALIVE_VALS, (1, 5000, 1500))

    # Verify authentic Edgi-Talk board banner (prevents VPN/proxy false connects)
    banner = bytearray()
    while b"\n" not in banner and len(banner) < 64:
        ch = sock.recv(64)
        if not ch:
            sock.close()
            raise RuntimeError("closed before GEMTTS_READY handshake")
        banner.extend(ch)
    head, _, rest = banner.partition(b"\n")
    if head.strip() != b"GEMTTS_READY":
        sock.close()
        raise RuntimeError(f"unexpected peer banner: {head!r}")

    sock.settimeout(None)
    print(f"[LINK] Connected to Edgi-Talk board {board_ip}:{args.port} (ready for live Xiaoyi TTS!)", flush=True)

    buf = bytearray(rest)
    try:
        while True:
            chunk = sock.recv(1024)
            if not chunk:
                print("[LINK] Board closed connection, will reconnect...", flush=True)
                return
            buf.extend(chunk)
            while b"\n" in buf:
                line_bytes, _, rest = buf.partition(b"\n")
                buf = bytearray(rest)
                raw_text = line_bytes.decode("utf-8", errors="ignore").strip()
                text = sanitize_tts_text(raw_text)
                if not text:
                    if raw_text:
                        print(f"[TTS] filtered control tag: {raw_text!r}", flush=True)
                    sock.sendall(struct.pack("<H", 0))
                    continue

                cache_key = (args.voice, args.pitch, args.rate, text)
                framed = cache.get(cache_key)
                if framed is None:
                    try:
                        framed = asyncio.run(
                            synthesize_to_framed_opus(text, args.voice, args.pitch, args.rate)
                        )
                        cache.put(cache_key, framed)
                    except Exception as e:
                        print(f"[TTS] synthesis error: {e}", flush=True)
                        framed = struct.pack("<H", 0)
                else:
                    print(f"[TTS] (cache) '{text}' -> {len(framed)} B", flush=True)

                sock.sendall(framed)
    finally:
        try:
            sock.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(description="Gemini-chan XiaoyiNeural Live TTS Bridge")
    ap.add_argument("--board", default="", help="Optional board IP (auto-discovered if omitted)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="Board TTS port (default: 8890)")
    ap.add_argument("--voice", default=DEFAULT_VOICE, help=f"Edge TTS voice (default: {DEFAULT_VOICE})")
    ap.add_argument("--pitch", default=DEFAULT_PITCH, help=f"Pitch shift (default: {DEFAULT_PITCH})")
    ap.add_argument("--rate", default=DEFAULT_RATE, help=f"Speech rate (default: {DEFAULT_RATE})")
    args = ap.parse_args()

    cache = TtsCache()
    print("=== Gemini-chan Cute Voice Live TTS Bridge ===", flush=True)
    print(f"  Voice : {args.voice} (pitch={args.pitch}, rate={args.rate})", flush=True)
    print(f"  Mode  : Firewall-free outbound TCP -> board:{args.port}", flush=True)

    last_ip = args.board
    while True:
        target_ip = last_ip or discover_board(args.port, args.board)
        if not target_ip:
            print("[FIND] Searching for Edgi-Talk board on LAN...", flush=True)
            time.sleep(2.0)
            continue

        try:
            serve_board_connection(target_ip, args, cache)
            time.sleep(0.4)
        except Exception as e:
            print(f"[LINK] {target_ip}:{args.port} -> {e}", flush=True)
            if not args.board:
                last_ip = ""
            time.sleep(1.5)


if __name__ == "__main__":
    main()
