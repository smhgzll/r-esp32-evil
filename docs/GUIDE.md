# r-esp32-evil — Build Guide

Resident Evil (PC, 1997) running on an ESP32-S3 with a 1.8" SPI TFT, a micro-SD
card, a PCM5102A DAC and analog joysticks — built on the
[Resident Evil PC decompilation](https://github.com/ecruells/resident-evil-pc-decomp).

This guide covers everything: parts and wiring, toolchain, preparing the SD card
from your own copy of the game, the PC host simulator, QEMU, building and
flashing the board, and how the port works.

> **No game data is included or downloaded.** You need your own copy of
> Resident Evil for PC (the GOG release, or the 1997 retail USA version). The
> tools convert *your* files.

---

## Contents

1. [Status](#1-status)
2. [How it works (short version)](#2-how-it-works-short-version)
3. [Parts](#3-parts)
4. [Wiring](#4-wiring)
5. [Software setup](#5-software-setup)
6. [Getting the code](#6-getting-the-code)
7. [Preparing the game data](#7-preparing-the-game-data)
8. [Writing the SD card](#8-writing-the-sd-card)
9. [Running on the PC: host simulator](#9-running-on-the-pc-host-simulator)
10. [Running in QEMU](#10-running-in-qemu)
11. [Building and flashing the board](#11-building-and-flashing-the-board)
12. [Controls](#12-controls)
13. [Configuration reference](#13-configuration-reference)
14. [Architecture in detail](#14-architecture-in-detail)
15. [Troubleshooting](#15-troubleshooting)
16. [Limitations and next steps](#16-limitations-and-next-steps)
17. [Licence](#17-licence)

---

## 1. Status

| Part | State |
|---|---|
| Game logic (decomp, ~158 k lines) | Unchanged upstream code plus one macro-gated patch (`patches/`) |
| Real ESP32-S3 board | **Plays.** Movies, title, character select, the mansion and the lab; saving and loading on the SD card; Bluetooth Xbox controller |
| Speed on the board | 30 fps (the game's own tick rate) in most rooms; 25–30 fps in heavy scenes (several enemies close to the camera); menus up to 60 fps |
| Software renderer (`port/marni/MarniDX_SW.cpp`) | Sprites, textured/gouraud triangles, depth buffer, all four blend modes |
| Audio mixer with SD streaming | Runs on the board (the console reports the mix level); `cmd tone` checks the DAC |
| FMV (`.rv1` movies) | Board, host and QEMU |
| Browser view + keyboard (host, QEMU, board over UART) | Working |
| PC host simulator | Plays from the title screen through the game |
| QEMU (ESP32-S3 machine) | The real firmware boots, plays the movies and runs the mansion hall |

Memory: 5.09 MB of fixed game buffers in PSRAM and a heap peak of about
2.9 MB leave roughly 600 KB of the 8 MB PSRAM free. See
[§14.4](#144-memory-budget).

---

## 2. How it works (short version)

The decompilation is a complete, playable reconstruction of the PC game. It
already separates the game from the operating system: the Linux build swaps in
an SDL2 platform layer and an OpenGL renderer. This project adds **one more
platform** instead of rewriting the game:

```
decomp/src/game, marni, system    the game, unchanged (+ patches/*.patch)
          │   platform.h · MarniDX · DirectSound · CMarniDirectInput
port/                             this project: shared by every target
  marni/MarniDX_SW.cpp            software rasteriser (replaces OpenGL)
  common/Audio.cpp                mixer + SD streaming (replaces SDL audio)
  common/Video.cpp                .rv1 movie player (replaces ffmpeg)
  common/Platform.cpp, Input.cpp  files, keys, pad
  common/Remote.cpp               browser link (frames out, keys in)
          │   re1_target.h
firmware/components/re1_board     ESP32-S3: ST7735, SD, I2S, ADC/GPIO, FreeRTOS
port/host                         PC host simulator (same code, native binary)
```

So no 3D engine from Quake or demos was needed: the game already contains its
own PS1-style 3D pipeline (`TmdRenderer`, `GteMatrix`); it only needed a new
backend that draws triangles without a GPU.

---

## 3. Parts

| Part | Notes |
|---|---|
| ESP32-S3 N16R8 dev board (16 MB flash, 8 MB **octal** PSRAM, two USB-C) | The "DevKitC-1 clone" with ports labelled **COM** (USB-UART bridge, UART0) and **USB** (native USB). Both can flash. |
| 1.8" SPI TFT, 128×160, ST7735(S), "V1.1" | Pins `LED SCK SDA A0 RESET CS GND VCC` plus a separate SD slot header (not used by default, see below) |
| Micro-SD card adapter with 74VHC125 buffer | Pins `CS SCK MOSI MISO VCC GND`. Usually has a 3.3 V regulator: feed it **5 V**. |
| PCM5102A I2S DAC module | Pins `VIN GND LCK DIN BCK SCK`, 3.5 mm out |
| 1 or 2 analog joystick modules (KY-023 style) | `GND +5V VRx VRy SW` — power them from **3.3 V** |
| 4–5 push buttons | Action, Run/Cancel, Aim, Inventory, (Options) |
| Micro-SD card | ≥ 1 GB, FAT32. The prepared tree is ~650 MB. |
| Wires, breadboard | Keep the SPI wires short (< 15 cm), especially for the SD card |

**The display's own SD slot**: the large SD slot on the TFT module has its own
pins (`SD_CS`, `SD_MOSI`, `SD_MISO`, `SD_SCK`). You can use it *instead of* the
separate micro-SD adapter: wire those four pins to the SD pins below. Do not use
both.

---

## 4. Wiring

All pins can be changed with `idf.py menuconfig` → **r-esp32-evil** (see
[§13.2](#132-firmware-options-menuconfig)). The defaults avoid the pins an
N16R8 module reserves: GPIO 26–32 (flash), 33–37 (octal PSRAM), 19/20 (USB),
43/44 (UART0 console), 0/45/46 (boot straps) and 48 (RGB LED on many clones).

### 4.1 Display — ST7735 on SPI2

| TFT pin | ESP32-S3 | Notes |
|---|---|---|
| VCC | 3V3 | (or 5V if your module has a regulator and the J1 jumper open) |
| GND | GND | |
| SCK | GPIO 12 | SPI clock |
| SDA | GPIO 11 | SPI MOSI |
| A0 | GPIO 9 | data/command (DC) |
| RESET | GPIO 14 | |
| CS | GPIO 10 | |
| LED | GPIO 21 | backlight; or tie to 3V3 and set *LED pin* to `-1` |

GPIO 10–12 are the SPI2 IO_MUX pins, which allow the highest clock.

### 4.2 SD card — SPI3

| SD adapter | ESP32-S3 |
|---|---|
| VCC | **5V** (the adapter's regulator makes 3.3 V) |
| GND | GND |
| CS | GPIO 39 |
| SCK | GPIO 40 |
| MOSI | GPIO 41 |
| MISO | GPIO 42 |

GPIO 39–42 double as JTAG pins; that is fine because the board is debugged over
USB, not external JTAG.

### 4.3 Audio — PCM5102A on I2S0

| PCM5102A | ESP32-S3 |
|---|---|
| VIN | 3V3 (or 5V, the module has a regulator) |
| GND | GND |
| BCK | GPIO 15 |
| LCK (WS) | GPIO 16 |
| DIN | GPIO 17 |
| SCK | **GND** — the DAC then makes its own clock from BCK |

On the back of most PCM5102A modules there are four solder jumpers. Set:
`H1L` (FLT) → L, `H2L` (DEMP) → L, `H3L` (XSMT) → **H** (un-mute), `H4L`
(FMT) → L (I2S). Many modules ship with XSMT floating: if there is no sound,
check that one first.

### 4.4 Controls

| Control | ESP32-S3 | Function |
|---|---|---|
| Joystick 1 VRx | GPIO 1 (ADC1) | left/right |
| Joystick 1 VRy | GPIO 2 (ADC1) | forward/back |
| Joystick 1 SW | GPIO 47 | inventory |
| Joystick 1 +5V / GND | **3V3** / GND | |
| Button ACTION | GPIO 6 → GND | action / confirm |
| Button RUN | GPIO 7 → GND | run / cancel |
| Button AIM | GPIO 8 → GND | aim (hold) |
| Button INVENTORY | GPIO 18 → GND | status screen |
| Button OPTIONS | GPIO 3 → GND | options |
| Joystick 2 VRx / VRy (optional) | GPIO 4 / GPIO 5 (ADC1) | see [§12](#12-controls) |

Buttons go between the GPIO and GND; internal pull-ups are enabled. Analog axes
must be ADC1 pins (GPIO 1–10). **Keep the sticks centred while the board
boots**: the resting position is measured at start-up.

GPIO 47 is a 3.3 V pin on N16R8 modules (VDD_SPI 3.3 V). On 1.8 V-flash modules
(e.g. N32R8V) move it.

### 4.5 Power

Power the board from either USB-C port. Total draw is roughly 150–300 mA
(backlight and SD card writes are the peaks). The 5V pin of the dev board is
the USB 5 V and can feed the SD adapter.

---

## 5. Software setup

Tested on Linux (Fedora, x86-64). macOS and WSL2 should work for
everything except the 32-bit host simulator on macOS.

### 5.1 Packages

```bash
# Fedora
sudo dnf install git cmake ninja-build gcc-c++ glibc-devel.i686 libstdc++-devel.i686 \
                 ffmpeg python3-pillow python3-numpy python3-pyserial dosfstools
# Debian / Ubuntu
sudo apt install git cmake ninja-build g++ g++-multilib ffmpeg \
                 python3-pil python3-numpy python3-serial dosfstools
```

`g++-multilib` / `glibc-devel.i686` are only for the host simulator (it is a
32-bit binary like the decomp's own Linux build). `ffmpeg`, Pillow and numpy are
for converting the game data.

### 5.2 ESP-IDF 5.5 and QEMU

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.6 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git esp-idf-v5.5.6
cd esp-idf-v5.5.6
./install.sh esp32s3
python tools/idf_tools.py install qemu-xtensa      # only for the QEMU target
```

`re1tool.py` finds ESP-IDF under `~/esp/esp-idf*` or `$IDF_PATH` and sources
`export.sh` itself, so you do **not** need to run `. export.sh` in your shell.
In fact, run `re1tool.py` from a *plain* shell: `export.sh` puts ESP-IDF's own
Python first on `PATH`, and that Python does not have Pillow/numpy.

---

## 6. Getting the code

```bash
git clone https://github.com/smhgzll/r-esp32-evil.git
cd r-esp32-evil
python3 tools/re1tool.py fetch
```

`fetch` clones the decompilation into `decomp/` at the commit pinned in
`patches/DECOMP_COMMIT` and applies `patches/*.patch`. Re-running it resets
`decomp/` to that state, so do not edit files there — change the patch instead.

The patch touches 29 files, and every change is behind a macro the upstream
builds never define (`RE1_SMALL_MEM`, `RE1_BG_ON_DEMAND`, `RE1_PORT_FAST`,
`RE1_PORT_BENCH`, `RE1_PORT_EXTRAS`, `RE1_PORT_TASKSWITCH_EXTERNAL`, ...), so
with none of them set the decomp builds exactly as upstream. The patch header
explains each macro.

---

## 7. Preparing the game data

```bash
python3 tools/re1tool.py prepare \
    --game "/path/to/GOG Games/Resident Evil" \
    --out  ~/re1-sd
```

`--game` may point at the install folder, its `english/` folder, or the `USA/`
folder itself. The result is `~/re1-sd/re1/`:

```
re1/
  config.ini          board defaults (HD textures off, fixed 320x240 layout)
  save/               save games are written here
  usa/                the game tree, every name lower-case
    data/ enemy/ players/ stage1..7/ objspr/ effspr/ item_m1/ item_m2/ sound/ voice/
    movie/*.rv1       the movies, re-encoded for the panel
```

What `prepare` does to the files:

| Data | Conversion | Why |
|---|---|---|
| Every file and folder name | lower-cased | FAT does not care; QEMU's host-directory access is case-sensitive |
| `Movie/*.avi` (Cinepak 320×240, 10 fps) | → `.rv1`: 160×120, one 256-colour palette per frame, 22 kHz mono PCM track | The board blits frames straight from the card; no video decoder needed |
| Sound banks small enough to stay in RAM (≤ 128 KB as 16-bit) | → 8-bit PCM, dithered | Halves the RAM they take (≈ 0.5 MB saved) |
| Larger WAVs (music, long voice clips) | copied as-is (16-bit) | The board streams them from the card |
| Backgrounds (`.pak`), textures (`.tim`), models, rooms | copied as-is | The game parses them byte-for-byte; the renderer down-samples textures when it uploads them |

Options:

| Option | Effect |
|---|---|
| `--video-codec rgb565` | full-colour movie frames (38 KB/frame instead of ~20 KB) |
| `--audio keep` | do not convert any sound bank |
| `--no-movies` | skip the (slow, ~1–3 min) movie conversion |
| `--skippable-fmv` | sets `SkipUnskippableFmv=1`: every movie can be skipped |
| `--force` | rebuild everything (otherwise only changed files are redone) |
| `-j N` | parallel movie conversions |

Check a tree with `python3 tools/re1tool.py check ~/re1-sd`.

---

## 8. Writing the SD card

1. Format the card **FAT32**. Either with your desktop tools, or:
   ```bash
   lsblk                                  # find the card, e.g. /dev/sdb1 - be sure!
   python3 tools/re1tool.py sdcard --from ~/re1-sd --format /dev/sdb1
   ```
   `--format` asks you to type the device name again, then runs
   `sudo mkfs.vfat -F 32`.
2. Mount the card (most desktops do it automatically) and copy the tree:
   ```bash
   python3 tools/re1tool.py sdcard --from ~/re1-sd --mount /run/media/$USER/RE1
   ```
   Only missing or changed files are copied, so updating a card is quick.
3. Unmount before removing it.

The card must end up with `re1/usa/...` and `re1/config.ini` at its root; the
firmware mounts the card at `/sd` and uses `/sd/re1` as the game folder.

---

## 9. Running on the PC: host simulator

The host simulator is the board's port layer compiled as a native program:
the same software renderer at 160×120, the same audio mixer (it streams the
mix to the browser), the same `.rv1` movies, the same memory settings. It is
the fastest way to work on the port.

```bash
python3 tools/re1tool.py host --data ~/re1-sd
```

This builds `build/host/re1_host`, starts it, starts the bridge and opens
`http://127.0.0.1:8080/`. Click the page once so the browser allows sound.

`--data` can also point straight at an original install (case is resolved
automatically). For headless runs:

```bash
build/host/re1_host --data ~/re1-sd/re1 --no-port --ticks 900 \
    --press 67 200 4 --capture 600 frame.ppm      # 67 = 'C' (action)
build/host/re1_host --data ~/re1-sd/re1 --no-port --ticks 900 \
    --cmd 40 "room 112 2" --capture 880 room.ppm  # a bridge command at tick 40
```

Captures at a fixed tick are the same from run to run (movie frames aside:
movies play by the clock), which is how renderer changes are checked: capture
the same ticks before and after and compare the files byte for byte. A build
without `RE1_PORT_FAST` gives the plain code paths to compare against:

```bash
cmake -S port/host -B build/hostslow -DRE1_HOST_FAST=OFF && cmake --build build/hostslow -j
```

A heap tracer build shows where memory goes:

```bash
cmake -S port/host -B build/hosttrace -DRE1_HOST_HEAPTRACE=ON && cmake --build build/hosttrace -j
RE1_HEAPTRACE=1 build/hosttrace/re1_host --data ~/re1-sd/re1 --no-port --ticks 1500 ...
```

---

## 10. Running in QEMU

QEMU runs the **real firmware** (same binary as the board except for the
target profile). Espressif's QEMU has no ST7735, SD card, I2S or ADC, so the
QEMU profile:

- reads the prepared tree from a host directory through *semihosting*,
- sends the picture over UART0 to the bridge, and takes keyboard input back
  the same way (exactly the "flag-controlled browser screen" mode of the board),
- has no sound (banks load, nothing plays; movies use the wall clock).

```bash
python3 tools/re1tool.py qemu --data ~/re1-sd
```

The first run builds the QEMU profile into `firmware/build-qemu/` (a few
minutes), then starts QEMU and the bridge and opens the browser. The console
panel on the page shows the ESP-IDF log.

Differences from the board profile (`firmware/sdkconfig.qemu`): quad PSRAM
(QEMU does not emulate octal), task watchdog off, 115200-baud console (baud
does not matter in QEMU), and the UART link uses the console VFS because
QEMU's UART interrupt emulation loops forever once the driver's ISR is on.

---

## 11. Building and flashing the board

### 11.1 Configure the pins (once)

```bash
cd firmware
. ~/esp/esp-idf-v5.5.6/export.sh
idf.py set-target esp32s3
idf.py menuconfig          # r-esp32-evil -> Display / SD card / Audio / Controls
```

The defaults match [§4](#4-wiring). `sdkconfig.defaults` sets up the N16R8:
240 MHz, 16 MB flash, octal PSRAM with the game's buffers in it, FAT long file
names, and the console on UART0 at **2 000 000 baud**.

### 11.2 Build and flash

```bash
python3 tools/re1tool.py build
python3 tools/re1tool.py flash -p /dev/ttyUSB0 --monitor
```

Use the port of the **COM** USB-C connector (`/dev/ttyUSB0` or `/dev/ttyACM0`
on Linux, `COMx` on Windows). If flashing does not start, hold **BOOT**, tap
**RST**, release BOOT. The monitor must run at 2 Mbaud:
`python3 tools/re1tool.py monitor -p /dev/ttyUSB0`.

### 11.3 Browser view on the real board

Enable *r-esp32-evil → Stream frames to the browser bridge over UART0*
(`CONFIG_RE1_REMOTE_LINK`), rebuild and flash, then:

```bash
python3 tools/re1tool.py web --serial /dev/ttyUSB0
```

The panel keeps working; the browser shows the same frames and its keyboard
drives the game. Frames are delta-coded, so a static camera costs almost
nothing, but a full-screen change is ~38 KB, i.e. about 5 frames/s at 2 Mbaud.
Turn it off for normal play.

### 11.4 First boot checklist

The log should show, in this order:

```
re1: r-esp32-evil (board) PSRAM free ... KB
st7735: panel up: 160x128 @ 27 MHz, picture 160x120
input: axis GPIO1 centre ...
storage: game data at /sd/re1
audio: I2S up: 22050 Hz stereo ...
[SW] software renderer 160x120 ...
[RE1] boot complete, heap free ...
```

then the Capcom logo movie.

---

## 12. Controls

| Game action | Board | Keyboard (browser) | Gamepad (browser or Bluetooth) |
|---|---|---|---|
| Move / turn | joystick 1 | arrow keys | left stick / D-pad |
| Action / confirm | ACTION | `C` or `Enter` | A |
| Run / cancel | RUN | `V` | B, LT |
| Aim (hold) | AIM | `X` | X, LB, RB, RT |
| Inventory | INVENTORY or stick 1 press | `Z` | Y, Start / Menu |
| Options | OPTIONS | `Esc` | Back / View |
| Port menu | OPTIONS + INVENTORY | `M` | Xbox button (Bluetooth) |

RE1 uses "tank" controls: up walks forward, left/right turn, down walks back;
run = hold RUN while walking; shoot = hold AIM, press ACTION.

**Second joystick** (`CONFIG_RE1_STICK2_ENABLE`), two modes:
- *Face buttons* (default): up = aim, down = run/cancel, left = inventory,
  right = action. With this you can play with two sticks and no buttons.
- *Second D-pad*: duplicates joystick 1 (left- or right-handed play).

The browser page also has an on-screen pad for touch screens.

**Bluetooth controller** (`components/re1_board/gamepad_ble.cpp`): an Xbox
Wireless Controller - Xbox One S model 1708 or Series X|S - with firmware 5.x,
which speaks Bluetooth LE, or another BLE HID gamepad. The ESP32-S3 has no
classic Bluetooth: an Xbox One controller on older firmware (the USB descriptor
says 3.x) must be updated first with the Xbox Accessories app on Windows or on
an Xbox. Pairing: switch the controller on, hold its pair button (top, by the
USB port) until the Xbox logo flashes fast. The board looks for a controller
whenever none is connected, bonds with the first gamepad it hears and takes it
back from then on when it is switched on; the console says what happens
(`pad:` lines). Bluetooth starts once the renderer has its buffers
(`re1t_renderer_ready`): its controller and tasks take ~45 KB of internal RAM,
and started first they pushed the depth buffer to PSRAM.

**Port menu** - opens anywhere (title, movies, the game's menus, play) and
freezes the game while it is up:

| Open / close | Browser `M` or the **Menu** button; board or gamepad OPTIONS + INVENTORY together |
|---|---|
| Move / change | up/down, left/right |
| Select | ACTION (`C` / `Enter`) |
| Close | RUN or OPTIONS (`V` / `Esc`), or `M` again |

Rows: FPS counter, TEXT SIZE, MENU TEXT, INVINCIBLE, INVISIBLE, ITEM (left/right
picks, ACTION adds it), FULL HEALTH, ROOM TOUR, AUTO TOUR (see BENCHMARK
below), CLOSE. The cheat and item rows need a game in progress and are dimmed
elsewhere; the tours work from the title too, where they start a new game as
Chris (no intro movie) and begin in its first room. The game sees no input until every key and button used in
the menu is released.

**OPTIONS > EXTRAS** (in game, port-added):

| Row | Effect |
|---|---|
| TEXT SIZE / MENU TEXT SIZE | ORIGINAL / LARGE / X-LARGE, applied at once and saved to `[Port]` in `config.ini` |
| INVINCIBLE | health is refilled every frame (instant-kill attacks still kill) |
| INVISIBLE | the enemies' shared sight and range checks never see you; one that is already attacking may still hit |
| SHOW FPS | the frame rate in the top-left corner (green 25+, yellow 15-24, red below 15); saved as `[Port] ShowFps` |
| GIVE ITEM | left/right picks a weapon, ammo, healing item or key; ACTION puts it in the first free slot |
| FULL HEALTH | restores health now |

The cheats last until the board is switched off.

**OPTIONS > BENCHMARK** (in game, port-added): a room tour to look for
stalls, slow loads and slow rooms on the target.

- *ROOM TOUR* starts in the room you are in; *AUTO TOUR* starts at the first
  room and visits every camera of every room (2 s each) on its own.
- During the tour: LEFT/RIGHT = previous/next room (through the door
  animation, like a real door), UP/DOWN = next/previous camera, ACTION = auto
  on/off, CANCEL = end (you are taken back to the room the tour started in).
- Room scripts and events are off, so no cutscene or movie starts; NPCs are
  not drawn; you are invisible and invincible and stand still. Enemies are
  there and move.
- The bottom band shows the room (`105 DINING ROOM`), the camera and the
  position in the list (160 rooms with the USA data).
- The console (browser bridge log or `re1tool.py monitor`) gets one line per
  room and per camera cut, and a summary when the tour ends:

```text
[BENCH] 29/160 202 DINING ROOM 2F: load 115 ms (with door 5006 ms), 5 cameras, heap 1210 KB free (largest 512 KB)
[BENCH]   camera 1: cut 32 ms
[BENCH]   202: 29.8 fps over 300 frames, worst frame 41 ms, 0 hitches, slowest camera cut 32 ms
[BENCH] ===== room tour finished: 160 rooms in 2410 s =====
```

Memory trouble shows up on the same console: `[MEM] big alloc ... FAILED`
(a background or mask could not be unpacked) and `[SW] out of memory ...`
(a texture could not be stored).

**Straight to a room** while a bridge runs (`re1tool.py web`, `qemu` or
`host`): `python3 tools/re1tool.py room 112` (or the *Go to room* box on the
browser page) puts the target on the room tour in that room - from the title
or a movie it skips the movies and starts a new game as Chris for it.
`re1tool.py room 112 2` also switches to the room's second camera, counted as
the band counts them (`CAM 2/2`); on that room already, only the camera
changes. `re1tool.py cmd cam 1` changes the camera alone,
`re1tool.py room 112 --auto` starts the auto tour there and
`re1tool.py cmd stop` ends a tour.

Other commands: `re1tool.py cmd save 1` / `cmd load 1` save the game in
progress to `savedat1.dat` (slots 1-8, the same file the typewriter writes) and
load it back - not during the room tour; `cmd tone` plays 2 s of 440 Hz on the
DAC and says whether the game's own sound was silent (see Troubleshooting).

**Where the time goes**: every 5 s the console prints the average frame,
split into exclusive buckets:

```text
[PERF] 152 frames, 433 tris/frame, 26.9 ms/frame: game 1.3 enemies 0.4 player 1.1 models 2.3 tmd 8.4 tmdasm 4.2 tmdsort 0.4 tmdsub 0.5 sprsort 1.6 raster 2.8 sprite 3.4 present 0.5
[WORK] per frame: obj 54 vtx 6390 prim 2130 offscr 653 tri 433 batch 164 tripx 232 spr 60 sprpx 30980 sprslow 0 flush 50
```

`tmd*` is the model pipeline (vertex transform, lighting and triangle set-up,
depth sort, hand-off), `raster` the triangle fill, `sprite` background, masks
and HUD, `models` posing the skeletons, `game` everything else in the tick.
The game ticks at 30 Hz, so 33 ms is the budget for a full frame rate.

`[WORK]` says how much each stage handled per frame, to read the buckets per
unit: model pieces (`obj`), their vertices and primitives, the primitives
dropped as wholly off the screen (`offscr`), triangles drawn and the pixels
they covered (`tri`, `tripx`), draw calls (`batch`), sprites and their pixels
(`spr`, `sprpx`; `sprslow` = pixels through the general texel/blend path)
and sprite-queue range flushes (`flush`). The example is room 112's second
camera, where most of the geometry is outside the picture: 1086 triangles
covered 232 pixels before off-screen primitives were dropped early.

At boot the board also logs how fast its memory is:

```text
I (749) re1: memory (data cache): PSRAM copy 30 MB/s, fill 49 MB/s, read 75 MB/s, cache miss 708 ns; internal copy 380 MB/s
```

A PSRAM access that misses the cache costs ~170 CPU cycles, and that, not
arithmetic, is what most of a frame waits on: the speed-ups so far
(`RE1_PORT_FAST`) mostly touch PSRAM less. The data cache's autoload
(prefetch after a miss) was tried and left off: +7 % sequential reads, but
twice the cost of a scattered miss.

The renderer divides with `re1_div` (`port/include/re1_fastmath.h`): libgcc's
own FPU division sequence written inline, so the same quotient without a call
into ROM per division. The game checks it at boot against the library:

```text
[RE1] re1_div self-test: 20256 quotients, all bit-identical (inline: 1)
```

To see where the rasteriser's time goes, build with `RE1_SW_RASTER_STATS=1`
(top of `port/marni/MarniDX_SW.cpp`): the `[WORK]` line then carries the
cycles spent reading triangles (`rcyc_vtx`), in the whole rasteriser
(`rcyc_tri`), in scanline set-up (`rcyc_line`) and in the pixel loops
(`rcyc_px`), and the scanline count (`lines`). In room 509 (several enemies
near the camera) that read ~410 cycles per triangle read, ~490 per triangle
set-up, ~330 per scanline and ~340 per pixel; triangles average 2 scanlines and
5 pixels there, and most of a pixel's cycles are a texel fetch missing the
cache.

Not enabled, on purpose: 120 MHz PSRAM (`CONFIG_SPIRAM_SPEED_120M`). It would
speed up everything that waits on PSRAM, but for octal PSRAM ESP-IDF marks it
experimental - accesses can fail once the chip is ~20 °C off the temperature it
booted at.

The tour moves the mansion's stage-variant flag and marks rooms visited, the
same as the decomp's debug ROOM CHANGE: load a save to play on afterwards.
In the host simulator, `RE1_BENCH_START=202` (a hex room id) starts the tour
at that room, or a command runs at a given tick, as the bridge would send it:
`build/host/re1_host --data ~/re1-sd/re1 --no-port --ticks 900 --cmd 40 "room 112 2" --capture 880 frame.ppm`.

---

## 13. Configuration reference

### 13.1 `config.ini` (on the card)

The same file the PC decomp uses. On the board the display section is ignored
(the panel is fixed) and HD textures are always off. Useful keys:

```ini
[Game]
Mode=OG                 ; OG = PC release; DC = Director's Cut overlay (needs DC/ data)
SkipUnskippableFmv=0    ; 1 = every movie can be skipped
[Debug]
EnableDebug=0           ; 1 = decomp debug menu on F1 (browser keyboard)
```

Settings of this port only (`[Port]`, kept when the game rewrites the file):

```ini
[Port]
TextScale=2.0       ; messages, item names, descriptions, file text (1.0 - 2.5)
MenuTextScale=2.0   ; the 8x8 menu font
ShowFps=1.00        ; frame rate in the top-left corner (OPTIONS > EXTRAS > SHOW FPS)
```

At 160x120 the game's glyphs would be 4x4 / 4x7 pixels. The renderer's text
layer draws everything that uses the game font bigger and lays it out again:
paragraphs are re-wrapped at word gaps so they still fit the screen, tables
grow only as far as their row spacing allows. 2.0 draws the font at its
native pixel size and is the sharpest; values between 1 and 2 work but are
less crisp. **Text that is part of a picture** - the prologue lines, the title
menu, the EQUIP/CHECK/COMBINE and MAP/FILE/EXIT labels, the original options
screens - is artwork, not font text, and keeps its size.

### 13.2 Firmware options (menuconfig)

| Option | Default | |
|---|---|---|
| Target | ESP32-S3 board | or QEMU |
| Stream frames to the browser bridge | off (on for QEMU) | |
| Game loop core / stack | 1 / 32 KB | internal RAM |
| Game task stacks | 24 KB each ×3 | PSRAM; the host measures ~5 KB high-water |
| LCD SPI clock | 27 MHz | lower it if the picture shows noise |
| LCD MADCTL | `0xA0` | landscape; `0x60` = rotated 180° |
| LCD BGR / invert / X,Y offset | BGR off, off, 0, 0 | for the different 1.8" panel variants |
| SD SPI clock | 10 MHz | 20 MHz with short wires |
| Audio volume | 70 % | |
| Joystick dead zone | 35 % | |

---

## 14. Architecture in detail

### 14.1 Rendering

The game lays itself out for a **320×240** back buffer (the smallest mode of
the PC release). `MarniDX_SW` reports that size and rasterises at **160×120**,
scaling every coordinate by ½. Every texture is box-filtered by ½ when it is
uploaded — colour-keyed art keeps a hard 0/255 alpha — so point sampling at
half resolution looks like a properly down-scaled 320×240 frame, at a quarter
of the pixels and texture memory.

- 2D: axis-aligned sprite fast path (16.16 fixed-point stepping), solid
  rectangles, lines.
- 3D: scanline triangles with plane-equation gradients, gouraud colour,
  perspective-correct UVs every 16 pixels (affine in between), 16-bit depth
  buffer with LEQUAL test/write, exactly the depth rules of the GL backend.
- Textures: RGB565, plus an alpha plane only for textures with partial alpha.
- Present: double-buffered; the panel task (core 0) byte-swaps the finished
  frame into two 5 KB DMA chunks while the game draws the next frame on core 1.
  The 160×120 picture is centred on the 160×128 panel.

The pre-rendered room backgrounds are the game's own `.pak` files, unpacked by
the game's LZW decoder on each camera cut (`RE1_BG_ON_DEMAND` reads one camera's
file instead of caching all of them).

### 14.2 The scheduler

The game runs its scripts as three cooperative tasks that switch stacks
(PSYQ `ChangeTh` on the PS1, naked assembly on the PC, `ucontext` on Linux).
On the ESP32-S3 each slot is a FreeRTOS task with its stack in PSRAM, and
control is handed back and forth with direct task notifications, so exactly
one of them runs at a time, on the game core (`taskswitch.cpp`). Restarting a
slot deletes its old task, which is what discarding a ucontext does.

### 14.3 Audio

`port/common/Audio.cpp` implements the game's `DirectSound` class: up to 80
banks, millibel volume, pan, loop/one-shot — the same contract as the decomp's
SDL backend — with an integer mixer. Banks with more than 64 KB of PCM stream
from the card: each keeps a 16 K-frame ring that a low-priority pump task fills,
so the mixer never waits for the SD card. Movie audio is a streamed source too,
and its play position is the movie clock. Output: 22 050 Hz stereo, I2S.

### 14.4 Memory budget

| Where | What | Size |
|---|---|---|
| PSRAM `.bss` | the game's fixed buffers (room data, TMD object pool, sound tables, …) | 5.09 MB |
| PSRAM heap | textures (~0.8 MB in a room), sound banks, model work buffers, file buffers | peak ≈ 2.5–2.9 MB |
| Internal SRAM | framebuffers + depth (111 KB), game-loop stack, DMA buffers (the SD card's 8 KB bounce buffer, the panel's), RTOS objects, the model pipeline's vertex scratch while it fits | ~200 KB |
| Caches | 32 KB instruction, 64 KB data with 64-byte lines (`sdkconfig.defaults`) | 96 KB of SRAM |
| IRAM | the per-vertex / per-pixel functions of the model pipeline and the rasteriser (marked `RE1_HOT_FN`) | ~13 KB |

The frame is bound by PSRAM traffic more than by arithmetic: the larger data
cache with 64-byte lines took room 112 from 37 to 32 ms per frame. With
Bluetooth on, about 30 KB of internal RAM stays free during play (Bluetooth
itself takes ~32 KB static and ~45 KB at run time).
`re1t_alloc_fast` (framebuffers, depth buffer, model scratch) takes internal
RAM only while the DMA pool (`SPIRAM_MALLOC_RESERVE_INTERNAL`, 48 KB) stays
free, and falls back to PSRAM after that; the SD card stages its transfers in
its own 8 KB buffer, so play never needs new DMA memory. The `[RE1]` line's
`internal=` is the free internal RAM. Keep the framebuffers and the depth
buffer internal: with the depth buffer in PSRAM room 112 takes 34 ms a frame
instead of 32.

Upstream the game wants ~12 MB of buffers; `RE1_SMALL_MEM` brings that down
without touching game behaviour: function-local scratch statics (effect-sprite
compositing, debug menu, Saturn logo, PS1 credits) become call-scoped heap
buffers, port-only headroom (+1 MB `g_DataBuffer` slack for Saturn models,
8192-triangle sort pool) is trimmed, the PAK LZW dictionary is packed and lives
only during an unpack, and the 203 KB `DirectSound` object becomes a 64-byte
block (only its flag word is ever read). `heaptrace.cpp` (host) is the tool for
measuring further changes.

### 14.5 Browser link (RVL)

Packets `A5 5A <type> <len32> <payload> 5A` over any byte stream; everything
outside a packet is console text. Frames are raw or delta-coded RGB565; input
is Win32 virtual-key codes (what the game's key bindings use) and a pad mask.
The format is specified in `port/include/re1_remote.h`; the bridge is
`tools/re1tool.py web` and the page is `tools/web/index.html`.

### 14.6 Files

```
patches/                    the decomp patch + pinned commit
port/include/               re1_target.h (target API), re1_remote.h, re1_rv1.h, ...
port/common/                platform layer shared by all targets
port/marni/                 the software renderer
port/host/                  host simulator (CMake, -m32)
port/sources.cmake          the translation-unit list both builds use
firmware/                   ESP-IDF project
  main/                     app_main + Kconfig (pins and options)
  components/re1/           the game library (decomp + port), .bss -> PSRAM
  components/re1_board/     ST7735, SD, I2S, input, UART link, semihosting, FreeRTOS switch
  sdkconfig.defaults        board profile;  sdkconfig.qemu = QEMU overlay
tools/re1tool.py            the tool;  tools/web/ = browser page
```

---

## 15. Troubleshooting

| Symptom | Check |
|---|---|
| `SD mount failed` | FAT32 (not exFAT); adapter VCC on **5 V**; wires short; try `SD SPI clock` 10000 kHz. The `card check:` lines after it read the card directly: the card's name and size (is it the card you prepared?), how much of it is erased. `64 erased (0xFF)` everywhere means an empty card *or* a card that cannot reach its flash: if the card reads fine in a PC but shows a different name/size here, its controller came up with its built-in defaults - check the adapter's supply (VCC on 5 V, not 3.3 V; VCC/GND wires seated). and what its first sector and partition hold (exFAT / NTFS / not a boot sector). The last line says whether a card still answers with CS released - a second card on the same wires, e.g. in the display module's own slot |
| `/sd/re1/usa not found` | the card root must contain `re1/` (copy the folder, not its contents) |
| Saves missing from the list, settings not kept, `SD write of sector N did not stick` | the card takes reads but drops writes - its supply. Flash programming draws more current than reading, and a card on a weak or missing 5 V (the SD module's regulator needs a real 5 V; fed only through the signal lines it limps along on reads) accepts every write and keeps none. Check the 5V pin under load (4.6-5.1 V): on dual-USB S3 boards it may be dead from one port or need the `IN-OUT` solder jumper. `re1tool.py cmd sdtest` writes and reads back test files; `cmd saves` lists the save folder. The board reads back every sector it writes, so a dropped write fails loudly instead of losing a save |
| `allocate_dma_buf: not enough mem`, `SD read ... failed after 6 tries (ESP_ERR_NO_MEM)` | internal RAM ran out (not the card): watch `internal=` in the `[RE1]` line; the SD driver has its own 8 KB buffer and `re1t_alloc_fast` keeps the DMA pool free, so this means something new takes internal RAM |
| Patches of the wrong picture on backgrounds, `SD read ... recovered after N retries` / `failed after 6 tries` | SD transfers time out now and then; failed sectors are retried, but frequent ones mean wiring or power: short wires, adapter on 5 V, a lower `SD SPI clock` |
| White or garbled panel | LCD SPI clock lower (e.g. 16 MHz); check A0/RESET wiring |
| Colours red/blue swapped | toggle *Panel is BGR* |
| Picture shifted by 1–2 pixels / garbage line at an edge | set *Column/Row offset* (1 or 2) |
| Mirrored or upside-down | MADCTL `0x60`, `0xE0` or `0x20` |
| No sound | `re1tool.py cmd tone`: a beep means the wiring is fine; its console line also says whether the game's mix was silent. Otherwise PCM5102A SCK to GND, XSMT high (H3L), FMT low; check BCK/LCK/DIN |
| Character walks by itself | sticks were not centred at boot; or raise the dead zone |
| Up/down reversed | *Invert joystick 1 Y* |
| `FATAL: ... PSRAM exhausted` | please report the room; the host heap tracer reproduces it |
| Monitor shows garbage | the console is 2 000 000 baud |
| QEMU shows nothing | it waits for the bridge to connect; use `re1tool.py qemu` |
| `ModuleNotFoundError: PIL` | run `re1tool.py` from a shell where ESP-IDF's `export.sh` was **not** sourced |

---

## 16. Limitations and next steps

- **Speed.** Busy scenes (several enemies close to the camera, e.g. room 509)
  drop to ~25 fps, and the save screen runs at ~9 fps. The frame waits on PSRAM
  more than on arithmetic. Ideas, in order of expected payoff: run the model
  vertex transform on core 0 while core 1 rasterises, store palette textures in
  cache-friendly tiles, and 120 MHz PSRAM (experimental for octal PSRAM, see
  [§12](#12-controls) "Not enabled, on purpose").
- **Audio.** 8-bit sound banks are slightly noisier than the originals; an
  IMA-ADPCM path in the mixer would keep 16-bit quality at a quarter of the size.
- **USB link.** The browser link uses UART0; the native USB-Serial-JTAG port
  would be faster but needs the console redirected to stay packet-safe.
- **Director's Cut / Saturn content** compiles in but is untested on the board,
  and its larger models need more memory than `RE1_SMALL_MEM` leaves.

---

## 17. Licence

Copyright (C) 2026 [smhgzll](https://github.com/smhgzll).

The decompilation is GPL-3.0, and this port links against it, so the port is
GPL-3.0 too (see `LICENSE`). Resident Evil is © CAPCOM. This project contains no
game data and is not affiliated with CAPCOM.
