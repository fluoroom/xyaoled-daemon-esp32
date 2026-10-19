# xyaoled

Go driver for XYAOLED Bluetooth LED panels (BlueZ via `tinygo.org/x/bluetooth`),
and the reverse-engineered protocol it implements.

Sources, in order of trust:

1. **HCI snoop captures** of the vendor app (Xyao LED 1.6.0, Flutter) on an
   Android phone, October 2026: GIF uploads (including a 172-frame GIF),
   multi-item playlists, a play-count change and a drawing session.
2. **Experiments** against a 96×16 panel with this driver.
3. **Static analysis** of the vendor `libapp.so` with REA's Ghidra provider,
   for the upload, ack and resend logic (addresses in `upload.go`).
4. `/power` and `/clear` command definitions from
   [samcgn/xyaoled-control](https://github.com/samcgn/xyaoled-control)'s
   `PROTOCOL.md`, an independent reverse-engineering of the same device
   family (not a fork; only those two byte sequences were taken).

## BLE

- Service `ae00`
- `ae01`: write without response (commands and uploads)
- `ae02`: notify (device replies, `88 ff …`)

The vendor app negotiates MTU 517 and writes 512-byte chunks. Frames longer
than one write are split across consecutive writes; the panel reassembles
them using the length field. Every write, command or upload, goes through one
lock: a command written in the middle of an upload corrupts the frame being
reassembled.

There is no separate data channel. Everything the app sends starts with
`99 aa`. Everything starting `88 ff` is a notification from the panel.

## App → panel frames

```
99 aa 00 2e ff 88 <len u16> <type> …
```

`len` is the total frame length including the 8-byte prefix. Multi-byte
numbers are little-endian. Most commands carry a sequence number that the
app increments per command; the panel does not seem to check it.

| Type | Bytes after the length | Meaning |
|---|---|---|
| `00 00` | `<seq> 00 YY MM DD hh mm ss <weekday> 01 00 <6 bytes> 00 00 00 00` | Handshake / clock sync. First command on every connect. See below. |
| `02 00` | `<seq> 00 01 01 00 00 00` | Unknown. Sent when the drawing screen is entered and when it is cleared. Makes the playlist stall for about a second; no other visible effect. |
| `04` | `<program> <seq> ff 00` | Select program before an upload. Acked with `88ff03` index 0. |
| `05 00` | `<seq> 01 01 00 00 00 00` | Clear the screen and wipe the playlist. Not acked. |
| `07`/`08`/`09` | see [Uploads](#uploads) | Upload packet (first / middle / last). |
| `0a 00` | `<seq> 00 00 00 00` | Leave the live program and resume the playlist. The app sends it whenever it enters or leaves a screen, and it is part of the handshake. |
| `0c 00` | `<seq> 00 06 00 02 03 00 00 00` | Sent when the play count of playlist item 1 was changed from 1 to 2. Not understood, see [Open questions](#open-questions). |
| `11 00` | `<seq> <01 on / 02 off> 01 01` (len `0e`) | Panel power. |
| `12 00` | `<seq> <level> <level×0.8> 00 00 00` | Brightness, level 0–125. |

### Handshake

The driver connects with:

1. `00 00` handshake (a fixed, replayed frame; see below)
2. `11 00` power off, long form: `… 12 00 11 00 <seq> 02 01 01 00 00 00 00`
3. `11 00` power on, long form, state `01`
4. `0a 00` resume
5. `05 00` clear playlist

The panel answers the handshake with:

```
88 ff 00 02 01 04 <width u16> <height u16> 08 01 <level> <level×0.8> 06 01 00 … <sum>
```

`60 00 10 00` is 96×16. `<level>` is the current brightness (seen as `7d 64`,
125/100, and `28 20`, 40/32).

The handshake frame carries the phone's local time, then `01 00` and 6 bytes
that change with it. **The panel checks those 6 bytes.** A frame with a new
time and the old 6 bytes is answered with `88 ff 0f …` and the panel
disconnects. How they are derived is not known: the vendor sent the same 6
bytes at 04:54:59 and at 05:00:27, and different ones at 04:58:15. So the
driver replays a frame the panel accepts, timed 2025-12-27 00:22:41
(a Saturday, weekday `06`).

## Panel → app notifications (`ae02`)

Every notification ends with a checksum: the low byte of the sum of all
preceding bytes.

| Prefix | Layout | Meaning |
|---|---|---|
| `88 ff 00` | see Handshake | Handshake accepted, panel info |
| `88 ff 0f` | `… 60 00 10 00 08 00 …` | Handshake rejected; the panel disconnects |
| `88 ff 03` | `<idx u16> <ctr> <n> 00 <status> 0b <sum>` | Ack. `idx` 0 acks a program select, `idx` n acks upload packet n. `ctr` counts notifications, so a repeat is a duplicate. `n` is the packet count + 1 of the current upload. `status >= 0x14` is OK; the app aborts on lower values. |
| `88 ff 05` | `01 <item> 00 00 00 00 0b <sum>` | A playlist upload was stored as item `<item>` (0-based). Also sent in place of a packet ack when the panel is not in receive mode. |

## Uploads

The payload is a GIF file sent as-is; the panel decodes it. Before an upload
the app selects the program (`04`, acked with index 0), then splits the GIF
into packets of at most 10000 bytes:

| Bytes | First packet (`07`) | Following packets (`08`, last `09`) |
|---|---|---|
| 0–5 | magic | magic |
| 6–7 | packet length | packet length |
| 8 | `07` | `08`, or `09` for the last |
| 9–10 | program, seq | program, seq |
| 11–13 | bytes remaining (uint24) | bytes remaining |
| 14–15 | packet count + 1 | packet index (1-based) |
| 16–23 | animation trailer | — |
| rest | GIF data | GIF data |

The seq is the one used for the program select. After each packet the app
waits for its `88ff03` ack. Without an ack it re-sends the packet after
`200ms per 250 bytes + 200ms`, up to 3 times.

### Animation trailer

```
<mode> 01 <total ms % 1000 u16> <GIF frame count u16> <total seconds u16>
```

The panel plays only as many GIF frames as the count says, so it must match
the GIF. A hardcoded count of 6 here was the old "6 frame limit". Captured
values:

| Trailer | Upload |
|---|---|
| `00 01 a0 00 ac 00 05 00` | 172 frames × 30 ms = 5.160 s |
| `00 01 30 02 a4 00 06 00` | 164 frames × 40 ms = 6.560 s |
| `00 01 20 03 06 00 01 00` | 6 frames × 300 ms = 1.800 s |
| `01 01 00 00 01 00 08 00` | static image (mode `01`), 1 frame, 8 s |

The app sends static images and drawing frames as one-frame GIFs with the
mode `01` trailer.

## Programs

The program byte in the select and the upload decides what an upload does:

- **`02`: append to the playlist.** The app builds a playlist by clearing
  (`05`) and then uploading each item to program 2 in turn; the panel
  answers each with `88ff05 01 <item>`. The playlist loops. There is no
  playlist command and no reordering. Re-selecting program 2 does not
  leave the live program.
- **`03`: live.** The upload is shown immediately and not stored (no
  `88ff05`). It stays until `0a` resumes the playlist, or until the next
  program-2 upload.

### Drawing (the app's pixel-art screen)

There is no pixel command. Every stroke re-uploads the whole 96×16 canvas as a
one-frame GIF (70–170 bytes) to program 3, with select + upload per change.
Erasing sends the next full frame without those pixels. Leaving the screen
sends `0a`. The "clear" on entering the drawing screen uploads a black frame
to program **2**, which leaves a black item in the playlist (a vendor app
bug).

With this driver, back-to-back live frames run at about 7–8 fps (ack latency
about 150 ms median, 50 ms best). Skipping the select for consecutive frames
works but is no faster: the panel's ack time dominates.

## Open questions

- **Handshake bytes**: the 6 bytes after the time, see Handshake.
- **`0c` (play count)**: the app sent `00 06 00 02 03 00 00 00` before
  re-uploading an unchanged playlist (byte-identical GIFs and trailers), so
  the play count is not in the upload. `06 00` matches the item's frame
  count and `02` the new play count. Replayed on a playlist whose first item
  has 10 frames: with `06 00` and count 3 that item froze on its first frame
  for a while; with `0a 00` and count 3 nothing visible changed.
- **`02 00`**: see the command table.
- **Panel reboot**: a 3-item playlist uploaded completely (items stored as 0,
  1, 2), after which the panel rebooted. The same app had just failed one
  attempt at it. Another 3-item upload and larger 2-item playlists worked.

## How the captures were made

Android → Developer options → Enable Bluetooth HCI snoop log → **Enabled**
(not Filtered, which truncates ATT payloads), toggle Bluetooth, use the app,
then `adb bugreport` and extract `FS/data/log/bt/btsnoop_hci.log`. The
snoop timestamps are UTC. With tshark:

```bash
tshark -r btsnoop_hci.log -Y 'btatt.opcode==0x52 || btatt.opcode==0x1b' \
  -T fields -e frame.time_epoch -e btatt.opcode -e btatt.value
```

`0x52` is a write command to `ae01` (handle `0x0082` here) and `0x1b` a
notification from `ae02` (handle `0x0084`). Join consecutive writes into
frames using the length field.
