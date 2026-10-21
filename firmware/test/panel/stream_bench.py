#!/usr/bin/env python3
"""Measure the stream on the real panel: send frames over /stream faster
than the panel takes them, then print the fps it reached and where each
frame's time went (/api/status "stream").

    firmware/test/panel/stream_bench.py [http://xyaoled.local] [--fps 5] [--colors 256] [--seconds 15] [frame.png ...]

Without frames, it sends 96x16 PNGs like the test page's webcam (a moving
gradient with noise). The playlist comes back after the stream closes."""
import argparse
import asyncio
import json
import math
import random
import struct
import time
import urllib.request
import zlib

import websockets


def png(w, h, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def webcam_frames(n=16):
    rnd = random.Random(1)
    frames = []
    for i in range(n):
        px = []
        for y in range(16):
            for x in range(96):
                v = 128 + 90 * math.sin((x + i * 5) / 9) * math.cos(y / 5)
                px += [max(0, min(255, int(c + rnd.randint(-25, 25)))) for c in (v, 255 - v, x * 2)] + [255]
        frames.append(png(96, 16, px))
    return frames


def status(base):
    with urllib.request.urlopen(base + '/api/status', timeout=10) as r:
        return json.load(r)


async def run(base, fps, colors, seconds, frames):
    host = base.split('://', 1)[1]
    async with websockets.connect(f'ws://{host}/stream?fps={fps}&colors={colors}', max_size=None) as ws:
        start = time.monotonic()
        i = 0
        while time.monotonic() - start < seconds:
            await ws.send(frames[i % len(frames)])
            i += 1
            await asyncio.sleep(0.05)  # 20 offered per second: never starve the panel
        st = await asyncio.get_running_loop().run_in_executor(None, status, base)
        elapsed = time.monotonic() - start
    s = st['stream']
    print(f'{s["frames"]} frames in {elapsed:.1f} s = {s["frames"] / elapsed:.2f} fps (asked {fps}, {colors} colours), '
          f'{i} offered, heap {st["free_heap"]} (min {st["min_free_heap"]})')
    if s['frames']:
        print('per frame: ' + ', '.join(f'{k} {s[k]:.1f}' for k in
              ('decode_ms', 'encode_ms', 'select_ms', 'data_ms', 'max_send_ms', 'gif_bytes')))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base', nargs='?', default='http://xyaoled.local')
    ap.add_argument('--fps', type=int, default=5)
    ap.add_argument('--colors', type=int, default=256)
    ap.add_argument('--seconds', type=float, default=15)
    ap.add_argument('frames', nargs='*')
    a = ap.parse_args()
    frames = [open(f, 'rb').read() for f in a.frames] or webcam_frames()
    asyncio.run(run(a.base.rstrip('/'), a.fps, a.colors, a.seconds, frames))


if __name__ == '__main__':
    main()
