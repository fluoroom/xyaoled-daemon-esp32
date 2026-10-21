# ESP32 port

Port of the Go daemon ([`reference/go-daemon`](../reference/go-daemon)) to
firmware that runs on its own: the ESP32 joins Wi-Fi, keeps the BLE
connection to the panel and serves the same HTTP API. The Go code stays as
the reference; the protocol is in [xyaoled/README.md](../reference/go-daemon/xyaoled/README.md).

Every feature of the Go daemon is listed below, with the new ones. An item
is ticked only when it works on the board, not when it compiles:

| Mark | Meaning |
|---|---|
| `[x]` | works on the board |
| `[ ] 🖥️` | the firmware does it in QEMU, with a simulated panel and Ethernet instead of the radio (`firmware/test/qemu`) |
| `[ ] 🧪` | written, and its logic passes the host tests (`make -C firmware/test/host`) |
| `[ ] 🔨` | written and builds; not run yet |
| `[ ]` | not written |

Open questions for the owner are in [QUESTIONS.md](QUESTIONS.md).

## Target

| | |
|---|---|
| Board | ESP32-C3 Zero / Pro Mini |
| SoC | ESP32-C3FH4: 1 × RISC-V core at 160 MHz, 4 MB flash in package, no PSRAM |
| RAM | 400 KB SRAM; 314 KB reach the app. The firmware's static use leaves 224 KB for the heap before Wi-Fi and BLE allocate theirs (to be measured on the board) |
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
firmware/
  main/                    app: settings, storage, wifi, display, http, stream
    pages/config.html      the /config page (the /test page is the Go one, from the submodule)
  components/
    xyaoled/               panel protocol: xy_proto (frames), xy_upload (acked uploads), xy_panel (NimBLE)
    imaging/               GIF/PNG/JPEG decoders onto the panel canvas, frame store, GIF encoder, passthrough scan
    httputil/              streaming multipart and /display JSON-base64 parsers
    pngle/, tjpgd/         vendored PNG and JPEG decoders (MIT / ChaN)
  test/host/               host tests (make) and the Go fixture generator (gogen/)
docs/                      this file, QUESTIONS.md
reference/go-daemon/       original Go daemon (Linux/BlueZ), git submodule
```

Registry components (pinned in `firmware/main/idf_component.yml`):
`joltwallet/littlefs` 1.23.0, `espressif/mdns` 1.14.0, `espressif/cjson` 1.7.19~2.

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

Host tests (gcc with ASan and UBSan; no board needed):

```bash
make -C firmware/test/host            # protocol, uploads, imaging, parsers
make -C firmware/test/host verify-go  # also decode the encoder's GIFs with Go's image/gif
make -C firmware/test/host fixtures   # regenerate fixtures from the Go reference logic
```

QEMU integration tests: the firmware built with `CONFIG_XY_QEMU`
(Ethernet instead of Wi-Fi, an in-process simulated panel instead of BLE,
`/api/sim/*` to fetch what the panel received) runs in Espressif's QEMU;
`test_api.py` calls every endpoint and compares the panel's GIFs with the
Go reference frames:

```bash
. ~/esp/esp-idf-v6.0.3/export.sh
QEMU=/path/to/qemu-system-riscv32 firmware/test/qemu/run.sh &   # serial log: firmware/build-qemu/serial.log
python firmware/test/qemu/test_api.py                          # IDF's Python: it has websockets
```

QEMU comes from `idf_tools.py install qemu-riscv32` (or the release listed
in `tools/tools.json`; on this machine it is unpacked in
`firmware/build/qemu`). Its build differs from the board's in
`firmware/sdkconfig.qemu`.

First boot: with no Wi-Fi saved the board opens `xyaoled-setup` (password
`xyaoledaemon`); join it and the phone or laptop opens the setup page (or go
to `http://192.168.4.1`). Afterwards the board is at
`http://xyaoled.local:11001/` (`/config`, `/test`).

## Checklist

### 0. Groundwork

- [x] Go daemon in `reference/go-daemon` as a git submodule of `fluoroom/xyaoled-daemon`
- [x] Protocol doc read from the submodule (`reference/go-daemon/xyaoled/README.md`), not copied
- [x] Language decided: C
- [x] ESP-IDF v6.0.3 installed for `esp32c3`
- [x] Board detected (`303a:1001` on `/dev/ttyACM0`)
- [x] Firmware builds (1.17 MB app, 22% of the partition free)
- [ ] Flashed and boots on the board; heap measured at boot, with Wi-Fi up, with BLE up (logged by `main.c`)
- [x] Partition table: one 1.5 MB app (no OTA) + NVS + 2.4 MB LittleFS scratch (builds)
- [ ] 🖥️ LittleFS mounted on `scratch` at `/scratch`, emptied at boot
- [x] Host test harness (`firmware/test/host`), fixtures generated from the Go logic
- [x] QEMU build of the whole firmware with a simulated panel, and API tests against it (`firmware/test/qemu`)
- [ ] 🖥️ Boots: free heap in QEMU (no radio) 314 KB at boot, 257 KB with HTTP up, 194 KB lowest under the API tests; flat across repeated requests (no leaks)

### 1. Configuration (Go: `config.go`, `.env`)

The `.env` file has no counterpart; settings come from Kconfig defaults
(`idf.py menuconfig` → XYAOLED) and are stored in NVS as one JSON value, so a
save is all or nothing, when changed from the `/config` page (section 5b).

- [ ] 🔨 Wi-Fi SSID and password (new: the Go daemon used the host's network)
- [ ] 🔨 `LED_DISPLAY_WIDTH` / `LED_DISPLAY_HEIGHT` → `panel_width` / `panel_height` (96 / 16)
- [ ] 🔨 `HTTP_PORT` → `http_port` (11001); `HTTP_HOST` does not apply
- [ ] 🔨 `UPSIDE_DOWN` → `upside_down`
- [ ] 🔨 `DEBUG_BLE` → `debug_ble` (logs every write and notification)
- [ ] 🖥️ `STREAM_FPS` → `stream_fps` (default 4, 1–5)
- [ ] 🖥️ `STREAM_RESUME_DELAY` → `stream_resume_delay` (seconds, default 1.5)
- [ ] 🔨 Panel matching: name substring (`XYAO`, the Go behaviour) or MAC (`panel_match`, `panel_name`, `panel_mac`). The Go `LED_PANEL_MAC` was read but never used.
- [ ] 🖥️ Startup log of the configuration
- [ ] 🔨 New: mDNS name (`hostname`, default `xyaoled` → `xyaoled.local`)
- [ ] 🖥️ New: GIF passthrough on/off (`passthrough`, default on)

### 2. BLE driver (Go: `xyaoled/device.go`, `xyaoled/upload.go`)

`xy_proto` (frames) and `xy_upload` (acked uploads over an abstract link)
are pure C and host-tested; `xy_panel` is the NimBLE side.

- [ ] 🔨 NimBLE central: active scan, name containing `XYAO` (case-insensitive) or MAC, 30 s timeout
- [ ] 🔨 Connect, discover service `ae00`, write char `ae01`, notify char `ae02`, its CCCD
- [ ] 🔨 MTU 517 requested, writes of MTU − 3 up to 512 bytes; data length extension requested
- [ ] 🧪 Frames longer than one write split across writes (host: 20-byte and 512-byte writes)
- [ ] 🔨 One lock for everything written (a command mid-upload corrupts it)
- [ ] 🧪 Notification queue; drained before each command that waits for an answer
- [ ] 🧪 Sequence number per command
- [ ] 🧪 Handshake frames (replayed `00 00`, power off, power on, `0a`, `05`) with 300 ms / 30 ms delays
- [ ] 🧪 Handshake reply parsed (`88 ff 00`: width, height, brightness); rejection (`88 ff 0f`) reported
- [ ] 🧪 Notification checksum (parser; not enforced, as in Go)
- [ ] 🧪 Brightness `12 00` (level, level × 0.8), power `11 00`, clear `05 00`, resume `0a 00`, raw commands
- [ ] 🧪 Clear before a playlist upload, never acked, 500 ms settle
- [ ] 🧪 Program select `04` acked with index 0, re-sent every 1 s, panel-busy timeout 60 s
- [ ] 🧪 Upload packets `07`/`08`/`09`: ≤ 10000 bytes, uint24 remaining, count/index (Go tests ported)
- [ ] 🧪 Animation trailer; static mode `01` (Go tests ported)
- [ ] 🧪 Ack parsing (`88 ff 03`): index, duplicate counter, status ≥ `0x14`
- [ ] 🧪 Resend without ack after 200 ms per 250 bytes + 200 ms, up to 3 times
- [ ] 🧪 `88 ff 05` instead of an ack: select again and restart
- [ ] 🧪 Wait for `88 ff 05` (stored) after playlist uploads, 5 s timeout
- [ ] 🔨 Whole-send retry: 3 attempts, 50 ms × attempt backoff, none on connection errors or a busy panel
- [ ] 🧪 Program 2: replace (clear first) or append; program 3: no clear, no stored wait
- [ ] 🔨 Dead link detection: NimBLE disconnect event (no writes to `ae01`)
- [x] Go unit tests (`upload_test.go`) ported as host tests

### 3. Display logic (Go: `display.go`)

- [ ] 🖥️ Connection monitor every 5 s; connects at boot without blocking the HTTP server
- [ ] 🔨 Reconnect with exponential backoff 5 s → 60 s; brightness restored after it (saved, else 125 on the first connect and 80 after)
- [ ] 🔨 Reconnect triggered by a send that fails on a dead connection
- [ ] 🖥️ Brightness saved (Go: `.brightness` file; here NVS) and restored
- [ ] 🖥️ Screen "off" = brightness ≤ 5: image sends refused
- [ ] 🖥️ Manual off flag: brightness changes refused until `/on`
- [ ] 🖥️ Level before `/off` remembered for `/on`
- [ ] 🖥️ Streaming flag: playlist sends refused (409) while `/stream` holds the panel
- [ ] 🖥️ Clear screen: black live frame, playlist kept
- [ ] 🔨 Connect / disconnect on demand (`/uploadTest`); as in Go, the monitor reconnects after a disconnect

### 4. Image pipeline (Go: `display.go`)

- [ ] 🖥️ Upload bodies decoded in blocks as they arrive (source never stored, except passthrough candidates and `/uploadTest` files sent before the command)
- [ ] 🖥️ Scaled frames in the scratch partition; scratch files removed after each send
- [ ] 🧪 Frame limit from the free scratch space (`IMG_ERR_FULL`, a clear error)
- [ ] 🧪 Format detection (PNG / JPEG / GIF)
- [ ] 🧪 PNG decoder, streaming (pngle), alpha over black
- [ ] 🧪 JPEG decoder (TJpgDec, baseline; pre-scaled 1/2–1/8 when the image is large); progressive JPEG rejected with a clear error
- [ ] 🧪 GIF decoder: frames, delays, disposal none / background / previous, transparency, interlacing (interlace not covered by fixtures: Go's encoder cannot write it)
- [ ] 🧪 Contain-fit nearest-neighbour scaling to the panel, centred on black, while decoding — frames equal the Go output pixel for pixel
- [ ] 🧪 Rotate 180° (`upside_down`)
- [ ] 🧪 Shared palette: first 256 distinct colours across all frames
- [ ] 🧪 Floyd–Steinberg dithering (error vs. Go's on the same image: 15.97 vs 16.03)
- [ ] 🧪 GIF encoder: global colour table for animations, local for stills (as Go writes them), LZW with table resets, loop forever, background disposal; Go's `image/gif` decodes the output back exactly
- [ ] 🧪 Zero frame delay → 50 ms
- [ ] 🧪 Static image → one-frame GIF held 600 s, static trailer
- [ ] 🧪 Corrupt and truncated inputs fail cleanly (fuzzed under ASan)
- [ ] 🧪 GIF passthrough. A GIF whose logical screen matches the panel is
      stored as received and sent as it is; only the trailer is computed, from
      one scan of its blocks. Falls back to the full pipeline (decoding the
      stored copy) when any check fails:
  - [ ] 🧪 width × height equals the panel size
  - [ ] 🧪 `upside_down` is off
  - [ ] 🧪 two frames or more (a single frame goes the still path)
  - [ ] 🧪 no frame has a local colour table (the panel's decoder loses sync on them)
  - [ ] 🧪 no frame has a zero delay
  - [ ] 🧪 every frame covers the whole screen and has no transparency (see QUESTIONS.md),
        unless `passthrough_partial` is on (experimental, default off)
  - [ ] 🔨 the panel draws partial and transparent frames right: `firmware/test/panel/passthrough.sh`
  - [ ] 🖥️ passthrough enabled in the config
  - [ ] 🔨 the log says which path an upload took, and why passthrough was refused
- [x] Go image tests (`display_test.go`) covered: still held 600 s, animation delays and loop, by the fixture tests

### 5. HTTP API (Go: `handler.go`, `main.go`)

Same paths, form fields and status codes as the Go daemon.

- [ ] 🖥️ `esp_http_server` on port 11001
- [ ] 🖥️ `GET /health`: status and config (width, height, stream fps, resume delay); plus `panel_connected`, `free_heap`
- [ ] 🖥️ `GET /test`: the Go test page, embedded from the submodule
- [ ] 🖥️ `POST /display`, `/display-r`: JSON `{image: base64, type}`; type detected when empty (see QUESTIONS.md for `type` after `image`)
- [ ] 🖥️ `POST /upload`: multipart `file`; `GET` redirects to `/test`
- [ ] 🖥️ `POST /playlist`: repeated `file`; every item encoded before the playlist is replaced
- [ ] 🖥️ `POST /live`: multipart `file` or raw body; replies `ms`
- [ ] 🖥️ `GET /clear-screen`
- [ ] 🖥️ `GET /resume` (409 while streaming)
- [ ] 🖥️ `GET /clear` (409 while streaming)
- [ ] 🖥️ `POST /uploadTest`: every command and legacy name of the Go handler; `GET` redirects to `/test`
- [ ] 🖥️ `POST /uploadTestGif`: multipart `gif`; `GET` redirects to `/test`
- [ ] 🖥️ `GET /brightness/{n}`: clamped 5–125, 409 when manually off
- [ ] 🖥️ `GET /off`, `/on`
- [ ] 🖥️ `GET /power/on`, `/power/off`
- [ ] 🖥️ `GET /reconnect`: 202 `{"status":"reconnecting"}`
- [ ] 🖥️ Errors: 405 wrong method (server), 400 bad input, 409 streaming, 500 send failure; 503 panel not connected (not exercised)
- [ ] 🧪 Streaming multipart parser (boundaries split across reads, partial reads, truncation)
- [ ] 🧪 Streaming parser for the `/display` JSON: base64 of `image` decoded in blocks; other keys skipped, escaped `\/` and line breaks accepted
- [ ] 🖥️ Startup log of the endpoint list
- [ ] Requests are served one at a time: a long upload delays the others (see QUESTIONS.md)

### 5b. Config page (new)

- [ ] 🖥️ `GET /config`: the page, embedded in flash
- [ ] 🖥️ `GET /api/config`: every setting (Wi-Fi password left out), defaults, which keys need a restart
- [ ] 🖥️ `POST /api/config`: validated as a whole, saved, applied; replies `restart_required`
- [ ] 🖥️ `POST /api/restart`
- [ ] 🔨 Settings on the page, each with its default and range, restart-only ones marked:
  - [ ] 🔨 Wi-Fi SSID and password (restart; also the Wi-Fi section, which connects at once)
  - [ ] 🔨 mDNS hostname (restart)
  - [ ] 🔨 HTTP port (restart)
  - [ ] 🔨 panel width and height
  - [ ] 🔨 panel match: name substring or MAC (reconnects)
  - [ ] 🔨 upside down
  - [ ] 🔨 GIF passthrough
  - [ ] 🔨 stream fps (1–5) and resume delay
  - [ ] 🔨 debug BLE logging
- [ ] 🔨 "Reset to defaults" (erases the settings in NVS, restarts)
- [ ] 🖥️ Export: `GET /api/config/export` downloads `xyaoled-config.json` (password and brightness included, `"version": 1`)
- [ ] 🖥️ Import: `POST /api/config/import`; one bad value rejects the file, missing keys keep their value, unknown keys are reported
- [ ] 🔨 Export / import buttons; import shows the changes before applying
- [ ] 🔨 Import works from the setup portal too
- [ ] 🔨 Status block: firmware version, uptime, IP, host name, Wi-Fi RSSI, portal, panel, brightness, heap, scratch use

### 6. Live stream (Go: `stream.go`)

- [ ] 🖥️ `WS /stream` (`CONFIG_HTTPD_WS_SUPPORT`)
- [ ] 🔨 Binary message = one image frame, decoded in RAM; frames over 64 KB close the socket (Go: 4 MB)
- [ ] 🖥️ Only the newest frame kept
- [ ] 🖥️ Rate limit `stream_fps`; `?fps=N` and text `fps=N` override (1–5)
- [ ] 🔨 Up to 4 clients counted; playlist blocked from the first connect until the resume
- [ ] 🖥️ Playlist resumes `stream_resume_delay` after the last disconnect; a reconnect within it keeps the live view
- [ ] 🖥️ One task sends frames and the resume (a late frame can't land after it)
- [ ] 🔨 Configured rate restored after the stream ends

### 7. Not ported

- Docker (`Dockerfile`, `docker-compose.yml`, `DOCKER.md`, `verify-bluetooth.sh`): no host to run on
- BlueZ / D-Bus error workarounds (`WriteValue` retries, stale object paths)
- SIGINT / SIGTERM shutdown
- `default.gif`: referenced only by the Dockerfile
- OTA updates: not wanted; the board is flashed over USB

### 8. New on the ESP32

- [ ] 🔨 Wi-Fi reconnect with backoff (1 s → 30 s)
- [ ] 🖥️ A 300-frame 120×20 animation through the full pipeline (~1.4 MB of frames in scratch), frames equal to Go's
- [ ] 🔨 Credentials can also be set in `menuconfig` (skips the portal during development)
- [ ] Measure Wi-Fi + BLE coexistence throughput (upload time and stream fps vs. the Go daemon's ~7 fps)

#### Wi-Fi setup portal

- [ ] 🔨 No credentials in NVS: open the AP `xyaoled-setup`, WPA2, password `xyaoledaemon`
- [ ] 🔨 Port 80 redirects to `/config`, so phones and laptops open the page by themselves (captive portal), also when the AP opens before the HTTP server (first boot)
- [ ] 🧪 DNS server answering every A query with 192.168.4.1 (malformed packets fuzzed)
- [ ] 🔨 `/config` lists the networks the ESP sees (scan in APSTA mode)
- [ ] 🔨 Connection attempt keeps the AP up; the page shows the result (IP and `xyaoled.local`, or wrong password / network not found / timed out) and allows retrying
- [ ] 🔨 The page survives the AP switching channel to match the router (single radio): it keeps polling through errors
- [ ] 🔨 On success: saved to NVS, the AP turns off 10 s later
- [ ] 🔨 Saved network unreachable for 2 min: the AP reopens while the saved network keeps being retried
