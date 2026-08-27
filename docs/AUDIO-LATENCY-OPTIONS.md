# Audio latency — analysis and options

Written after the 26 August 2026 session. Video is solved (144 ms → 38 ms).
Audio is not, and this records everything established about it, why the current
configuration trades quality for latency, and what to try next.

**Target: audio under 100 ms.** Reference point: DouWan measures 133 ms audio on
the same contaminated scale (see §4).

---

## 1. The central trade-off

Everything about audio comes down to one property on the sink.

| | `sync=false` | `sync=true` |
|---|---|---|
| Behaviour | Play each packet on arrival | Play at the timestamp the client scheduled |
| Measured latency | **171-180 ms** | **351-360 ms** |
| Clock discipline | **None** | Full |
| Audio quality | Degrades — clicks, drift | Clean |
| XMirror default | Pre-1.64 (abandoned) | 1.64+ (current) |

Upstream **deliberately moved from `sync=false` to `sync=true` in 1.64** because
the old method relied on GStreamer's internal clock and suffered drift. Running
`-vsync no` puts us back on the method upstream abandoned.

That is almost certainly the cause of the reported audio quality problem: the
iPad's audio clock and the PC's sound card clock are never exactly equal, and
with no discipline the buffer slowly under- or over-runs.

**So `-vsync no` is not a viable destination.** It buys ~180 ms at the cost of
the thing that keeps audio clean.

### Why `-vsync no` also affects audio at all

`renderers/audio_renderer.c:180` — in mirroring mode (AAC) the audio sink's
`sync` follows the **video** flag:

```c
default:                       /* AAC, i.e. mirroring mode */
    if (*video_sync) {
        g_string_append (launch, " sync=true");
    } else {
        g_string_append (launch, " sync=false");
    }
```

There is no way to set them independently from the command line. Decoupling
requires a code change (§5, option C).

---

## 2. Ruled out, with evidence

Each of these was applied and measured. **None changed audio latency.** The
built pipeline string was read from the engine log to confirm the settings were
actually in effect, rather than inferred from behaviour.

| Hypothesis | Outcome |
|---|---|
| `buffer-time` 200 ms → 30 ms | 360 → 351 ms. No effect. |
| `low-latency=true` | No effect. |
| `processing-deadline` 20 ms → 0 | No effect. |
| Bounding the audio `queue` | 171 → 180 ms. No effect, and **reverted** — it risks underruns for no gain, and likely contributed to the quality problem. |
| `DELAY_AAC` 0.20 → 0.02 (`raop_rtp.c:41`) | No effect. Only biases the *pre-sync estimate*; once the first real RTP sync arrives it is overwritten. **Reverted.** |
| RAOP jitter buffer (`raop_buffer.c`) | Inspected. With `no_resend` it *"always returns the first entry"* — no fill threshold, no steady-state delay. |
| `-al 0.05` | Advertised to the client as an `Audio-Latency` header (`raop_handlers.h:1207`), but **the client ignores it** — documented upstream, and consistent with our measurements. |
| WASAPI `exclusive=true` | **Stutters.** Also blocks other applications' audio, and makes the probe unable to measure at all (loopback taps the shared mixer). |

The confirmed pipeline, for reference:

```
appsrc ! queue ! avdec_aac ! audioconvert ! audioresample ! volume ! level !
wasapi2sink low-latency=true buffer-time=30000 processing-deadline=0 sync=true
```

---

## 3. Where the 180 ms difference actually comes from

With `sync=true` the sink honours the playback time the **client** scheduled.
AirPlay deliberately sends audio ahead of its playback moment so receivers can
absorb jitter. Honouring that schedule *is* the latency.

This is protocol behaviour, not a bug in the receiver. It is also why none of the
local buffer settings in §2 moved the number — they are all downstream of a
decision the sender already made.

`ts-offset` on the audio sink was tried at −250 ms to render earlier than
scheduled. **Audio stopped entirely** — it shifts past the point where data is
available and starves the sink.

An untested middle ground remains: −100 ms or −150 ms may sit inside the
available lead. See §5, option A.

---

## 4. The measurement is contaminated — read this before tuning

`tools/sync-probe` emits its beeps from **Safari's Web Audio** on the iPad. That
adds an unknown, constant delay *inside the iPad*, before AirPlay sees the
sound, and it is not part of the path normal audio takes.

Consequences:

- **Absolute audio numbers are inflated** by an unknown amount.
- The constant is at most ~133 ms, or DouWan could not have measured that low.
- **Comparisons between apps are valid**; absolutes are not.
- The probe's `suggested XMirror trim` line is derived from the contaminated
  desync and must be ignored.

Video has no equivalent problem: it is measured optically off the screen.

**Do not chase a 20-40 ms audio difference with this tool.** It is inside the
error bars. Before any further audio tuning, get a clean number by capturing the
engine's own arrival latency:

```
# add "-d 1" to arguments.txt, then:
XMirror.exe --log engine.log
# look for: "raop_rtp audio: ... latency = ..."   (raop_rtp.c:639)
```

That measures client-capture → receiver-arrival with no Safari involved, and
splits the budget into "iPad and network" versus "our pipeline" — exactly as it
did for video, where it proved the network was innocent.

**Known flaw:** `--log` opens the file exclusively, so it cannot be read while
the app runs. Fix that first, or stop the app to read the log.

---

## 5. Options, best first

### A. `sync=true` with a tuned audio `ts-offset`  — most promising

Keeps clock discipline (and therefore audio quality) while rendering earlier
than the client scheduled.

```
-as "wasapi2sink low-latency=true processing-deadline=0 ts-offset=-100000000"
```

Test −100 ms, then −150 ms, then −200 ms. Stop at the largest value that does
**not** produce dropouts. −250 ms is known to starve it.

Expected: ~200 ms with clean audio. Probably not under 100 ms, but it is the
only option that improves latency without sacrificing quality.

This is exactly the mechanism that fixed video, where `ts-offset=-100000000`
took it from 144 ms to 38 ms.

### B. Accept `sync=false` and fix the quality separately

Currently gives ~171-180 ms. The quality cost comes from unmanaged clock drift.
Possible mitigations, none tested:

- `slave-method` on the sink (`resample`, `skew`, `none`) to compensate for drift
- Restoring a generous `buffer-time` — with `sync=false` a larger buffer costs
  little latency but absorbs jitter, which is the opposite of the usual trade

### C. Decouple audio sync from video sync in code

`audio_renderer.c:180` ties them together. Splitting them would allow
`sync=false` on video (fast) with `sync=true` on audio (clean) — or the reverse.

Note this **worsens A/V alignment**, since the two streams would then be
scheduled by different rules. Useful for diagnosis, questionable as a shipped
default.

### D. Accept the current position

Audio 341 → 171 ms is already a halving. If the residual desync is not audible
on real content, the remaining gap may be mostly measurement artifact (§4).

---

## 6. What was rejected and why

- **Reverse-engineering DouWan.** Copying implementation from a paid competitor's
  binaries would put proprietary code into a GPLv3 project. Black-box
  measurement of it, by contrast, has been genuinely valuable and is what gave
  us the 49 ms / 133 ms reference.
- **WASAPI exclusive mode.** Stutters, silences other applications, and defeats
  measurement.
- **Chasing `-al`.** The client ignores it. Documented upstream and confirmed.

---

## 7. Current state

```
-n XMirror -nh -vd d3d11h264dec -vc d3d11convert
-vs "d3d11videosink processing-deadline=0 fullscreen-toggle-mode=GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_ALT_ENTER"
-as wasapi2sink -al 0.05 -vsync no
```

Video ~38-49 ms, audio ~171-180 ms, audio quality degraded by `-vsync no`.

The audio queue bounding has been reverted; `DELAY_AAC` is back to upstream.

**Recommended next step:** switch to `-vsync 0` and work through option A. That
restores audio quality immediately, at the cost of latency, and then buys the
latency back incrementally with a measurable, revertible knob.

---

## 8. 27 August session — what changed and what closed

Measured with `tools/sync-probe` on a freshly restarted probe (a stale rolling
history silently contaminates the numbers; restart it between configurations).

**Baseline reconfirmed:** video ~43 ms, audio ~185 ms, desync ~-140 ms.

### Newly ruled out

| Hypothesis | Outcome |
|---|---|
| `buffer-time` under `sync=false` | No effect. Section 2 only tested it under `sync=true`; both modes are now closed. `low-latency=true` makes wasapi2sink take the IAudioClient3 minimum period and ignore `buffer-time` regardless. |
| **Removing the audio `queue` entirely** | **Worse: 185 -> 205-213 ms.** Without the thread boundary the blocking sink write back-pressures the RTP receive thread. Bounding it gave nothing (section 2); removing it costs. The upstream default is the best of the three. Reverted. |
| `audioLatencies` / `outputLatencyMicros` | Dead end. `raop_handlers.h:173` sends `outputLatencyMicros` as a *boolean* rather than an integer, and both entries are hardcoded zero -- but DouWan's `/info` advertises byte-identical zeros. Not the differentiator. |

### Removed as free wins

- **`level` element.** It computed RMS/peak per buffer and posted a bus message
  per interval that the handler discards (`GST_MESSAGE_ELEMENT: break`). Video
  improved ~47 -> ~43 ms; both pipelines share the GLib main loop. Restore it
  only if a VU meter is added.

### DouWan, measured black-box

Queried its `/info` over RTSP on port 47011 (no disassembly):

- `model` = `AppleTV3,2`, `sourceVersion` = `220.68` -- **identical to our
  `global.h` constants.** Same protocol lineage; there is no handshake to copy.
- `audioLatencies` identical to ours.
- Differs only in `features` (high word `0xE` vs our `0x0`) and a `displays`
  entry with `maxFPS: 30`. Neither is audio-related.
- Architecture from its imports: FFmpeg 7 decode + `SDL_OpenAudioDevice` /
  `SDL_QueueAudio`. Play-on-arrival, i.e. functionally our `sync=false`.

The `obs-airplay-receiver` project is a Windows port of `mika314/obs-airplay`
and is **built on UxPlay's own library** -- not an independent implementation.

### Still untested

**Option A (section 5) was never actually measured.** It was configured, then
the app was restarted for a different test before a reading was taken. It
remains the only open lever, and it is the one that also restores audio quality:
`sync=true` with the audio sink at `ts-offset=-200000000`. Ceiling is roughly
351 - 200 = ~150 ms; `-250 ms` is known to starve the sink.

### Shipped default after this session

```
-n XMirror -nh -vd d3d11h264dec -vc d3d11convert
-vs "d3d11videosink processing-deadline=0 ts-offset=-100000000 fullscreen-toggle-mode=..."
-as "wasapi2sink low-latency=true processing-deadline=0" -al 0.05 -vsync no
```

Chosen because it is the configuration actually measured. `-vsync no` trades
clock discipline for ~166 ms; set `-vsync 0` in `arguments.txt` if drift appears.
