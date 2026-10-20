# ESP32 port

Port of the Go daemon ([`reference/go-daemon`](../reference/go-daemon)) to
firmware that runs on its own: the ESP32 joins Wi-Fi, keeps the BLE
connection to the panel and serves the same HTTP API. The Go code stays as
the reference; the protocol is in [xyaoled/README.md](../reference/go-daemon/xyaoled/README.md).

Every feature of the Go daemon is listed below. Tick an item when it works
on the board, not when it compiles.

## Target

| | |
|---|---|
| Board | ESP32-C3 Zero / Pro Mini |
| SoC | ESP32-C3FH4: 1 × RISC-V core at 160 MHz, 4 MB flash in package, no PSRAM |
| RAM | 400 KB SRAM, of which about 320 KB is heap-usable DRAM; Wi-Fi + BLE together take roughly 100 KB of it |
| Radio | 2.4 GHz Wi-Fi and BLE 5 share one radio (time-sliced coexistence) |
| USB | Built-in USB Serial/JTAG (`303a:1001`), shows up as `/dev/ttyACM0` |
| SDK | ESP-IDF v6.0.3, installed in `~/esp/esp-idf-v6.0.3` |

### Language: C

The firmware is written in C (C17) against ESP-IDF.

- ESP-IDF, FreeRTOS, NimBLE, `esp_http_server`, NVS, LittleFS and cJSON
  are all C APIs; C uses them without wrappers.
- Nothing in the daemon needs C++: the Go goroutines and channels map
  onto FreeRTOS tasks and queues, and `sync.Mutex` onto semaphores.
- C makes every allocation visible, which matters with ~200 KB of heap
  and no PSRAM.
- C is not faster than C++ compiled the same way (no exceptions, no RTTI);
  the gain is control over memory and a smaller binary. If a C++-only
  library ever becomes necessary, ESP-IDF builds `.cpp` files in the same
  component, so the choice isn't a trap.

### Memory budget

The Go daemon reads whole uploads into memory (up to 32 MB) and decodes
every GIF frame at source resolution. That cannot work here, so the
image pipeline changes shape:

- **Scale while decoding.** The Go resizer is nearest neighbour, and each
  output pixel depends on exactly one source pixel. So compositing GIF
  frames (disposal methods included) on a 96×16 canvas, sampling only the
  source pixels that land on it, gives the same result as compositing at
  full size and scaling afterwards. The canvas is 96×16×3 = 4.6 KB
  whatever the source size.
- **Decode while receiving.** GIF LZW decodes sequentially, so request
  bodies are decoded in 4 KB blocks as they arrive and the source file is
  never stored. Only the scaled frames go to flash, in a LittleFS scratch
  partition. (PNG and JPEG, single frames, are decoded the same way.)
- **Two passes for animations.** The shared palette needs every frame
  before encoding: pass 1 writes scaled RGB frames to flash and builds
  the palette, pass 2 dithers and LZW-encodes them into the output GIF
  (also in flash), which is then sent in 10000-byte packets.
- **Limits.** The source file size does not matter; the frame count does.
  A 96×16 RGB frame is 4.6 KB, so the 2.4 MB scratch partition holds
  about 450 frames plus the output GIF. Passthrough GIFs (section 4) are
  stored as received.
- **Live stream frames stay in RAM** (64 KB limit): writing them to flash
  at 4 fps would wear it out for no gain.

### Flash layout (`firmware/partitions.csv`)

No OTA: the firmware is flashed over USB, so one app slot is enough and the
rest goes to the scratch partition.

| Partition | Offset | Size | |
|---|---|---|---|
| bootloader, partition table | `0x0` | 36 KB | |
| `nvs` | `0x9000` | 24 KB | settings, brightness, Wi-Fi credentials |
| `phy_init` | `0xf000` | 4 KB | radio calibration |
| `factory` | `0x10000` | 1.5 MB | the app |
| `scratch` | `0x190000` | 2.4 MB | LittleFS: frames and GIFs being processed |

Changing this table later works over USB but wipes NVS.

## Layout

```
firmware/              ESP-IDF project (idf.py build)
  main/                app entry, config, HTTP handlers
  components/xyaoled/  BLE panel driver (protocol, uploads)    [planned]
  components/imaging/  decoders, scaler, palette, GIF encoder  [planned]
docs/                  this file
reference/go-daemon/   original Go daemon (Linux/BlueZ), git submodule
```

## Setup

The board needs no driver on Linux: it is a USB CDC-ACM device and the
`cdc_acm` kernel module binds it as `/dev/ttyACM0`. KDE shows no
notification for it because it is not storage or media.

The port belongs to group `uucp`, so add yourself once and log in again:

```bash
sudo usermod -aG uucp $USER
```

Toolchain (already done on this machine):

```bash
git clone --depth 1 --branch v6.0.3 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.3
~/esp/esp-idf-v6.0.3/install.sh esp32c3
```

Build and flash (fish: `. ~/esp/esp-idf-v6.0.3/export.fish`):

```bash
. ~/esp/esp-idf-v6.0.3/export.sh
cd firmware
idf.py set-target esp32c3   # once
idf.py build flash monitor  # -p /dev/ttyACM0 if it is not found
```

If flashing fails to connect, hold BOOT, tap RESET, release BOOT.

## Checklist

### 0. Groundwork

- [x] Go daemon in `reference/go-daemon` as a git submodule of `fluoroom/xyaoled-daemon`
- [x] Protocol doc read from the submodule (`reference/go-daemon/xyaoled/README.md`), not copied
- [x] Language decided: C
- [x] ESP-IDF v6.0.3 installed for `esp32c3`
- [x] Board detected (`303a:1001` on `/dev/ttyACM0`)
- [x] Firmware skeleton builds
- [ ] Skeleton flashed and boots on the board
- [x] Partition table: one 1.5 MB app (no OTA) + NVS + 2.4 MB LittleFS scratch (builds)
- [ ] LittleFS component mounted on `scratch`

### 1. Configuration (Go: `config.go`, `.env`)

The `.env` file has no counterpart; settings come from Kconfig defaults
(`idf.py menuconfig`) and are stored in NVS when changed from the `/config`
page (section 5).

- [ ] Wi-Fi SSID and password (new: the Go daemon used the host's network)
- [ ] `LED_DISPLAY_WIDTH` / `LED_DISPLAY_HEIGHT` (96 / 16)
- [ ] `HTTP_PORT` (11001); `HTTP_HOST` does not apply
- [ ] `UPSIDE_DOWN`
- [ ] `DEBUG_BLE` (log every write and ack)
- [ ] `STREAM_FPS` (default 4, capped at 5)
- [ ] `STREAM_RESUME_DELAY` (default 1.5 s)
- [ ] `LED_PANEL_MAC`: the Go daemon reads it but never uses it (it connects
      to the first device whose name contains `XYAO`). Decide whether to
      keep name matching or connect by MAC.
- [ ] Startup log of the configuration
- [ ] New: mDNS name (`xyaoled.local`) so the IP need not be known
- [ ] New: GIF passthrough on/off (default on, see section 4)

### 2. BLE driver (Go: `xyaoled/device.go`, `xyaoled/upload.go`)

- [ ] NimBLE central: scan, match name containing `XYAO` (case-insensitive), 30 s timeout
- [ ] Connect, discover service `ae00`, write char `ae01`, notify char `ae02`
- [ ] Negotiate MTU 517, 512-byte writes; split frames longer than one write
- [ ] One TX lock for everything written (a command mid-upload corrupts it)
- [ ] Notification queue; drain before commands that expect no ack
- [ ] Sequence number per command
- [ ] Handshake: replayed `00 00` frame, power off, power on, `0a` resume, `05` clear (300 ms init delay, 30 ms between commands)
- [ ] Parse handshake reply (`88 ff 00`: width, height, brightness) and rejection (`88 ff 0f`)
- [ ] Notification checksum
- [ ] Brightness command `12 00` (level, level × 0.8)
- [ ] Power command `11 00` on/off
- [ ] Clear playlist `05 00` (no ack; 500 ms settle delay)
- [ ] Resume playlist `0a 00`
- [ ] Raw command (body after the length field)
- [ ] Program select `04` with ack index 0, retry every 1 s, panel-busy timeout 60 s
- [ ] Upload packets `07`/`08`/`09`: ≤ 10000 bytes, uint24 remaining, packet count/index
- [ ] Animation trailer (mode, total ms % 1000, frame count, total s); static mode `01`
- [ ] Ack parsing (`88 ff 03`): index, duplicate counter, status ≥ `0x14`
- [ ] Resend without ack after 200 ms per 250 bytes + 200 ms, up to 3 times
- [ ] `88 ff 05` instead of an ack: not in receive mode, select again and restart
- [ ] Wait for `88 ff 05` (stored) after playlist uploads, 5 s timeout
- [ ] Whole-send retry: 3 attempts, 50 ms × attempt backoff, no retry on connection errors
- [ ] Program 2 (playlist): replace (clear first) or append
- [ ] Program 3 (live): no clear, no stored-wait
- [ ] Ping: detect a dead link without writing to `ae01`
- [ ] Port the Go unit tests (`upload_test.go`) as host tests of the packet builder

### 3. Display logic (Go: `display.go`)

- [ ] Connection monitor every 5 s (link check)
- [ ] Reconnect with exponential backoff 5 s → 60 s; restore brightness after it
- [ ] Reconnect triggered by a send that fails on a dead connection
- [ ] Brightness persisted (Go: `.brightness` file; here NVS) and restored at boot
- [ ] Screen "off" = brightness ≤ 5: image sends are refused while off
- [ ] Manual off flag: brightness changes refused until `/on`
- [ ] Remember the level before `/off` for `/on`
- [ ] Streaming flag: playlist sends refused (409) while `/stream` holds the panel
- [ ] Clear screen: black live frame, playlist kept
- [ ] Connect / disconnect on demand (`/uploadTest`)

### 4. Image pipeline (Go: `display.go`)

- [ ] Upload bodies decoded in blocks as they arrive (source never stored in full)
- [ ] Scaled frames written to the scratch partition; scratch cleaned after each send
- [ ] Clear error when an animation exceeds the scratch space (frame count limit)
- [ ] Format detection (PNG / JPEG / GIF, single or multi-frame)
- [ ] PNG decoder, streaming
- [ ] JPEG decoder (`esp_jpeg` / TJpgDec, with its 1/2–1/8 pre-scaling)
- [ ] GIF decoder: all frames, delays, disposal (none / background / previous), transparency
- [ ] Contain-fit nearest-neighbour scaling to 96×16, centred on black, while decoding
- [ ] Rotate 180° (`UPSIDE_DOWN`)
- [ ] Composite onto opaque black
- [ ] Shared palette: first 256 distinct colours across all frames
- [ ] Floyd–Steinberg dithering against it
- [ ] GIF encoder: one global colour table, LZW, loop forever, disposal background per frame
- [ ] Zero frame delay → 50 ms
- [ ] Static image → one-frame GIF with a 600 s delay
- [ ] New: GIF passthrough. A GIF whose logical screen already matches the
      configured panel size is sent from flash as-is, skipping decode and
      re-encode; only the animation trailer is computed, from one scan of
      its blocks (frame count, total delay). Falls back to the full pipeline
      when any of these fail:
  - [ ] width × height equals `LED_DISPLAY_WIDTH` × `LED_DISPLAY_HEIGHT`
  - [ ] `UPSIDE_DOWN` is off
  - [ ] no frame has a local colour table (the panel's decoder loses sync on them)
  - [ ] no frame has a zero delay (the full pipeline turns those into 50 ms)
  - [ ] passthrough is enabled in the config
  - [ ] log which path an upload took, and why passthrough was refused
- [ ] Port the Go image tests (`display_test.go`) as host tests

### 5. HTTP API (Go: `handler.go`, `main.go`)

- [ ] `esp_http_server` on port 11001
- [ ] `GET /health`: status and config (width, height, stream fps, resume delay); add free heap
- [ ] `GET /test`: test page, embedded gzipped in flash
- [ ] `POST /display`, `/display-r`: JSON `{image: base64, type}`, type detected when empty
- [ ] `POST /upload`: multipart `file`; `GET` redirects to `/test`
- [ ] `POST /playlist`: repeated multipart `file`, replaces the playlist
- [ ] `POST /live`: multipart `file` or raw body, shown without touching the playlist; replies `ms`
- [ ] `GET /clear-screen`
- [ ] `GET /resume` (409 while streaming)
- [ ] `GET /clear` (409 while streaming)
- [ ] `POST /uploadTest`: `connect`, `disconnect`, `send_animated`, `send_static`, `screen_on`, `screen_off`, `brightness_cycle`, `raw`; legacy names `send_*_reinit`, `send_*_no_reinit`, `send_other_static_no_reinit`, `send_playlist`; `GET` redirects to `/test`
- [ ] `POST /uploadTestGif`: multipart `gif`; `GET` redirects to `/test`
- [ ] `GET /brightness/{n}`: clamped 5–125, 409 when manually off
- [ ] `GET /off`, `/on`
- [ ] `GET /power/on`, `/power/off`
- [ ] `GET /reconnect`: 202 `{"status":"reconnecting"}`
- [ ] Error mapping: 405 wrong method, 400 bad input, 409 streaming, 500 send failure, 503 device not ready
- [ ] Streaming multipart parser (no whole-body buffering)
- [ ] Streaming parser for the `/display` JSON: base64 of `image` decoded
      in blocks straight to flash (cJSON needs the whole document in RAM)
- [ ] Startup log of the endpoint list

### 5b. Config page (new)

A web page for every setting, separate from `/test`, so nothing needs
`menuconfig` or a reflash after the first install.

- [ ] `GET /config`: the page, embedded gzipped in flash like `/test`
- [ ] `GET /api/config`: every setting as JSON (Wi-Fi password left out)
- [ ] `POST /api/config`: validate, save to NVS, apply; reply with what
      needs a restart
- [ ] `POST /api/restart`
- [ ] Settings on the page, each with its default and range:
  - [ ] Wi-Fi SSID and password (restart)
  - [ ] mDNS hostname (restart)
  - [ ] HTTP port (restart)
  - [ ] panel width and height
  - [ ] panel match: name substring (`XYAO`) or MAC (reconnects)
  - [ ] upside down
  - [ ] GIF passthrough
  - [ ] stream fps (1–5) and resume delay
  - [ ] debug BLE logging
- [ ] Live settings apply without a restart; restart-only ones are marked on the page
- [ ] "Reset to defaults" (erases the settings namespace in NVS)
- [ ] Export: `GET /api/config/export` downloads every setting as
      `xyaoled-config.json`, Wi-Fi password and saved brightness included,
      with a `"version"` field for the format
- [ ] Import: `POST /api/config/import` with that file; validated as a whole
      before anything is saved (one bad value rejects the file), keys
      missing from it keep their current value, unknown keys are reported;
      replies with what needs a restart
- [ ] Export / import buttons on the page; import shows the changes before applying
- [ ] Import works from the setup portal too, so a new or wiped board is configured from a backup in one step
- [ ] Status block: firmware version, IP, Wi-Fi RSSI, panel connected, free heap, scratch space used/free

### 6. Live stream (Go: `stream.go`)

- [ ] `WS /stream` (`CONFIG_HTTPD_WS_SUPPORT`)
- [ ] Binary message = one image frame; frame size limit (Go: 4 MB; here bounded by RAM, e.g. 64 KB)
- [ ] Keep only the newest frame, drop the rest
- [ ] Rate limit `STREAM_FPS`; `?fps=N` and text `fps=N` override (1–5)
- [ ] Several clients counted; playlist blocked from first connect until resume
- [ ] Resume the playlist `STREAM_RESUME_DELAY` after the last disconnect; a reconnect within it cancels the resume and keeps the live view
- [ ] Sender task owns both frame sends and the resume (a late frame can't land after it)
- [ ] Configured rate restored after the stream ends

### 7. Not ported

- Docker (`Dockerfile`, `docker-compose.yml`, `DOCKER.md`, `verify-bluetooth.sh`): no host to run on
- BlueZ / D-Bus error workarounds (`WriteValue` retries, stale object paths)
- SIGINT / SIGTERM shutdown
- `default.gif`: referenced only by the Dockerfile
- OTA updates: not wanted; the board is flashed over USB

### 8. New on the ESP32

- [ ] Wi-Fi reconnect
- [ ] Credentials can also be set in `menuconfig` (skips the portal during development)

#### Wi-Fi setup portal

- [ ] No credentials in NVS: open the AP `xyaoled-setup`, WPA2, password `xyaoledaemon`
- [ ] DNS server answering every name with 192.168.4.1, so phones and laptops open the page by themselves (captive portal)
- [ ] `/config` lists the networks the ESP sees (scan in APSTA mode)
- [ ] Connection attempt keeps the AP up; the page shows the result (IP and `xyaoled.local`, or wrong password / network not found) and allows retrying
- [ ] The page survives the AP switching channel to match the router (single radio): it reconnects and polls for the result
- [ ] On success: save to NVS, turn the AP off after a few seconds
- [ ] Saved network unreachable for ~2 min: reopen the AP and keep retrying the saved network in parallel
- [ ] Measure Wi-Fi + BLE coexistence throughput (upload time and stream fps vs. the Go daemon's ~7 fps)
