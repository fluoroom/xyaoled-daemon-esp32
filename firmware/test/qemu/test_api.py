#!/usr/bin/env python3
"""HTTP API tests against the firmware running in QEMU (test/qemu/run.sh).

Every endpoint of the Go daemon is called with the Go daemon's fields and
checked for its status codes; what the simulated panel received is fetched
from /api/sim/last and compared with the frames the Go logic produces
(fixtures from test/host/gogen), decoding it with Go's image/gif.

    python test/qemu/test_api.py [base_url]   # default http://127.0.0.1:18001
"""
import asyncio
import base64
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid

BASE = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:18001'
HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.join(HERE, '..', 'host', 'fixtures')
GOGEN = os.path.join(HERE, '..', 'host', 'gogen')
GOCACHE = os.path.join(HERE, '..', 'host', 'build', 'gocache')

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print('  FAIL', what)


def fixture(name):
    with open(os.path.join(FIX, name), 'rb') as f:
        return f.read()


class Resp:
    def __init__(self, status, headers, body):
        self.status, self.headers, self.body = status, headers, body

    def json(self):
        return json.loads(self.body)

    @property
    def text(self):
        return self.body.decode(errors='replace')


def request(method, path, body=None, headers=None, timeout=180):
    req = urllib.request.Request(BASE + path, data=body, method=method, headers=headers or {})

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *a, **k):
            return None

    opener = urllib.request.build_opener(NoRedirect)
    try:
        with opener.open(req, timeout=timeout) as r:
            return Resp(r.status, r.headers, r.read())
    except urllib.error.HTTPError as e:
        return Resp(e.code, e.headers, e.read())


def get(path):
    return request('GET', path)


def post_json(path, obj):
    return request('POST', path, json.dumps(obj).encode(), {'Content-Type': 'application/json'})


def multipart(path, fields):
    """fields: list of (name, value, filename or None)."""
    boundary = '----xyaotest' + uuid.uuid4().hex
    out = b''
    for name, value, filename in fields:
        disp = f'form-data; name="{name}"' + (f'; filename="{filename}"' if filename else '')
        out += f'--{boundary}\r\nContent-Disposition: {disp}\r\n'.encode()
        if filename:
            out += b'Content-Type: application/octet-stream\r\n'
        out += b'\r\n' + (value if isinstance(value, bytes) else value.encode()) + b'\r\n'
    out += f'--{boundary}--\r\n'.encode()
    return request('POST', path, out, {'Content-Type': f'multipart/form-data; boundary={boundary}'})


def stats():
    return get('/api/sim/stats').json()


def last():
    r = get('/api/sim/last')
    return r.body, r.headers.get('X-Trailer'), r.headers.get('X-Program')


def go_verify(gif, frames_name):
    """Decode gif with Go's image/gif and compare with fixture frames (a
    fixture name, or a path without .rgb)."""
    with tempfile.NamedTemporaryFile(suffix='.gif', delete=False) as f:
        f.write(gif)
        path = f.name
    env = dict(os.environ, GOCACHE=GOCACHE, GOPROXY='off', GOTOOLCHAIN='local')
    frames = frames_name if os.path.isabs(frames_name) else os.path.join(FIX, frames_name)
    r = subprocess.run(['go', 'run', '.', 'verify', path, frames + '.rgb'],
                       cwd=GOGEN, env=env, capture_output=True, text=True)
    os.unlink(path)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def trailer(static, frames=0, total_ms=0):
    if static:
        return '0101000001000800'
    ms, secs = total_ms % 1000, total_ms // 1000
    return bytes([0, 1, ms & 255, ms >> 8, frames & 255, frames >> 8, secs & 255, secs >> 8]).hex()


# ---- Tests ----

def test_pages_and_health():
    r = get('/health')
    check(r.status == 200, f'/health {r.status}')
    h = r.json()
    check(h['status'] == 'healthy' and h['config']['width'] == 96 and h['config']['height'] == 16
          and h['config']['stream_fps'] == 4 and h['config']['stream_resume_delay_ms'] == 1500, f'/health body {h}')
    r = get('/test')
    check(r.status == 200 and b'XYAOLED test page' in r.body, f'/test {r.status}')
    r = get('/config')
    check(r.status == 200 and b'XYAOLED configuration' in r.body, f'/config {r.status}')
    r = get('/')
    check(r.status == 302 and r.headers.get('Location') == '/config', f'GET /: {r.status}')
    for p in ('/upload', '/uploadTest', '/uploadTestGif'):
        r = get(p)
        check(r.status == 302 and r.headers.get('Location') == '/test', f'GET {p}: {r.status}')
    r = request('DELETE', '/health')
    check(r.status == 405, f'DELETE /health: {r.status}')


def test_upload_animation():
    s0 = stats()
    r = multipart('/upload', [('file', fixture('anim_small.gif'), 'a.gif')])
    check(r.status == 200 and r.json()['status'] == 'ok', f'/upload anim: {r.status} {r.text}')
    gif, tr, prog = last()
    s1 = stats()
    check(prog == '2', f'program {prog}')
    check(tr == trailer(False, 4, (10 + 5 + 20 + 7) * 10), f'trailer {tr}')
    check(s1['clears'] == s0['clears'] + 1, 'playlist not cleared first')
    check(s1['header_errors'] == 0, 'packet header errors')
    ok, out = go_verify(gif, 'anim_small')
    check(ok, f'panel GIF vs Go frames: {out}')


def test_upload_large_animation():
    r = multipart('/upload', [('file', fixture('anim_large.gif'), 'big.gif')])
    check(r.status == 200, f'/upload large: {r.status} {r.text}')
    ok, out = go_verify(last()[0], 'anim_large')
    check(ok, f'large GIF vs Go frames: {out}')


def test_upload_still():
    r = multipart('/upload', [('file', fixture('png_small.png'), 'a.png')])
    check(r.status == 200, f'/upload png: {r.status} {r.text}')
    gif, tr, prog = last()
    check(tr == trailer(True), f'still trailer {tr}')
    ok, out = go_verify(gif, 'png_small')
    check(ok, f'still vs Go: {out}')


def test_passthrough():
    src = fixture('pass_ok.gif')
    r = multipart('/upload', [('file', src, 'p.gif')])
    check(r.status == 200, f'passthrough upload: {r.status} {r.text}')
    gif, tr, _ = last()
    check(gif == src, 'panel-sized GIF was re-encoded instead of passed through')
    check(tr == trailer(False, 3, 120), f'passthrough trailer {tr}')

    src = fixture('pass_lct.gif')
    r = multipart('/upload', [('file', src, 'l.gif')])
    check(r.status == 200, f'lct upload: {r.status}')
    gif, _, _ = last()
    check(gif != src and gif.startswith(b'GIF89a'), 'GIF with local colour tables was passed through')

    post_json('/api/config', {'passthrough': False})
    multipart('/upload', [('file', fixture('pass_ok.gif'), 'p.gif')])
    check(last()[0] != fixture('pass_ok.gif'), 'passthrough off but GIF sent as is')
    post_json('/api/config', {'passthrough': True})


def test_display_json():
    img = base64.b64encode(fixture('photo.jpg')).decode()
    r = post_json('/display', {'image': img})
    check(r.status == 200, f'/display jpeg: {r.status} {r.text}')
    check(last()[1] == trailer(True), 'jpeg not a still')
    r = post_json('/display-r', {'type': 'animated', 'image': base64.b64encode(fixture('anim_small.gif')).decode()})
    check(r.status == 200 and last()[1] == trailer(False, 4, 420), f'/display-r animated: {r.status}')
    # "static" keeps the first frame of an animation.
    r = post_json('/display', {'type': 'static', 'image': base64.b64encode(fixture('anim_small.gif')).decode()})
    check(r.status == 200 and last()[1] == trailer(True), f'/display static of a GIF: {r.status} {last()[1]}')
    r = post_json('/display', {'type': 'animated', 'image': base64.b64encode(fixture('png_small.png')).decode()})
    check(r.status == 500, f'animated PNG: {r.status}')
    r = request('POST', '/display', b'{not json', {'Content-Type': 'application/json'})
    check(r.status == 400 and 'Invalid JSON' in r.text, f'bad JSON: {r.status} {r.text}')
    r = post_json('/display', {'type': 'static'})
    check(r.status == 400 and 'Missing image data' in r.text, f'missing image: {r.status} {r.text}')
    r = post_json('/display', {'image': 'abc$'})
    check(r.status == 400 and 'base64' in r.text, f'bad base64: {r.status} {r.text}')
    r = post_json('/display', {'image': base64.b64encode(b'hello world').decode()})
    check(r.status == 500, f'not an image: {r.status}')


def test_playlist():
    s0 = stats()
    r = multipart('/playlist', [('file', fixture('png_small.png'), 'a.png'),
                                ('note', 'ignored', None),
                                ('file', fixture('anim_small.gif'), 'b.gif')])
    check(r.status == 200 and r.json().get('items') == 2, f'/playlist: {r.status} {r.text}')
    s1 = stats()
    check(s1['uploads'] == s0['uploads'] + 2 and s1['clears'] == s0['clears'] + 1,
          f'playlist uploads {s1["uploads"] - s0["uploads"]} clears {s1["clears"] - s0["clears"]}')
    r = multipart('/playlist', [('other', 'x', None)])
    check(r.status == 400 and 'No files' in r.text, f'empty playlist: {r.status} {r.text}')
    r = multipart('/playlist', [('file', fixture('png_small.png'), 'a.png'), ('file', b'garbage', 'b.bin')])
    check(r.status == 500 and 'item 2' in r.text, f'bad playlist item: {r.status} {r.text}')


def test_live():
    r = request('POST', '/live', fixture('png_small.png'), {'Content-Type': 'image/png'})
    check(r.status == 200 and 'ms' in r.json(), f'/live raw: {r.status} {r.text}')
    check(last()[2] == '3', 'live upload not to program 3')
    r = multipart('/live', [('file', fixture('photo.jpg'), 'p.jpg')])
    check(r.status == 200, f'/live multipart: {r.status}')
    r = request('POST', '/live', b'', {'Content-Type': 'image/png'})
    check(r.status == 400, f'/live empty: {r.status}')


def test_commands():
    s0 = stats()
    for path in ('/clear', '/clear-screen', '/resume', '/power/off', '/power/on'):
        r = get(path)
        check(r.status == 200 and r.json()['status'] == 'ok', f'{path}: {r.status} {r.text}')
    s1 = stats()
    check(s1['resumes'] == s0['resumes'] + 1 and s1['power'], 'resume/power not received')
    check(last()[2] == '3', 'clear-screen not a live frame')
    r = get('/reconnect')
    check(r.status == 202 and r.json()['status'] == 'reconnecting', f'/reconnect: {r.status}')
    time.sleep(3)


def test_brightness():
    r = get('/brightness/50')
    check(r.status == 200 and r.json()['brightness'] == 50 and stats()['brightness'] == 50, f'/brightness/50 {r.text}')
    check(get('/brightness/999').json()['brightness'] == 125, 'clamp to 125')
    check(get('/brightness/1').json()['brightness'] == 5, 'clamp to 5')
    check(get('/brightness/abc').status == 400, 'bad brightness')
    get('/brightness/50')
    r = get('/off')
    check(r.status == 200 and r.json()['brightness'] == '5', f'/off: {r.text}')
    r = get('/brightness/60')
    check(r.status == 409, f'brightness while off: {r.status}')
    r = multipart('/upload', [('file', fixture('png_small.png'), 'a.png')])
    check(r.status == 500 and 'screen is off' in r.text, f'upload while off: {r.status} {r.text}')
    r = get('/on')
    check(r.status == 200 and r.json()['brightness'] == 60, f'/on restores the last requested level: {r.text}')
    get('/brightness/100')


def test_upload_test():
    def cmd(*fields):
        return multipart('/uploadTest', list(fields))
    r = cmd(('command', 'raw', None), ('hex', '0a00 01 00000000', None))
    check(r.status == 200 and r.text == 'ok', f'raw: {r.status} {r.text}')
    r = cmd(('command', 'nope', None))
    check(r.status == 400 and 'Unknown command' in r.text, f'unknown: {r.status}')
    r = cmd(('command', 'send_static', None))
    check(r.status == 500 and 'missing static upload' in r.text, f'missing file: {r.status} {r.text}')
    r = cmd(('command', 'send_static', None), ('static', fixture('png_small.png'), 's.png'))
    check(r.status == 200 and last()[1] == trailer(True), f'send_static: {r.status} {r.text}')
    # The file before the command, as curl -F can send it.
    r = cmd(('animated', fixture('anim_small.gif'), 'a.gif'), ('command', 'send_animated_no_reinit', None))
    check(r.status == 200 and last()[1] == trailer(False, 4, 420), f'file before command: {r.status} {r.text}')
    r = cmd(('command', 'send_playlist', None), ('static', fixture('png_small.png'), 's.png'),
            ('animated', fixture('anim_small.gif'), 'a.gif'))
    check(r.status == 200 and last()[1] == trailer(True), f'send_playlist sends the still last: {r.status} {last()[1]}')
    r = cmd(('command', 'brightness_cycle', None), ('brightness', '10,20,90', None))
    check(r.status == 200 and stats()['brightness'] == 90, f'brightness_cycle: {r.status}')
    r = cmd(('command', 'screen_off', None))
    check(r.status == 200, 'screen_off')
    r = cmd(('command', 'screen_on', None))
    check(r.status == 200 and stats()['brightness'] == 80, 'screen_on → 80')
    r = cmd(('command', 'raw', None), ('hex', 'zz', None))
    check(r.status == 500, f'bad hex: {r.status}')
    r = multipart('/uploadTestGif', [('gif', fixture('anim_small.gif'), 'g.gif')])
    check(r.status == 200 and r.json()['status'] == 'ok', f'/uploadTestGif: {r.status}')
    r = multipart('/uploadTestGif', [('other', 'x', None)])
    check(r.status == 400, f'/uploadTestGif without gif: {r.status}')
    get('/brightness/100')


def test_config_api():
    c = get('/api/config').json()
    check('wifi_password' not in c['settings'] and 'http_port' in c['restart_keys'], 'config GET')
    r = post_json('/api/config', {'stream_fps': 3, 'bogus': 1})
    j = r.json()
    check(r.status == 200 and not j['restart_required'] and j['unknown_keys'] == ['bogus'], f'POST config: {j}')
    check(get('/health').json()['config']['stream_fps'] == 3, 'stream_fps not applied')
    r = post_json('/api/config', {'stream_fps': 2, 'panel_width': 0})
    check(r.status == 400 and 'panel_width' in r.json()['error'], f'invalid value: {r.status} {r.text}')
    check(get('/health').json()['config']['stream_fps'] == 3, 'rejected change partly applied')
    r = post_json('/api/config', {'http_port': 11002})
    check(r.json().get('restart_required') is True, 'port change needs a restart')
    post_json('/api/config', {'http_port': 11001, 'stream_fps': 4})

    r = get('/api/config/export')
    e = r.json()
    check('attachment' in (r.headers.get('Content-Disposition') or ''), 'export not a download')
    check(e.get('version') == 1 and 'wifi_password' in e and 'brightness' in e, f'export fields {sorted(e)}')
    bad = dict(e, stream_resume_delay=999)
    r = post_json('/api/config/import', bad)
    check(r.status == 400, f'bad import accepted: {r.status}')
    good = dict(e, upside_down=True, hostname='xytest')
    r = post_json('/api/config/import', good)
    check(r.status == 200 and r.json()['restart_required'], f'import: {r.status} {r.text}')
    check(get('/api/config').json()['settings']['upside_down'] is True, 'import not applied')
    post_json('/api/config/import', e)
    check(get('/api/config').json()['settings']['hostname'] == e['hostname'], 'import back')
    r = request('POST', '/api/config', b'not json', {'Content-Type': 'application/json'})
    check(r.status == 400, f'POST config not JSON: {r.status}')


def test_long_animation():
    # 300 frames that are not panel-sized: the full pipeline over the
    # scratch partition (about 1.4 MB of frames).
    with tempfile.TemporaryDirectory() as d:
        base = os.path.join(d, 'long')
        env = dict(os.environ, GOCACHE=GOCACHE, GOPROXY='off', GOTOOLCHAIN='local')
        subprocess.run(['go', 'run', '.', 'anim', '120', '20', '300', base], cwd=GOGEN, env=env, check=True)
        with open(base + '.gif', 'rb') as f:
            src = f.read()
        start = time.time()
        r = multipart('/upload', [('file', src, 'long.gif')])
        took = time.time() - start
        check(r.status == 200, f'300-frame upload: {r.status} {r.text}')
        gif, tr, _ = last()
        check(tr == trailer(False, 300, 300 * 30), f'300-frame trailer {tr}')
        ok, out = go_verify(gif, base)
        check(ok, f'300-frame GIF vs Go: {out}')
        print(f'  300 frames: {len(src)} bytes in, {len(gif)} bytes to the panel, {took:.1f} s in QEMU')
    s = get('/api/status').json()
    check(s['scratch']['used'] < 64 * 1024, f'scratch not cleaned: {s["scratch"]["used"]} bytes used')


def test_status():
    s = get('/api/status').json()
    check(s['panel']['connected'] and s['wifi']['state'] == 'connected' and s['scratch']['total'] > 2000000,
          f'status {s}')
    check(s['free_heap'] > 50000, f'free heap {s["free_heap"]}')
    print(f'  heap: free {s["free_heap"]}, lowest {s["min_free_heap"]}, largest block {s["largest_block"]}')


def test_stream():
    import websockets

    async def run():
        host = BASE.split('://', 1)[1]
        s0 = stats()
        async with websockets.connect(f'ws://{host}/stream?fps=5', max_size=None) as ws:
            frame = fixture('png_small.png')
            for _ in range(10):
                await ws.send(frame)
                await asyncio.sleep(0.05)
            await ws.send('fps=2')
            await asyncio.sleep(2.5)
            # The playlist is blocked while a stream is open.
            loop = asyncio.get_running_loop()
            r = await loop.run_in_executor(None, lambda: multipart('/upload', [('file', frame, 'a.png')]))
            check(r.status == 409, f'upload while streaming: {r.status}')
            for p in ('/clear', '/resume'):
                r = await loop.run_in_executor(None, lambda: get(p))
                check(r.status == 409, f'{p} while streaming: {r.status}')
            s1 = stats()
            sent = s1['uploads'] - s0['uploads']
            # Newest-frame-only: fewer sends than frames, at least one.
            check(1 <= sent < 10, f'{sent} stream frames sent for 10 received')
            print(f'  stream: {sent} of 10 frames sent')
        # Resume after the delay (1.5 s), then the playlist works again.
        await asyncio.sleep(0.5)
        check(stats()['resumes'] == s1['resumes'], 'resumed before the delay')
        await asyncio.sleep(2.5)
        check(stats()['resumes'] == s1['resumes'] + 1, 'playlist not resumed after the stream')
        r = await asyncio.get_running_loop().run_in_executor(
            None, lambda: multipart('/upload', [('file', fixture('png_small.png'), 'a.png')]))
        check(r.status == 200, f'upload after the stream: {r.status}')

    asyncio.run(run())


def test_restart_keeps_settings():
    get('/brightness/77')
    post_json('/api/config', {'stream_fps': 2})
    request('POST', '/api/restart')
    time.sleep(3)
    for _ in range(60):
        try:
            if get('/health').status == 200 and get('/api/status').json()['panel']['connected']:
                break
        except OSError:
            pass
        time.sleep(1)
    check(get('/health').json()['config']['stream_fps'] == 2, 'settings lost across a restart')
    check(stats()['brightness'] == 77, f'brightness not restored after restart: {stats()["brightness"]}')
    post_json('/api/config', {'stream_fps': 4})


def wait_up():
    for _ in range(120):
        try:
            if get('/api/status').json()['panel']['connected']:
                return True
        except (OSError, ValueError):
            pass
        time.sleep(1)
    return False


def main():
    if not wait_up():
        print('firmware not reachable at', BASE)
        return 2
    tests = [test_pages_and_health, test_upload_animation, test_upload_large_animation, test_upload_still,
             test_passthrough, test_display_json, test_playlist, test_live, test_commands, test_brightness,
             test_upload_test, test_config_api, test_long_animation, test_status, test_stream, test_restart_keeps_settings]
    for t in tests:
        before = len(failures)
        start = time.time()
        try:
            t()
        except Exception as e:  # report and go on with the next test
            failures.append(f'{t.__name__}: {e!r}')
            print('  ERROR', repr(e))
        print(f'{"FAIL" if len(failures) > before else "ok  "} {t.__name__} ({time.time() - start:.1f} s)')
    print(f'{len(tests)} tests, {len(failures)} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
