# xyaoled

Go driver for XYAOLED Bluetooth LED panels (BlueZ via `tinygo.org/x/bluetooth`).

The upload protocol was recovered from the vendor app (Xyao LED 1.6.0, a Flutter
app) with REA's Ghidra provider; see the comment at the top of `upload.go` for
the function addresses it was read from.

## BLE

- Service `ae00`
- `ae01`: write (commands and uploads)
- `ae02`: notify (device replies, `88 ff …`)

## Commands

Every frame starts with `99 aa 00 2e ff 88` followed by the total frame length
(uint16 LE).

- Connect: five fixed init commands (`initialize` in `device.go`).
- Brightness: `… 10 00 12 00 <seq> <level> <level*0.8> 00 00 00`, level 0–125.
- Select slot: `… 0d 00 04 <seq> <slot> ff 00`.
- Power on/off (type `0x0011`): `… 0e 00 11 00 <seq> <01|02> 01 01`, `01`=on, `02`=off.
- Clear screen + wipe playlist (type `0x0005`): `… 11 00 05 00 <seq> 01 01 00 00 00 00`.
  Sent before every upload so it replaces the playlist instead of being
  appended to it.

The power and clear commands were identified from
[samcgn/xyaoled-control](https://github.com/samcgn/xyaoled-control)'s
`PROTOCOL.md`, an independent reverse-engineering of the same device family
(not a fork — only these two command definitions were taken from it).

## Uploading a GIF

The GIF file is sent as-is, split into frames of at most 10000 bytes:

| Bytes | First frame | Following frames |
|---|---|---|
| 0–5 | magic | magic |
| 6–7 | frame length | frame length |
| 8 | `07` | `08`, or `09` for the last |
| 9–10 | seq, slot | seq, slot |
| 11–13 | bytes remaining (uint24 LE) | bytes remaining |
| 14–15 | frame count + 1 | frame index (1-based) |
| 16–23 | mode trailer (meaning unknown) | — |
| rest | GIF data | GIF data |

Each frame is written in MTU−3 sized writes, then the sender waits for the
device's ack before sending the next frame:

```
88 ff 03 <idx lo> <idx hi> <ctr> .. .. <status> ..
```

A repeated `ctr` is a duplicate and ignored. `status >= 0x14` means OK; lower
values are rejections and abort the upload. Without an ack the frame is
re-sent after `200ms per 250 bytes + 200ms` (up to 3 times).

All writes to the device go through one mutex: a command written in the middle
of an upload corrupts the frame being reassembled.
