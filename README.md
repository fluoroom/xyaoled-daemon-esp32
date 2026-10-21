# XYAOLED ESP32

Standalone firmware for an ESP32-C3 that keeps a Bluetooth connection to an
XYAOLED LED panel and displays images sent to it over Wi-Fi, with the HTTP
API of the original Go daemon.

| Path | |
|---|---|
| [`firmware/`](firmware) | ESP-IDF firmware, in C |
| [`docs/ESP32_PORT.md`](docs/ESP32_PORT.md) | Port plan, setup and feature checklist |
| [`docs/QUESTIONS.md`](docs/QUESTIONS.md) | Open decisions for the owner |
| [`reference/go-daemon/xyaoled/README.md`](reference/go-daemon/xyaoled/README.md) | Reverse-engineered panel protocol |
| [`reference/go-daemon/`](reference/go-daemon) | Original Linux daemon in Go, kept as the reference (git submodule of [fluoroom/xyaoled-daemon](https://github.com/fluoroom/xyaoled-daemon)) |

## Build

```bash
git clone --recurse-submodules https://github.com/fluoroom/xyaoled-daemon-esp32.git
# already cloned without it: git submodule update --init
# update the Go reference:    git submodule update --remote reference/go-daemon

. ~/esp/esp-idf-v6.0.3/export.sh
cd firmware
idf.py build flash monitor
```

Host tests, no board needed: `make -C firmware/test/host`. Integration
tests of the whole firmware in QEMU: `firmware/test/qemu`.

See [docs/ESP32_PORT.md](docs/ESP32_PORT.md#setup) for the toolchain,
serial-port setup and first boot (Wi-Fi setup portal).

Status: the whole daemon is ported and builds; the hardware-independent
parts pass the host tests, and the firmware passes the API tests in QEMU
with a simulated panel. BLE with the real panel and Wi-Fi have not run on
the board yet.

## Credits

The core protocol (BLE transport, upload/ack/retry logic, animation header,
playlist and live programs) is this project's own reverse engineering of the
vendor app, from its code and from HCI snoop captures of it. The `/power/on`,
`/power/off` and `/clear` commands were identified in October 2026 from
[samcgn/xyaoled-control](https://github.com/samcgn/xyaoled-control), an
independent Python reverse-engineering of the same device family — this is
not a fork of it; only those command definitions (trivial byte sequences)
were taken from its `PROTOCOL.md`.
