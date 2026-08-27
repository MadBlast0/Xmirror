"""XMirror sync probe.

Measures three numbers while the iPad mirrors a page served by this script:

  video latency  iPad frame render -> that frame visible on the PC
  audio latency  iPad beep emission -> that beep audible in the PC's output
  A/V desync     video latency - audio latency
                 (positive = picture lags sound on the PC)

Video uses a 16-bit binary timestamp painted into the page on every displayed
frame, decoded back out of a screen capture of the video window. Audio uses a
1 kHz beep emitted on a 2 s grid of the *PC's* clock, so a detected onset maps
back to its emission time without having to carry a payload. The iPad's clock
is aligned to the PC's over HTTP before either measurement starts.
"""

from __future__ import annotations

import argparse
import ctypes
import http.server
import json
import pathlib
import socket
import statistics
import sys
import threading
import time
from ctypes import wintypes

import mss
import numpy as np
import soundcard as sc

HERE = pathlib.Path(__file__).resolve().parent

NPATCH, NBITS = 20, 16
WRAP = 1 << NBITS               # timestamp wraps every 65.536 s
TONE_HZ, GRID_MS = 1000.0, 2000.0
DEFAULT_TITLE = "AirPlay Video Stream"

# One shared epoch for every timestamp in this process. time.time() is sampled
# once; everything after is perf_counter deltas, which never jump.
_T0_WALL, _T0_PERF = time.time(), time.perf_counter()


def now_ms() -> float:
    return (_T0_WALL + (time.perf_counter() - _T0_PERF)) * 1000.0


# ---------------------------------------------------------------------------
# HTTP server: the page, plus the clock-sync endpoint
# ---------------------------------------------------------------------------
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):          # keep the console readable
        pass

    def _send(self, body: bytes, ctype: str):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/t"):
            self._send(json.dumps({"t": now_ms()}).encode(), "application/json")
            return
        self._send((HERE / "probe.html").read_bytes(), "text/html; charset=utf-8")


def lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


# ---------------------------------------------------------------------------
# Locating the video window
# ---------------------------------------------------------------------------
user32 = ctypes.windll.user32
user32.SetProcessDPIAware()
EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def find_windows(match: str | None = None) -> list[tuple[int, str]]:
    found: list[tuple[int, str]] = []

    def cb(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            if n:
                buf = ctypes.create_unicode_buffer(n + 1)
                user32.GetWindowTextW(hwnd, buf, n + 1)
                if match is None or match.lower() in buf.value.lower():
                    found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(EnumProc(cb), 0)
    return found


def client_rect(hwnd: int) -> dict | None:
    # A minimized window is parked off-screen at -32000 with a zero-size client
    # area; capturing it yields nothing useful.
    if user32.IsIconic(hwnd):
        return None
    r = wintypes.RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(r)):
        return None
    pt = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    w, h = r.right - r.left, r.bottom - r.top
    if w < 64 or h < 64:
        return None
    return {"left": pt.x, "top": pt.y, "width": w, "height": h}


# ---------------------------------------------------------------------------
# Video: decode the binary timestamp out of a captured frame
# ---------------------------------------------------------------------------
def page_rect(img: np.ndarray) -> tuple[int, int, int, int] | None:
    """Bounding box of the page's magenta border.

    Anchoring on magenta rather than brightness means Safari's own toolbar and
    the black letterbox bars are both ignored, so this is the page viewport
    exactly -- no assumption that the page fills the mirrored screen.
    """
    r, g, b = img[:, :, 2], img[:, :, 1], img[:, :, 0]
    mask = (r > 150) & (b > 150) & (g < 100)
    rows = np.flatnonzero(mask.any(axis=1))
    cols = np.flatnonzero(mask.any(axis=0))
    if rows.size < 4 or cols.size < 4:
        return None
    y0, y1, x0, x1 = int(rows[0]), int(rows[-1]), int(cols[0]), int(cols[-1])
    if (y1 - y0) < 80 or (x1 - x0) < 80:
        return None
    return x0, y0, x1, y1


def decode_frame(gray: np.ndarray, box: tuple[int, int, int, int]) -> int | None:
    """Read the 16-bit timestamp out of the patch band."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0

    # Band occupies 4%..96% horizontally, 4.5%..15.5% vertically. Sample the
    # inner half of each cell so encoder ringing at the edges cannot bleed in.
    by0 = y0 + int(0.075 * h)
    by1 = y0 + int(0.125 * h)
    if by1 - by0 < 2:
        return None

    span = 0.92 * w / NPATCH
    means = np.empty(NPATCH)
    for i in range(NPATCH):
        cx0 = x0 + int(0.04 * w + (i + 0.25) * span)
        cx1 = x0 + int(0.04 * w + (i + 0.75) * span)
        if cx1 - cx0 < 1:
            return None
        means[i] = gray[by0:by1, cx0:cx1].mean()

    # Patches 0/18 are always white and 1/19 always black, giving a per-frame
    # reference so the threshold survives any gamma or colour-range shift the
    # encode/decode path introduces.
    hi = (means[0] + means[18]) / 2.0
    lo = (means[1] + means[19]) / 2.0
    if hi - lo < 25:
        return None
    thr = (hi + lo) / 2.0

    v = 0
    for b in range(NBITS):
        v = (v << 1) | int(means[2 + b] > thr)
    return v


class VideoProbe(threading.Thread):
    daemon = True

    def __init__(self, title: str):
        super().__init__()
        self.title = title
        self.samples: list[float] = []
        self.lock = threading.Lock()
        self.state = "looking for the video window"
        self.stop_flag = threading.Event()

    def run(self):
        with mss.MSS() as sct:
            rect = None
            recheck = 0.0
            while not self.stop_flag.is_set():
                if time.perf_counter() > recheck:
                    recheck = time.perf_counter() + 1.0
                    wins = find_windows(self.title)
                    rect = client_rect(wins[0][0]) if wins else None
                if rect is None:
                    wins = find_windows(self.title)
                    self.state = ("video window is minimized -- restore it"
                                  if wins else
                                  "video window not found (is mirroring active?)")
                    time.sleep(0.25)
                    continue

                t = now_ms()
                try:
                    raw = sct.grab(rect)
                except Exception:
                    rect = None
                    continue
                img = np.asarray(raw)[:, :, :3].astype(np.float32)

                if float(img.max()) < 12:
                    self.state = ("capture is black -- the sink is bypassing the "
                                  "compositor (try the D3D12 renderer, or windowed)")
                    time.sleep(0.1)
                    continue

                box = page_rect(img)
                if box is None:
                    self.state = "video window found, but the probe page is not visible"
                    time.sleep(0.05)
                    continue

                v = decode_frame(img.mean(axis=2), box)
                if v is None:
                    self.state = "page found, but the marker band is unreadable"
                    time.sleep(0.05)
                    continue

                self.state = "ok"
                with self.lock:
                    self.samples.append((t - v) % WRAP)
                    if len(self.samples) > 600:
                        del self.samples[:-600]

    def take(self) -> list[float]:
        with self.lock:
            s, self.samples = self.samples[:], []
        return s


# ---------------------------------------------------------------------------
# Audio: detect the 1 kHz beep in the PC's own output
# ---------------------------------------------------------------------------
class AudioProbe(threading.Thread):
    daemon = True
    SR = 48000
    BLOCK = 480                 # 10 ms
    SMOOTH = 192                # 4 ms envelope window

    def __init__(self):
        super().__init__()
        self.samples: list[float] = []
        self.lock = threading.Lock()
        self.state = "starting audio capture"
        self.stop_flag = threading.Event()

    def run(self):
        try:
            spk = sc.default_speaker()
            mic = sc.get_microphone(str(spk.name), include_loopback=True)
        except Exception as e:
            self.state = f"loopback capture unavailable: {e}"
            return

        phase = 0.0
        dphi = 2 * np.pi * TONE_HZ / self.SR
        kernel = np.ones(self.SMOOTH) / self.SMOOTH
        tail = np.zeros(0, dtype=np.complex128)
        tail_t: float | None = None
        floor = 1e-4
        last_onset = -1e9

        with mic.recorder(samplerate=self.SR, channels=1, blocksize=self.BLOCK) as rec:
            self.state = "listening"
            while not self.stop_flag.is_set():
                data = rec.record(numframes=self.BLOCK)
                t_end = now_ms()
                x = np.asarray(data, dtype=np.float64).reshape(-1)
                n = x.size
                if n == 0:
                    continue
                t_start = t_end - 1000.0 * n / self.SR

                # Complex demodulation at the beep frequency, so anything that
                # is not near 1 kHz contributes almost nothing to the envelope.
                ph = phase + dphi * np.arange(n)
                phase = float((phase + dphi * n) % (2 * np.pi))
                prod = x * np.exp(-1j * ph)

                if tail_t is None:
                    tail_t = t_start
                buf, buf_t0 = np.concatenate([tail, prod]), tail_t
                if buf.size < self.SMOOTH:
                    tail, tail_t = buf, buf_t0
                    continue

                env = np.abs(np.convolve(buf, kernel, mode="valid"))
                keep = self.SMOOTH - 1
                tail = buf[-keep:]
                tail_t = buf_t0 + 1000.0 * (buf.size - keep) / self.SR

                floor = 0.95 * floor + 0.05 * max(float(np.median(env)), 1e-6)
                thr = max(floor * 8.0, 2e-3)

                hits = np.flatnonzero(env > thr)
                if hits.size:
                    i = int(hits[0])
                    # -SMOOTH/2 undoes the group delay of the box envelope.
                    onset = buf_t0 + 1000.0 * (i - self.SMOOTH / 2) / self.SR
                    if onset - last_onset > 900.0:
                        last_onset = onset
                        lat = onset - (np.floor(onset / GRID_MS) * GRID_MS)
                        with self.lock:
                            self.samples.append(float(lat))
                            if len(self.samples) > 120:
                                del self.samples[:-120]

    def take(self) -> list[float]:
        with self.lock:
            s, self.samples = self.samples[:], []
        return s


# ---------------------------------------------------------------------------
def fmt(vals: list[float]) -> str:
    if not vals:
        return "--"
    m = statistics.median(vals)
    spread = (max(vals) - min(vals)) / 2 if len(vals) > 1 else 0.0
    return f"{m:5.0f}ms +/-{spread:3.0f}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--title", default=DEFAULT_TITLE)
    ap.add_argument("--duration", type=float, default=0.0, help="0 = until Ctrl-C")
    ap.add_argument("--list-windows", action="store_true")
    args = ap.parse_args()

    if args.list_windows:
        for hwnd, t in find_windows():
            print(f"{hwnd:>10}  {t}")
        return 0

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    url = f"http://{lan_ip()}:{args.port}/"
    print("=" * 70)
    print("  XMirror sync probe")
    print("=" * 70)
    print(f"  1. On the iPad, open Safari at:  {url}")
    print("  2. Tap once (iOS requires a gesture before it will play audio)")
    print("  3. Start Screen Mirroring to XMirror")
    print("  4. Keep the page in the foreground; set Auto-Lock to Never")
    print("=" * 70)
    print()
    print(f"{'video':>16} {'audio':>16} {'A/V desync':>14}   status")

    vp, apr = VideoProbe(args.title), AudioProbe()
    vp.start()
    apr.start()

    vhist: list[float] = []
    ahist: list[float] = []
    t_end = time.perf_counter() + args.duration if args.duration else None

    try:
        while True:
            time.sleep(2.0)
            vhist = (vhist + vp.take())[-400:]
            ahist = (ahist + apr.take())[-60:]

            if vhist and ahist:
                d = statistics.median(vhist) - statistics.median(ahist)
                desync = f"{d:+6.0f}ms"
                note = ("in sync" if abs(d) < 25
                        else "picture lags sound" if d > 0 else "sound lags picture")
            else:
                desync = "--"
                note = vp.state if not vhist else apr.state

            print(f"{fmt(vhist):>16} {fmt(ahist):>16} {desync:>14}   {note}")

            if t_end and time.perf_counter() > t_end:
                break
    except KeyboardInterrupt:
        pass

    print()
    if vhist:
        print(f"  video latency  median {statistics.median(vhist):6.0f} ms  (n={len(vhist)})")
    if ahist:
        print(f"  audio latency  median {statistics.median(ahist):6.0f} ms  (n={len(ahist)})")
    if vhist and ahist:
        d = statistics.median(vhist) - statistics.median(ahist)
        print(f"  A/V desync     {d:+6.0f} ms  "
              f"({'picture lags sound' if d > 0 else 'sound lags picture'})")
        print()
        print(f"  suggested XMirror trim:  -vsync {-round(d / 10) * 10}")
    print()
    print("  Accuracy is roughly +/-25 ms absolute: screen capture quantises to the")
    print("  refresh interval, and WASAPI loopback adds a small fixed buffer. The")
    print("  desync figure is the one to act on; the absolutes are indicative.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
