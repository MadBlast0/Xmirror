# Latency investigation — session log

A full record of the 26 August 2026 session: how mirroring latency was measured,
what was found, what worked, what did not, and what is still open.

**Headline result: video glass-to-glass latency went from 144 ms to 38 ms, with
audio/video clock sync left intact.** Audio is unresolved and is the open item.

---

## 1. Starting point

The app was already running a tuned low-latency configuration:

```
-n XMirror -nh -vd d3d11h264dec -vc d3d11convert -vs d3d11videosink
-as wasapi2sink -al 0.05 -vsync 0
```

The complaint was that mirroring felt noticeably delayed compared with a
commercial app (DouWan), which felt instant.

Two guesses were wrong and are recorded here because they shaped the early work:

- **"It's your Wi-Fi."** It was not. See §3.
- **"80-120 ms is the physical floor for wireless AirPlay, and apps claiming
  10-15 ms must be wired."** Also wrong. DouWan measured **49 ms** over the same
  Wi-Fi to the same iPad. Being wrong about this mattered: it nearly stopped the
  investigation before it started.

---

## 2. How latency was measured

`tools/sync-probe/probe.py`, already in the repo. It measures:

- **Video latency** — a 16-bit timestamp is painted into a page displayed on the
  iPad. The PC screen-captures the mirror window and decodes that timestamp back
  out. The difference is true glass-to-glass delay.
- **Audio latency** — 1 kHz beeps emitted on a 2 s grid of the *PC's* clock,
  detected through WASAPI loopback.

The iPad's clock is aligned to the PC's over HTTP before measuring.

### Running it

```powershell
# XMirror must be running and mirroring; probe page open on the iPad
python -u tools/sync-probe/probe.py --duration 110
python -u tools/sync-probe/probe.py --title "DouWan (Window Capture)" --duration 110
python tools/sync-probe/probe.py --list-windows      # find a window title
```

Dependencies: `mss`, `numpy`, `soundcard` (installed into native CPython 3.13,
not the MSYS2 one).

### Pitfalls that cost time

| Symptom | Cause |
|---|---|
| `video window found, but the probe page is not visible` | The probe screen-captures a **screen region**. Anything overlapping the mirror window, or Safari not being foreground on the iPad, produces garbage. |
| `video window not found` | The mirror window was not titled `AirPlay Video Stream`. See the D1 bug in `SETTINGS-WINDOW-PLAN.md` — before it was fixed, the window was never retitled on the Auto renderer, so this probe could not find it at all. |
| Nonsense readings (20 s, 60 s medians) | Stale clock alignment. Reload the probe page on the iPad after any app restart. |
| No output when run in background | Python buffers stdout when not attached to a TTY. Use `python -u`. |

### The audio number is not trustworthy in absolute terms

The beeps come from **Safari's Web Audio** on the iPad, which adds its own output
delay *before* AirPlay sees the sound. That inflates every audio reading by a
constant, and it is not part of the path normal audio takes.

The constant is identical for every app measured, so **comparisons are valid and
absolutes are not**. DouWan's 133 ms is the reference point, not zero.

Do **not** act on the probe's `suggested XMirror trim` line — it is derived from
the contaminated desync figure.

---

## 3. Ruling out the network and the iPad

`--log <path>` was added to the app (see §6) so the engine's own diagnostics
could be captured. With `-d 1`, XMirror logs the telemetry the iPad sends back:

| iPad-reported metric | Observed |
|---|---|
| `rttAvg` | 7-33 ms |
| `encoderCurrentFPS` | 60 |
| `queuedFramesAvg` | 0 |
| `encoderDropFPS`, `idleDropFPS` | 0 |
| `lossAvg` | ~0 |

The iPad was encoding at a full 60 fps, dropping nothing, with an empty send
queue, over a link with 10-30 ms round trip and no loss.

**Conclusion: neither the network nor the iPad was the bottleneck.** The delay
was inside the receive → decode → present path, which is ours to fix.

---

## 4. The video fix

### Root cause

The GStreamer video pipeline is:

```
appsrc ! queue ! h265parse ! d3d11h265dec ! d3d11convert ! videoscale ! d3d11videosink sync=true
```

`video_renderer_render_buffer()` sets each frame's PTS from the **iPad's NTP
capture timestamp**. With `sync=true`, the sink holds every frame until the
pipeline clock reaches that timestamp.

Frames were arriving roughly **106 ms before their PTS came due**, so the sink
sat on them. The scheduling was wrong by a systematic offset.

### The wrong fix (and why it was wrong)

`-vsync no` sets `sync=false` and made video 38 ms immediately. It was used as a
diagnostic and **should not be used as a setting**, because:

`libxmirror/renderers/audio_renderer.c:180` ties the **audio** sink's `sync` to
the same `video_sync` flag. In mirroring mode (AAC), `-vsync no` produces
`wasapi2sink sync=false` as well — silently removing clock discipline from audio.
Measured audio went 171 → 373 ms and became unstable.

This was not spotted until the actual pipeline string was read out of the log.
**Read the built pipeline before drawing conclusions from behaviour.**

### The right fix

Keep `sync=true` everywhere and cancel the bogus scheduling delay with the sink's
`ts-offset` property:

```
-vs "d3d11videosink processing-deadline=0 ts-offset=-100000000 fullscreen-toggle-mode=GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_ALT_ENTER"
```

- `ts-offset=-100000000` — present each frame 100 ms earlier than scheduled.
- `processing-deadline=0` — the sink defaults to 15 ms, added to live-pipeline
  latency. Free saving.
- The `fullscreen-toggle-mode` must be restated: `xmirror.cpp:3062` only injects
  its Alt+Enter default when `-vs` carries **no** options, so passing any options
  silently loses Alt+Enter fullscreen.

Passing options through `-vs` needs no code change: `xmirror.cpp:1411` splits the
argument at the first space into element plus properties.

### Results

| Configuration | Video latency | A/V sync |
|---|---|---|
| Original `-vsync 0` | 144 ms | intact |
| `-vsync no` (diagnostic only) | 38 ms | **broken** |
| **`-vsync 0` + `ts-offset=-100 ms`** | **38-40 ms** | **intact** |
| DouWan (reference) | 49 ms | — |

n=400 samples on the final measurement. This is now faster than the commercial
app that prompted the investigation.

---

## 5. Audio — still open

Audio remains at ~350 ms and is the current problem.

### What was ruled out, with evidence

Everything below was verified by reading the **actual pipeline string** from the
log, not inferred:

```
appsrc ! queue ! avdec_aac ! audioconvert ! audioresample ! volume ! level !
wasapi2sink low-latency=true buffer-time=30000 processing-deadline=0 sync=true
```

| Hypothesis | Result |
|---|---|
| 200 ms ring buffer (`buffer-time` default) | Applied `buffer-time=30000`. Audio 360 → 351 ms. **Not the cause.** |
| `low-latency=false` | Set to `true`. No material change. |
| `processing-deadline` 20 ms | Set to 0. No material change. |
| `-al 0.05` not reaching the protocol | It does — `xmirror.cpp:2761` sets `audio_delay_micros`, overriding the 250 ms default in `raop.c:637`. |
| Sink options not parsing | They parse. Confirmed in the built pipeline above. |

Note: unlike `-vs`, `-as` is **not** split by XMirror. It works only because the
whole string is appended into the launch line and `gst_parse_launch` accepts
`element prop=value` syntax.

### What failed

Applying `ts-offset=-250000000` to the audio sink — the same trick that fixed
video — **stopped audio playing entirely**. It starves the sink rather than
correcting scheduling. Reverted.

### Remaining candidates

1. The bare **`queue`** in the audio pipeline. Unbounded: GStreamer defaults are
   200 buffers / 10 MB / **1 second**. The video queue was capped during this
   session; the audio one was not.
2. The **RAOP audio jitter buffer** (`raop_buffer.c`), not yet examined.
3. A genuine protocol-level buffer that AirPlay audio requires for smooth
   playback, in which case ~350 ms may be closer to inherent than it appears.

DouWan measures 133 ms audio on the same contaminated scale, so at least
~220 ms of ours looks recoverable.

---

## 6. Supporting changes made this session

| Change | File | Why |
|---|---|---|
| `--log <path>` option | `src/main.cpp` | `main()` reattaches stdout to `CONOUT$`, discarding redirection, so engine diagnostics could not be captured from a GUI-subsystem process. **Known flaw: it opens the file exclusively, so the log cannot be read while the app runs.** |
| Bounded the video `queue` | `libxmirror/renderers/video_renderer.c` | A bare `queue` defaults to 1 second of buffering. Capped to 2 buffers. Deliberately **not** leaky: this queue holds encoded data, so dropping buffers would discard reference frames and corrupt the picture until the next keyframe. |
| Mirror-window adoption fix | `src/mainwindow.cpp` | The fallback poll gave up after 60 s, so connecting later than that left the window untitled — which also broke the sync probe. It now polls while the engine runs and stops once adopted. |
| `processing-deadline=0` on both sinks | `arguments.txt` | 15 ms video, 20 ms audio, added to live-pipeline latency by default. |

---

## 7. Resolution findings

Unrelated to latency but established in the same session.

**HEVC is available and was not being used.** `CLAUDE.md` stated no HEVC decoder
was bundled. That was wrong — `gst-inspect` shows `d3d11h265dec` (hardware, on
the RTX 3060) and `h265parse` both present, because the plugin resolver copies
whole DLLs and `libgstd3d11.dll` contains every d3d11 decoder.

`-h265` needs no decoder reconfiguration: `video_renderer.c:370-378` substitutes
`h264` → `h265` in element names automatically, so `-vd d3d11h264dec` becomes
`d3d11h265dec` for the HEVC pipeline. XMirror builds both pipelines and advertises
the capability; a device that cannot do HEVC simply keeps using H.264.

HEVC roughly halves the bitrate for the same picture.

**Match the client's panel, not a 16:9 preset.** An 11-inch iPad is 2388x1668,
which is 3:2. Requesting a 16:9 size is height-limited and pillarboxed:

| Requested | What the iPad actually delivers |
|---|---|
| 1920x1080 (16:9) | 1620x1080 — downscaled, black bars |
| 2560x1440 (16:9) | 2062x1440 — still downscaled, still bars |
| **2388x1668 (3:2)** | **2388x1668 — 1:1, no scaling, no bars** |

Confirmed in the log: `video format is h265 high definition (HD/4K) video
2388x1668`.

The resolution control in the settings window is therefore an **editable**
combo accepting any `WxH[@R]`, not a fixed list — the useful value is the
client's own panel size, which no preset list can cover.

---

## 8. Current configuration

```
-n XMirror -nh
-vd d3d11h264dec -vc d3d11convert
-vs "d3d11videosink processing-deadline=0 ts-offset=-100000000 fullscreen-toggle-mode=GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_ALT_ENTER"
-as "wasapi2sink low-latency=true buffer-time=30000 processing-deadline=0"
-al 0.05 -vsync 0
```

With HEVC enabled and resolution `2388x1668@60` in the settings window.

Rollback files kept beside `arguments.txt`: `.synced`, `.before-audio`,
`.video-fixed`.

---

## 9. Open items

1. **Audio latency (~350 ms).** Next: cap the audio `queue`, then read
   `raop_buffer.c`.
2. **The two `ts-offset` values are compensation, not a cure.** They paper over a
   systematic error in the NTP timestamp conversion. The proper fix is to correct
   that conversion once, so both sinks schedule correctly with no magic numbers.
   Until then, `-100000000` is tuned to this machine and may not suit others.
3. **`--log` opens its file exclusively** and should use shared access.
4. **Thread count grows across sessions** (41 → 65 → 79 observed). Possibly
   GStreamer pool growth, possibly a leak. Unverified.
5. **`CLAUDE.md` still contains stale claims.** The HEVC one is corrected here
   but not in that file. It also previously claimed `libgstwasapi.dll` was not
   shipped (it is) and that `main.cpp` had a single-instance guard (it did not,
   until this session).
