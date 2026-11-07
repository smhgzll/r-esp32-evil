#!/usr/bin/env python3
"""re1tool.py - one tool for the r-esp32-evil workflow.

  fetch      check out the decomp at the pinned commit and apply patches/
  prepare    build the SD-card tree from your own copy of the game
  sdcard     copy a prepared tree onto a mounted card (optionally format it)
  build      build the firmware (board profile)          [needs ESP-IDF]
  flash      flash the firmware to the board               [needs ESP-IDF]
  monitor    serial console of the board (2 Mbaud)
  qemu       build the QEMU profile and run it with the browser view
  host       build and run the PC host simulator with the browser view
  web        browser view/keyboard bridge for a serial port or TCP socket
  room       jump to a room on the running target (room tour, no cutscenes)
  cmd        send a text command to the running target
  check      sanity-check a prepared tree

Run `re1tool.py <command> -h` for the options of each command.
Everything is documented in docs/GUIDE.md.

No game data is part of this project: `prepare` converts the files of a
copy of Resident Evil (PC, 1997 / GOG) that you own.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import http.server
import json
import os
import shutil
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time
import wave
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware"
WEB_DIR = ROOT / "tools" / "web"

FB_W, FB_H = 160, 120
RV1_HEADER = struct.Struct("<4sHHHBBIIIIIIIHHI16s")   # 64 bytes, see port/include/re1_rv1.h
assert RV1_HEADER.size == 64

# In-memory bank limit of the board's mixer (RE1_AUDIO_STREAM_THRESHOLD):
# WAVs whose PCM is smaller are loaded whole into PSRAM, bigger ones stream.
STREAM_THRESHOLD = 64 * 1024


def log(msg: str) -> None:
    print(msg, flush=True)


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


# ===========================================================================
# fetch
# ===========================================================================
def cmd_fetch(args) -> None:
    script = ROOT / "tools" / "fetch_decomp.sh"
    cmd = ["bash", str(script)] + (["--update"] if args.update else [])
    sys.exit(subprocess.call(cmd))


# ===========================================================================
# prepare
# ===========================================================================
def find_usa_tree(game: Path) -> Path:
    """Accept the USA folder itself, the GOG install root, or its language dir."""
    candidates = [game, game / "USA", game / "usa", game / "english" / "USA",
                  game / "English" / "USA"]
    for c in candidates:
        if c.is_dir() and any((c / d).is_dir() for d in ("Stage1", "STAGE1", "stage1")):
            return c
    for c in game.rglob("*"):
        if c.is_dir() and c.name.lower() == "usa" and any(
                (c / d).is_dir() for d in ("Stage1", "STAGE1", "stage1")):
            return c
    die(f"no USA data tree (with Stage1/) under {game}")
    raise SystemExit


def wav_info(path: Path):
    try:
        with wave.open(str(path), "rb") as w:
            return w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    except (wave.Error, EOFError):
        return None


def convert_wav_8bit(src: Path, dst: Path) -> None:
    """16-bit PCM -> 8-bit unsigned PCM (halves PSRAM for in-memory banks)."""
    import numpy as np
    with wave.open(str(src), "rb") as w:
        ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        data = w.readframes(n)
    s = np.frombuffer(data, dtype="<i2").astype(np.int32)
    # TPDF dither keeps quiet tails from turning into buzz.
    rng = np.random.default_rng(len(s))
    d = (rng.integers(-128, 128, s.shape) + rng.integers(-128, 128, s.shape)) // 2
    u8 = np.clip((s + d + 32768) >> 8, 0, 255).astype(np.uint8)
    with wave.open(str(dst), "wb") as o:
        o.setnchannels(ch)
        o.setsampwidth(1)
        o.setframerate(rate)
        o.writeframes(u8.tobytes())


def rgb_to_565(rgb):
    import numpy as np
    r = (rgb[..., 0].astype(np.uint16) >> 3) << 11
    g = (rgb[..., 1].astype(np.uint16) >> 2) << 5
    b = rgb[..., 2].astype(np.uint16) >> 3
    return (r | g | b).astype("<u2")


def probe_movie(src: Path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=r_frame_rate:format=duration", "-of", "json", str(src)],
        capture_output=True, text=True, check=True).stdout
    j = json.loads(out)
    num, den = (int(x) for x in j["streams"][0]["r_frame_rate"].split("/"))
    dur = float(j["format"].get("duration", 0.0))
    return num, den, dur


def convert_movie(src: str, dst: str, codec: str, audio_rate: int, w: int, h: int) -> str:
    """AVI (Cinepak) -> .rv1 (see port/include/re1_rv1.h)."""
    import numpy as np
    from PIL import Image

    srcp, dstp = Path(src), Path(dst)
    num, den, dur = probe_movie(srcp)
    tmp_audio = dstp.with_suffix(".pcm.tmp")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(srcp), "-vn", "-ac", "1",
                    "-ar", str(audio_rate), "-f", "s16le", str(tmp_audio)], check=False)
    audio = tmp_audio.read_bytes() if tmp_audio.exists() else b""
    tmp_audio.unlink(missing_ok=True)

    proc = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-i", str(srcp), "-an", "-vf",
         f"scale={w}:{h}:flags=area", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE)
    frame_bytes = w * h * 3
    frames = []
    while True:
        buf = proc.stdout.read(frame_bytes)
        if len(buf) < frame_bytes:
            break
        rgb = np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 3)
        if codec == "rgb565":
            frames.append(rgb_to_565(rgb).tobytes())
        else:
            img = Image.fromarray(rgb, "RGB").quantize(256, method=Image.Quantize.FASTOCTREE,
                                                       dither=Image.Dither.NONE)
            pal = np.array(img.getpalette()[:768], dtype=np.uint8)
            pal = np.pad(pal, (0, 768 - len(pal))).reshape(256, 3)
            frames.append(rgb_to_565(pal).tobytes() + np.asarray(img, dtype=np.uint8).tobytes())
    proc.wait()
    if not frames:
        return f"{srcp.name}: no frames decoded"

    count = len(frames)
    index_off = 64
    data_off = index_off + count * 8
    index = bytearray()
    off = data_off
    for f in frames:
        index += struct.pack("<II", off, len(f))
        off += len(f)
    audio_off = off if audio else 0
    duration_ms = int(round(count * 1000 * den / num)) if num else int(dur * 1000)
    hdr = RV1_HEADER.pack(b"RV1\0", 1, w, h, 0 if codec == "rgb565" else 1, 0, num, den,
                          count, index_off, audio_off, len(audio) // 2, audio_rate, 1, 16,
                          duration_ms, b"\0" * 16)
    with open(dstp, "wb") as o:
        o.write(hdr)
        o.write(index)
        for f in frames:
            o.write(f)
        o.write(audio)
    return f"{srcp.name}: {count} frames, {duration_ms / 1000:.1f}s, {dstp.stat().st_size // 1024} KB"


DEVICE_CONFIG = """; config.ini - r-esp32-evil (written by tools/re1tool.py prepare)
;
; Same file format as the PC decomp. The board ignores the display settings
; (the panel is fixed) and HD textures (no room for them). The [Input] keys are
; written by the game on exit.

[Display]
FullScreen=1
Width=320
Height=240
BitDepth=16
VSync=0

[Assets]
Path=
Version=USA

[Save]
Path=

[Game]
Mode=OG
Ps1EndingCredits=0
SkipUnskippableFmv={skip_fmv}

[Textures]
HdTextures=0
DumpTextures=0

[Debug]
EnableDebug=0

[Port]
; r-esp32-evil only. Text is laid out again for the small panel: 1.0 = the
; original layout, 2.0 = the font at its native pixel size (sharpest; long
; lines are re-wrapped), values between work but look less crisp.
; TextScale = messages / descriptions, MenuTextScale = the 8x8 menu font.
TextScale=2.0
MenuTextScale=2.0
"""


def cmd_prepare(args) -> None:
    game = Path(args.game).expanduser()
    out = Path(args.out).expanduser()
    usa = find_usa_tree(game)
    dst_root = out / "re1"
    dst_usa = dst_root / "usa"
    log(f"[prepare] game tree : {usa}")
    log(f"[prepare] output    : {dst_root}  (copy the 're1' folder to the card root)")
    if dst_usa.exists() and not args.force:
        log("[prepare] output exists - only missing/changed files are written (--force rebuilds)")
    dst_usa.mkdir(parents=True, exist_ok=True)
    (dst_root / "save").mkdir(exist_ok=True)

    movies = []
    stats = {"copied": 0, "sfx8": 0, "skipped": 0, "bytes": 0}
    for src in sorted(usa.rglob("*")):
        if src.is_dir():
            continue
        rel = src.relative_to(usa)
        rel_l = Path(*[p.lower() for p in rel.parts])
        dst = dst_usa / rel_l
        dst.parent.mkdir(parents=True, exist_ok=True)
        top = rel_l.parts[0]
        ext = src.suffix.lower()

        if top == "movie" and ext == ".avi":
            if not args.no_movies:
                movies.append((src, dst.with_suffix(".rv1")))
            if args.keep_avi:
                pass
            else:
                continue
        up_to_date = not args.force and dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime

        if ext == ".wav" and top in ("sound", "voice") and args.audio == "sfx8":
            info = wav_info(src)
            # Banks the board keeps in RAM (after conversion <= threshold) go
            # 8-bit; banks that stream from the card stay 16-bit.
            if info and info[1] == 2 and info[0] * info[3] * 2 <= 2 * STREAM_THRESHOLD:
                if not up_to_date:
                    convert_wav_8bit(src, dst)
                stats["sfx8"] += 1
                stats["bytes"] += dst.stat().st_size
                continue
        if up_to_date:
            stats["skipped"] += 1
            stats["bytes"] += dst.stat().st_size
            continue
        shutil.copy2(src, dst)
        stats["copied"] += 1
        stats["bytes"] += dst.stat().st_size

    log(f"[prepare] files: {stats['copied']} copied, {stats['sfx8']} sound banks -> 8-bit, "
        f"{stats['skipped']} up to date")

    if movies:
        todo = [(s, d) for s, d in movies
                if args.force or not d.exists() or d.stat().st_mtime < s.stat().st_mtime]
        log(f"[prepare] movies: {len(todo)} to convert ({args.video_codec}, {FB_W}x{FB_H})")
        with ProcessPoolExecutor(max_workers=args.jobs) as ex:
            futs = [ex.submit(convert_movie, str(s), str(d), args.video_codec, 22050, FB_W, FB_H)
                    for s, d in todo]
            for f in as_completed(futs):
                log(f"[prepare]   {f.result()}")
        for _, d in movies:
            if d.exists():
                stats["bytes"] += d.stat().st_size

    cfg = dst_root / "config.ini"
    if args.force or not cfg.exists():
        cfg.write_text(DEVICE_CONFIG.format(skip_fmv=1 if args.skippable_fmv else 0))
    total = sum(p.stat().st_size for p in dst_root.rglob("*") if p.is_file())
    log(f"[prepare] done: {total / 1e6:.1f} MB in {dst_root}")
    log("[prepare] next: python3 tools/re1tool.py sdcard --mount <card mount point> "
        f"--from {out}")


# ===========================================================================
# check
# ===========================================================================
REQUIRED = ["usa/data", "usa/stage1", "usa/enemy", "usa/players", "usa/sound", "usa/objspr",
            "usa/effspr", "usa/item_m1", "usa/item_m2", "save"]


def cmd_check(args) -> None:
    root = Path(args.tree).expanduser()
    if (root / "re1").is_dir():
        root = root / "re1"
    ok = True
    for r in REQUIRED:
        if not (root / r).is_dir():
            log(f"  MISSING {r}/")
            ok = False
    for p in root.rglob("*"):
        if p.name != p.name.lower():
            log(f"  NOT LOWER-CASE {p.relative_to(root)} (QEMU semihosting needs lower case)")
            ok = False
            break
    movies = list((root / "usa" / "movie").glob("*.rv1")) if (root / "usa/movie").is_dir() else []
    for m in movies:
        with open(m, "rb") as f:
            h = RV1_HEADER.unpack(f.read(64))
        if h[0] != b"RV1\0":
            log(f"  BAD MOVIE {m.name}")
            ok = False
    total = sum(p.stat().st_size for p in root.rglob("*") if p.is_file())
    log(f"{root}: {len(movies)} movies, {total / 1e6:.1f} MB -> {'OK' if ok else 'PROBLEMS'}")
    sys.exit(0 if ok else 1)


# ===========================================================================
# sdcard
# ===========================================================================
def cmd_sdcard(args) -> None:
    src = Path(args.src).expanduser()
    if (src / "re1").is_dir():
        src = src / "re1"
    if not (src / "usa").is_dir():
        die(f"{src} is not a prepared tree (run `prepare` first)")

    if args.format:
        dev = args.format
        if not dev.startswith("/dev/"):
            die("--format takes a block device such as /dev/sdb1")
        log(f"About to FORMAT {dev} as FAT32 (label RE1). Everything on it is erased.")
        if not args.yes and input("Type the device name again to confirm: ").strip() != dev:
            die("not confirmed")
        subprocess.run(["sudo", "mkfs.vfat", "-F", "32", "-s", "64", "-n", "RE1", dev], check=True)
        log(f"Formatted. Mount it (e.g. `udisksctl mount -b {dev}`) and run again with --mount.")
        if not args.mount:
            return

    if not args.mount:
        die("give --mount <mount point of the card>")
    mnt = Path(args.mount).expanduser()
    if not mnt.is_dir():
        die(f"{mnt} is not a directory")
    dst = mnt / "re1"
    files = [p for p in src.rglob("*") if p.is_file()]
    total = sum(p.stat().st_size for p in files)
    free = shutil.disk_usage(mnt).free
    log(f"[sdcard] {len(files)} files, {total / 1e6:.1f} MB -> {dst} ({free / 1e9:.1f} GB free)")
    if total > free:
        die("not enough space on the card")
    done = 0
    copied = 0
    for p in files:
        rel = p.relative_to(src)
        q = dst / rel
        q.parent.mkdir(parents=True, exist_ok=True)
        if not q.exists() or q.stat().st_size != p.stat().st_size:
            shutil.copyfile(p, q)
            copied += 1
        done += p.stat().st_size
        if copied and copied % 200 == 0:
            log(f"[sdcard]   {done * 100 // max(total, 1)}%")
    (dst / "save").mkdir(exist_ok=True)
    log(f"[sdcard] {copied} files written. Syncing...")
    os.sync()
    log("[sdcard] done - unmount the card before removing it.")


# ===========================================================================
# ESP-IDF helpers
# ===========================================================================
def find_idf() -> Path:
    env = os.environ.get("IDF_PATH")
    if env and (Path(env) / "export.sh").exists():
        return Path(env)
    for c in sorted((Path.home() / "esp").glob("esp-idf*"), reverse=True):
        if (c / "export.sh").exists():
            return c
    die("ESP-IDF not found: set IDF_PATH or install it under ~/esp (docs/GUIDE.md)")
    raise SystemExit


def idf(argv: list[str], cwd: Path = FIRMWARE, env_extra: dict | None = None) -> int:
    idf_path = find_idf()
    cmd = f". '{idf_path}/export.sh' >/dev/null && idf.py " + " ".join(
        "'" + a.replace("'", "'\\''") + "'" for a in argv)
    env = dict(os.environ)
    env.update(env_extra or {})
    return subprocess.call(["bash", "-c", cmd], cwd=cwd, env=env)


def ensure_decomp() -> None:
    if not (ROOT / "decomp" / "src" / "Globals.cpp").exists():
        log("[re1tool] decomp/ missing - fetching it first")
        if subprocess.call(["bash", str(ROOT / "tools" / "fetch_decomp.sh")]) != 0:
            die("fetch failed")


QEMU_BUILD = FIRMWARE / "build-qemu"


def qemu_idf_args() -> list[str]:
    return ["-B", str(QEMU_BUILD), "-D", f"SDKCONFIG={QEMU_BUILD / 'sdkconfig'}",
            "-D", "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu"]


def cmd_build(args) -> None:
    ensure_decomp()
    if args.qemu:
        sys.exit(idf(qemu_idf_args() + ["build"]))
    if not (FIRMWARE / "build").exists():
        idf(["set-target", "esp32s3"])
    sys.exit(idf(["build"]))


def cmd_flash(args) -> None:
    ensure_decomp()
    argv = ["-p", args.port, "-b", str(args.baud), "flash"]
    if args.monitor:
        argv += ["monitor", "-b", "2000000"]
    sys.exit(idf(argv))


def cmd_monitor(args) -> None:
    sys.exit(idf(["-p", args.port, "monitor", "-b", "2000000"]))


# ===========================================================================
# Browser bridge
# ===========================================================================
class Link:
    """Byte link to the target: serial port or TCP socket."""

    def __init__(self, serial_port: str | None, tcp: str | None, baud: int):
        self.serial = None
        self.sock = None
        self.lock = threading.Lock()
        self.serial_port, self.baud = serial_port, baud
        if serial_port:
            self._open_serial()
        else:
            host, port = tcp.rsplit(":", 1)
            deadline = time.time() + 60
            while True:
                try:
                    self.sock = socket.create_connection((host, int(port)), timeout=2)
                    break
                except OSError:
                    if time.time() > deadline:
                        raise
                    time.sleep(0.5)
            self.sock.settimeout(0.05)
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def _open_serial(self):
        """Open without touching DTR/RTS: on the ESP32-S3's own USB port that
        sequence resets the chip (and the port vanishes while it re-enumerates)."""
        import serial  # pyserial
        deadline = time.time() + 30
        while True:
            try:
                s = serial.Serial()
                s.port = self.serial_port
                s.baudrate = self.baud
                s.timeout = 0.05
                s.dtr = False
                s.rts = False
                s.open()
                self.serial = s
                return
            except (OSError, serial.SerialException):
                if time.time() > deadline:
                    raise
                time.sleep(0.5)

    def read(self) -> bytes:
        try:
            if self.serial:
                try:
                    return self.serial.read(65536)
                except Exception:  # noqa: BLE001 - device dropped (board reset): reopen
                    try:
                        self.serial.close()
                    except Exception:  # noqa: BLE001
                        pass
                    log("[bridge] serial port lost, reconnecting...")
                    time.sleep(0.5)
                    self._open_serial()
                    log("[bridge] serial port back")
                    return b""
            data = self.sock.recv(65536)
            if data == b"":
                raise ConnectionError("target closed the link")
            return data
        except (socket.timeout, TimeoutError):
            return b""

    def write(self, data: bytes) -> None:
        with self.lock:
            if self.serial:
                try:
                    self.serial.write(data)
                except Exception:  # noqa: BLE001 - the reader reopens the port
                    pass
            else:
                self.sock.sendall(data)


class Demux:
    """Split the target's byte stream into RVL packets and console text."""

    def __init__(self):
        self.buf = bytearray()
        self.corrupt = False   # a packet was cut by console text: ask for a key frame

    def feed(self, data: bytes):
        self.buf += data
        packets, text = [], bytearray()
        b = self.buf
        i = 0
        while i < len(b):
            if b[i] == 0xA5:
                if i + 1 >= len(b):
                    break
                if b[i + 1] == 0x5A:
                    if i + 7 > len(b):
                        break
                    ln = struct.unpack_from("<I", b, i + 3)[0]
                    if ln > 4 * 1024 * 1024:
                        text.append(b[i])
                        i += 1
                        continue
                    end = i + 7 + ln + 1
                    if end > len(b):
                        break
                    if b[end - 1] == 0x5A:
                        packets.append(bytes(b[i:end]))
                        i = end
                        continue
                    self.corrupt = True
            text.append(b[i])
            i += 1
        del self.buf[:i]
        return packets, text.decode("utf-8", "replace")


class WsClient:
    def __init__(self, conn):
        self.conn = conn
        self.lock = threading.Lock()
        self.alive = True

    def send(self, payload: bytes, binary: bool = True) -> None:
        if not self.alive:
            return
        op = 0x82 if binary else 0x81
        n = len(payload)
        if n < 126:
            hdr = struct.pack("!BB", op, n)
        elif n < 65536:
            hdr = struct.pack("!BBH", op, 126, n)
        else:
            hdr = struct.pack("!BBQ", op, 127, n)
        try:
            with self.lock:
                self.conn.sendall(hdr + payload)
        except OSError:
            self.alive = False

    def recv(self):
        def exact(n):
            out = b""
            while len(out) < n:
                chunk = self.conn.recv(n - len(out))
                if not chunk:
                    raise ConnectionError
                out += chunk
            return out
        b0, b1 = exact(2)
        op, ln = b0 & 0x0F, b1 & 0x7F
        if ln == 126:
            ln = struct.unpack("!H", exact(2))[0]
        elif ln == 127:
            ln = struct.unpack("!Q", exact(8))[0]
        mask = exact(4) if b1 & 0x80 else b"\0\0\0\0"
        data = bytes(c ^ mask[i % 4] for i, c in enumerate(exact(ln)))
        return op, data


def run_bridge(link: Link, http_port: int, open_browser: bool) -> None:
    clients: list[WsClient] = []
    clients_lock = threading.Lock()

    def broadcast(payload: bytes, binary=True):
        with clients_lock:
            for c in list(clients):
                c.send(payload, binary)
                if not c.alive:
                    clients.remove(c)

    def reader():
        demux = Demux()
        line = ""
        while True:
            try:
                data = link.read()
            except Exception as e:  # noqa: BLE001
                broadcast(f"[bridge] link lost: {e}\n".encode(), binary=False)
                log(f"[bridge] link lost: {e}")
                return
            if not data:
                continue
            packets, text = demux.feed(data)
            if demux.corrupt:
                # The delta frames that follow would draw on a wrong picture.
                demux.corrupt = False
                link.write(bytes([0xA5, 0x5A, ord("R"), 0, 0, 0, 0, 0x5A]))
            for p in packets:
                broadcast(p)
            if text:
                sys.stdout.write(text)
                sys.stdout.flush()
                broadcast(text.encode(), binary=False)

    class Handler(http.server.SimpleHTTPRequestHandler):
        protocol_version = "HTTP/1.1"   # RFC 6455: the upgrade answer must be 1.1

        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(WEB_DIR), **kw)

        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path != "/ws":
                return super().do_GET()
            key = self.headers.get("Sec-WebSocket-Key", "")
            accept = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            ws = WsClient(self.connection)
            with clients_lock:
                clients.append(ws)
            link.write(bytes([0xA5, 0x5A, ord("R"), 0, 0, 0, 0, 0x5A]))   # key frame please
            try:
                while ws.alive:
                    op, data = ws.recv()
                    if op == 8:
                        break
                    if op == 2 and data[:2] == b"\xa5\x5a":
                        link.write(data)   # K / P / R packets from the page
            except (ConnectionError, OSError):
                pass
            ws.alive = False

    class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True
        allow_reuse_address = True

    threading.Thread(target=reader, daemon=True).start()
    srv = Server(("127.0.0.1", http_port), Handler)
    url = f"http://127.0.0.1:{http_port}/"
    log(f"[bridge] open {url}   (keys: arrows = move, C = action, V = run/cancel, "
        "X = aim, Z = inventory, Esc = options)")
    if open_browser:
        import webbrowser
        threading.Timer(1.0, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


def cmd_web(args) -> None:
    if not args.serial and not args.tcp:
        die("give --serial PORT or --tcp HOST:PORT")
    run_bridge(Link(args.serial, args.tcp, args.baud), args.http, not args.no_open)


# ===========================================================================
# qemu
# ===========================================================================
def find_qemu() -> str:
    q = shutil.which("qemu-system-xtensa")
    if q:
        return q
    for c in sorted((Path.home() / ".espressif" / "tools" / "qemu-xtensa").glob(
            "*/qemu/bin/qemu-system-xtensa"), reverse=True):
        return str(c)
    die("qemu-system-xtensa not found: python $IDF_PATH/tools/idf_tools.py install qemu-xtensa")
    raise SystemExit


def cmd_qemu(args) -> None:
    ensure_decomp()
    data = Path(args.data).expanduser().resolve()
    if (data / "re1").is_dir():
        pass
    elif data.name == "re1":
        data = data.parent
    else:
        die(f"{data} has no re1/ folder (run `prepare --out {data}` first)")
    if not args.no_build:
        if idf(qemu_idf_args() + ["build"]) != 0:
            die("QEMU firmware build failed")
    flash = QEMU_BUILD / "qemu_flash.bin"
    if idf(qemu_idf_args() + ["merge-bin", "-o", str(flash), "--fill-flash-size", "16MB"]) != 0:
        die("merge-bin failed")

    run_dir = QEMU_BUILD / "run"
    run_dir.mkdir(exist_ok=True)
    link = run_dir / "sdcard"
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(data)

    port = args.serial_port
    qemu_cmd = [find_qemu(), "-machine", "esp32s3", "-m", "8M",
                "-drive", f"file={flash},if=mtd,format=raw",
                "-semihosting-config", "enable=on,target=native",
                "-display", "none", "-monitor", "none",
                "-serial", f"tcp:127.0.0.1:{port},server=on,wait=on"]
    log("[qemu] " + " ".join(qemu_cmd))
    qp = subprocess.Popen(qemu_cmd, cwd=run_dir)
    try:
        time.sleep(0.5)
        run_bridge(Link(None, f"127.0.0.1:{port}", 0), args.http, not args.no_open)
    finally:
        qp.terminate()


# ===========================================================================
# host
# ===========================================================================
def cmd_host(args) -> None:
    ensure_decomp()
    build = ROOT / "build" / "host"
    if not args.no_build:
        if subprocess.call(["cmake", "-S", str(ROOT / "port" / "host"), "-B", str(build),
                            "-DCMAKE_BUILD_TYPE=RelWithDebInfo"]) != 0:
            die("cmake configure failed")
        if subprocess.call(["cmake", "--build", str(build), "-j", str(os.cpu_count() or 4)]) != 0:
            die("host build failed")
    data = Path(args.data).expanduser().resolve()
    if (data / "re1").is_dir():
        data = data / "re1"
    exe = build / "re1_host"
    hp = subprocess.Popen([str(exe), "--data", str(data), "--port", str(args.port)])
    try:
        run_bridge(Link(None, f"127.0.0.1:{args.port}", 0), args.http, not args.no_open)
    finally:
        hp.terminate()


# ===========================================================================
# room / cmd: text commands through a running bridge (`web`, `qemu`, `host`)
# ===========================================================================
def send_command(text: str, http_port: int) -> None:
    """Send a 'C' packet to the target behind the bridge's websocket."""
    payload = text.encode()[:56]
    try:
        s = socket.create_connection(("127.0.0.1", http_port), timeout=3)
    except OSError:
        die(f"no bridge on 127.0.0.1:{http_port} - start `re1tool.py web` (or qemu/host) first")
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{http_port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = s.recv(4096)
        if not chunk:
            die("the bridge closed the connection")
        head += chunk
    if b" 101 " not in head.split(b"\r\n", 1)[0]:
        die("the bridge refused the websocket")
    packet = bytes([0xA5, 0x5A, ord("C")]) + struct.pack("<I", len(payload)) + payload + b"\x5a"
    mask = os.urandom(4)
    frame = bytes([0x82, 0x80 | len(packet)]) + mask + bytes(
        b ^ mask[i % 4] for i, b in enumerate(packet))
    s.sendall(frame)
    s.sendall(bytes([0x88, 0x80]) + os.urandom(4))    # close
    s.close()
    log(f"sent: {text}")


def cmd_room(args) -> None:
    room = args.room.lower().removeprefix("0x").removeprefix("room")
    try:
        int(room, 16)
    except ValueError:
        die("room is a hex id like 112 (stage 1, room 0x12) or 20B")
    if args.auto:
        send_command("auto " + room, args.http)
    else:
        send_command(f"room {room}" + (f" {args.camera}" if args.camera else ""), args.http)


def cmd_cmd(args) -> None:
    send_command(" ".join(args.text), args.http)


# ===========================================================================
def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("fetch", help="fetch the decomp + apply patches")
    p.add_argument("--update", action="store_true", help="git fetch even if present")
    p.set_defaults(fn=cmd_fetch)

    p = sub.add_parser("prepare", help="build the SD-card tree from your game copy")
    p.add_argument("--game", required=True, help="GOG install dir, its english/ dir, or USA/")
    p.add_argument("--out", required=True, help="output directory (gets re1/ inside)")
    p.add_argument("--audio", choices=["sfx8", "keep"], default="sfx8",
                   help="sfx8: in-RAM sound banks to 8-bit (default); keep: copy as-is")
    p.add_argument("--video-codec", choices=["pal8", "rgb565"], default="pal8",
                   help="pal8: 256 colours/frame, ~20 KB/frame (default); rgb565: 38 KB")
    p.add_argument("--no-movies", action="store_true", help="skip FMV conversion")
    p.add_argument("--keep-avi", action="store_true", help="also copy the original AVIs")
    p.add_argument("--skippable-fmv", action="store_true",
                   help="let every movie be skipped ([Game] SkipUnskippableFmv=1)")
    p.add_argument("--force", action="store_true", help="rebuild every file")
    p.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    p.set_defaults(fn=cmd_prepare)

    p = sub.add_parser("check", help="sanity-check a prepared tree")
    p.add_argument("tree")
    p.set_defaults(fn=cmd_check)

    p = sub.add_parser("sdcard", help="copy a prepared tree to a mounted SD card")
    p.add_argument("--from", dest="src", required=True, help="prepare --out directory")
    p.add_argument("--mount", help="mount point of the card")
    p.add_argument("--format", metavar="DEV", help="format DEV (e.g. /dev/sdb1) as FAT32 first")
    p.add_argument("--yes", action="store_true", help="do not ask before formatting")
    p.set_defaults(fn=cmd_sdcard)

    p = sub.add_parser("build", help="build the firmware")
    p.add_argument("--qemu", action="store_true", help="QEMU profile (firmware/build-qemu)")
    p.set_defaults(fn=cmd_build)

    p = sub.add_parser("flash", help="flash the board")
    p.add_argument("-p", "--port", required=True, help="e.g. /dev/ttyUSB0, /dev/ttyACM0, COM5")
    p.add_argument("-b", "--baud", type=int, default=921600)
    p.add_argument("--monitor", action="store_true", help="open the console afterwards")
    p.set_defaults(fn=cmd_flash)

    p = sub.add_parser("monitor", help="serial console (2 Mbaud)")
    p.add_argument("-p", "--port", required=True)
    p.set_defaults(fn=cmd_monitor)

    p = sub.add_parser("web", help="browser view + keyboard for a serial/TCP target")
    p.add_argument("--serial", help="serial port of the board (UART0 / COM port)")
    p.add_argument("--baud", type=int, default=2000000)
    p.add_argument("--tcp", help="HOST:PORT (QEMU serial socket, host simulator)")
    p.add_argument("--http", type=int, default=8080)
    p.add_argument("--no-open", action="store_true", help="do not open a browser")
    p.set_defaults(fn=cmd_web)

    p = sub.add_parser("qemu", help="run the firmware in QEMU with the browser view")
    p.add_argument("--data", required=True, help="prepare --out directory")
    p.add_argument("--serial-port", type=int, default=5555)
    p.add_argument("--http", type=int, default=8080)
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--no-open", action="store_true")
    p.set_defaults(fn=cmd_qemu)

    p = sub.add_parser("host", help="run the PC host simulator with the browser view")
    p.add_argument("--data", required=True, help="prepare --out directory (or a game tree)")
    p.add_argument("--port", type=int, default=7720)
    p.add_argument("--http", type=int, default=8080)
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--no-open", action="store_true")
    p.set_defaults(fn=cmd_host)

    p = sub.add_parser("room", help="jump to a room on the running target (room tour)")
    p.add_argument("room", help="hex room id as the game shows it, e.g. 112 = WARDROBE")
    p.add_argument("camera", nargs="?", type=int,
                   help="camera, 1-based as the tour band counts them (CAM 2/2 = 2)")
    p.add_argument("--auto", action="store_true", help="auto tour from that room")
    p.add_argument("--http", type=int, default=8080, help="the bridge's port")
    p.set_defaults(fn=cmd_room)

    p = sub.add_parser("cmd", help="send a text command: room <hex> [camera] | cam <n> | auto [hex] | stop | "
                         "save <n> | load <n> | saves | sdtest | tone")
    p.add_argument("text", nargs="+")
    p.add_argument("--http", type=int, default=8080, help="the bridge's port")
    p.set_defaults(fn=cmd_cmd)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
