# XYAOLED Daemon

HTTP daemon that keeps a Bluetooth connection to an XYAOLED LED panel and
displays images sent to it. Images are resized to the panel (96x16 by default);
GIFs keep their frame timing and loop forever. Every send reinitialises the
panel first.

## Build and run

```bash
go build -o xyaoled-daemon .
./xyaoled-daemon
```

See [DOCKER.md](DOCKER.md) for running in a container.

## Configuration

Read from `.env` (falls back to `.env.example`); environment variables override it.

| Key | Default | |
|---|---|---|
| `LED_PANEL_MAC` | — | Panel MAC address |
| `LED_DISPLAY_WIDTH` / `LED_DISPLAY_HEIGHT` | 96 / 16 | Panel size |
| `HTTP_HOST` / `HTTP_PORT` | 0.0.0.0 / 11001 | Listen address |
| `UPSIDE_DOWN` | false | Rotate images 180° |
| `DEBUG_BLE` | false | Log every BLE write and ack |

## Endpoints

| Method | Path | |
|---|---|---|
| GET | `/test` | Test page: connect, send, brightness |
| POST | `/display`, `/display-r` | JSON `{"image": "<base64>", "type": "static"\|"animated"}`; `type` is detected when empty |
| POST | `/upload` | Multipart `file`; animated if it is a multi-frame GIF |
| POST | `/playlist` | Multipart `file`, repeated: replaces the playlist with these items, played in order and looped |
| POST | `/live` | Image or GIF as multipart `file` or the raw body: shown now without touching the playlist, until `/resume` or the next upload. Frames posted back to back give about 7 fps |
| GET | `/clear-screen` | Blank the screen, keep the playlist (`/resume` brings it back) |
| GET | `/resume` | Leave `/live` or `/clear-screen` and play the playlist |
| POST | `/uploadTest` | Multipart `command`: `connect`, `disconnect`, `send_animated` (+ `animated` file), `send_static` (+ `static` file), `screen_on`, `screen_off`, `brightness_cycle` (+ `brightness` steps), `raw` (+ `hex`: a command from its type byte on, e.g. `0a00 01 00000000`; for protocol experiments) |
| POST | `/uploadTestGif` | Multipart `gif`; sent as animated |
| GET | `/brightness/{n}` | Set brightness 5–125 |
| GET | `/off`, `/on` | Dim to 5 / restore the previous level |
| GET | `/power/on`, `/power/off` | Hardware power on/off (distinct from `/off`, `/on`, which only dim brightness) |
| GET | `/clear` | Clear the screen and wipe the on-device playlist |
| GET | `/reconnect` | Reconnect to the panel |
| GET | `/health` | Health check |

Older `/uploadTest` command names (`send_*_reinit`, `send_*_no_reinit`,
`send_playlist`) still work; they all send with reinit. `GET /upload`,
`/uploadTest` and `/uploadTestGif` redirect to `/test`.

```bash
curl -X POST http://localhost:11001/display \
  -H "Content-Type: application/json" \
  -d "{\"image\":\"$(base64 -w 0 animation.gif)\"}"

curl -F file=@first.gif -F file=@second.png http://localhost:11001/playlist
curl --data-binary @frame.png http://localhost:11001/live
curl http://localhost:11001/resume
```

## Protocol

See [xyaoled/README.md](xyaoled/README.md).

The core protocol (BLE transport, upload/ack/retry logic, animation header,
playlist and live programs) is this project's own reverse engineering of the
vendor app, from its code and from HCI snoop captures of it. The `/power/on`, `/power/off` and
`/clear` commands were identified in October 2026 from
[samcgn/xyaoled-control](https://github.com/samcgn/xyaoled-control), an
independent Python reverse-engineering of the same device family — this is
not a fork of it; only those command definitions (trivial byte sequences)
were taken from its `PROTOCOL.md`.
