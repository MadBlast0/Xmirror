# Low-latency configuration (known good)

This records a configuration verified working on Windows 11 with low audio and
video latency over Wi-Fi. **Treat it as a baseline to protect, not a starting
point to tune.** The flags below were chosen together; changing one in
isolation is the usual way latency regresses.

Verified: 26 August 2026, x64.

## The configuration

```
-n XMirror -nh -vd d3d11h264dec -vc d3d11convert -vs d3d11videosink -as wasapi2sink -al 0.05 -vsync 0
```

This is both the built-in default (`kDefaultArguments` in
[`src/mainwindow.cpp`](../src/mainwindow.cpp)) and the contents of the active
`arguments.txt`. They are intentionally identical — if `arguments.txt` is ever
deleted, the app falls back to exactly the same line.

### Why each flag matters

| Flag | Purpose | If you change it |
|---|---|---|
| `-vd d3d11h264dec` | GPU H.264 decode | The default `decodebin` falls back to `avdec_h264` (libav, CPU-only). **This is the single biggest source of added latency and CPU load.** |
| `-vc d3d11convert` | GPU colour conversion | A CPU converter forces a GPU→CPU→GPU round trip per frame |
| `-vs d3d11videosink` | Zero-copy GPU render | Frames stay on the GPU. `glimagesink` costs an extra copy; D3D9 is dead in GStreamer |
| `-as wasapi2sink` | Low-latency audio out | `wasapi2` is the newer, lower-latency WASAPI implementation. `wasapisink` (v1) is also bundled but is the slower path |
| `-al 0.05` | 50 ms audio buffer | Replaces XMirror's 250 ms default. Below ~0.03 tends to glitch |
| `-vsync 0` | A/V sync trim, in ms | Accepts negatives (`-vsync -40`). Read at startup only |
| `-n` / `-nh` | Name shown to clients, without `@hostname` | Cosmetic only |

The three `d3d11` flags are a set. Keeping frames on the GPU end to end is what
makes this fast; mixing a GPU decoder with a CPU sink reintroduces the readback
the set exists to avoid.

## Verified build environment

The manifest at `release/resources/build-manifest.json` records this per build.

| Component | Version |
|---|---|
| Qt | 6.11.2 |
| GStreamer (base / good / bad / libav) | 1.28.6 |
| gcc (UCRT64) | 16.2.0 |
| CMake | 4.4.2 |
| MSYS2 Python | 3.14.7 |
| Beacon Python (native) | 3.13.14 |

Built with:

```powershell
.\build.ps1 package -Architecture x64 -SkipInstaller -BeaconPython "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe"
```

`-BeaconPython` is needed when another Python (a stray venv) shadows the real
interpreter on `PATH`. Drop `-SkipInstaller` to also produce the MSI, which
requires the .NET 8 SDK.

## Runtime prerequisites

### Bonjour Service — required for discovery

A Windows service named `Bonjour Service` must be installed and running, or
clients will never see this PC.

`libxmirror` loads `dnssd.dll` and calls `DNSServiceRegister()`
([`libxmirror/lib/dnssd.c`](../libxmirror/lib/dnssd.c)). That DLL is only a client
stub — it forwards over IPC to the Bonjour daemon and performs no mDNS itself.
With no daemon, registration fails silently and the PC does not advertise.

The app detects this at startup and offers to install it. Answering **Yes** runs
the bundled `mDNSResponder.exe -install` (Apple's daemon, built from source by
[`scripts/build-bonjour.ps1`](../scripts/build-bonjour.ps1)); nothing is
downloaded. It requires admin, so expect a UAC prompt.

Note: `--self-test` passing does **not** mean discovery works. It verifies
`dnssd.dll` loads, not that the service behind it is running. Check the service
directly:

```powershell
Get-Service 'Bonjour Service'
```

### Firewall

Allow the app on **Private** networks. A denied or dismissed firewall prompt
looks identical to a broken app: no device ever appears.

Client and PC must also be on the same subnet. Many routers block mDNS between
wired and wireless clients, and across guest or IoT networks.

## Where configuration lives

Read once at startup, in this order — the first that exists wins:

1. `%ProgramData%\XMirror\arguments.txt` — machine-wide
2. `%APPDATA%\MadBlast\XMirror\arguments.txt` — per user
3. Built-in default (identical to the line above)

Environment variables in the file are expanded, so `-n %COMPUTERNAME%` works.

**There is no runtime reload.** Editing the file requires restarting the app.

The GUI post-processes the parsed arguments in
`MainWindow::applyRendererAndFullscreenArgs()`: it injects `-fs` from the
fullscreen checkbox and overrides `-vs` when the renderer combo is set to
D3D11/D3D12. On **Auto** it leaves `-vs` from `arguments.txt` alone — which is
why Auto is the correct setting for this configuration.

## Hardware or software?

**Hardware, for the parts that matter.** This app is a *receiver*: it decodes
video, it never encodes it. Encoding happens on the iPhone or Mac.

| Stage | Where it runs | Element |
|---|---|---|
| H.264 decode | **GPU** — Direct3D 11 video acceleration | `d3d11h264dec` |
| Colour conversion | **GPU** | `d3d11convert` |
| Presentation to screen | **GPU**, zero-copy | `d3d11videosink` |
| Audio decode (ALAC/AAC) | CPU | built in |
| Audio output | CPU → WASAPI | `wasapi2sink` |

"Direct3D 11" is the Windows API for talking to the GPU, so every `d3d11*`
element above is hardware-accelerated. The frame is decoded on the GPU, converted
on the GPU, and drawn from GPU memory — it is never copied back to system RAM.
That round trip is what the configuration exists to avoid.

Audio decode stays on the CPU, which is fine: ALAC and AAC are cheap compared to
video, and no GPU path would help.

### The software fallback

`libgstlibav.dll` is bundled, which provides `avdec_h264` — a pure CPU decoder.
It is the fallback if the D3D11 decoder fails to load. If you ever see it in the
logs, hardware decode is **not** happening and latency and CPU load will both be
noticeably worse.

### Dual-GPU machines

This machine has two GPUs: AMD Radeon integrated graphics and an NVIDIA GeForce
RTX 3060 Laptop. D3D11 uses whichever adapter Windows hands the process,
normally the one driving the display. Either will decode H.264 in hardware, so
both are acceptable — but if latency ever regresses without a config change,
a GPU preference switch in Windows Graphics Settings is worth checking.

Note that NVIDIA's dedicated decoder elements (`nvh264dec`, via the `nvcodec`
plugin) are **not** bundled — no CUDA. `d3d11h264dec` is the hardware path here,
and it works on both adapters.

## Video quality

With the configuration above:

- **Requested resolution:** 1920×1080 @ 60 — XMirror's default when `-s` is absent
- **Codec:** H.264 only. `-h265` is not enabled, and the bundled plugin set has
  no HEVC decoder, so **4K is not available**
- **Frame rate cap:** 30 fps — XMirror's `-fps` default

`-s` is a *request*, not a guarantee. The client decides what it actually sends,
and an iPhone mirrors at its own aspect ratio (roughly 19.5:9), so the picture is
pillarboxed inside a 16:9 target. Raising `-s` does not add detail the client
never sent, and asking for more than the client wants to encode can cost latency.

To see the resolution actually negotiated, launch from a terminal — the app
attaches to a parent console and logs the stream dimensions on connect:

```powershell
.\release\XMirror.exe
```

## Do not change without measuring

Highest to lowest risk of losing the current latency:

1. **`-vd`, `-vc`, `-vs`** — the GPU path. A wrong value here means a CPU
   fallback, a black window, or a failure to start
2. **`-al` below 0.03** — audio begins to glitch
3. **`-h265` / 4K** — no HEVC decoder is bundled; expect no picture
4. **Renderer combo set away from Auto** — overrides the tuned `-vs`
5. **`-fps` above 30** — more frames to encode, decode and present

Safe to change freely: `-n`, `-nh`, and `-vsync` (which exists precisely to be
adjusted, in ms, positive or negative).

## If latency regresses

1. Confirm `arguments.txt` still matches the line at the top of this document
2. Confirm the renderer combo is on **Auto** — it silently overrides `-vs`
3. Confirm the GPU decoder is actually in use. `avdec_h264` in the logs means
   the D3D11 decoder failed to load and it fell back to CPU
4. Confirm the plugin DLLs are present: `libgstd3d11.dll` and
   `libgstwasapi2.dll` in `lib\gstreamer-1.0`
5. On a dual-GPU laptop, confirm Windows is giving the app the discrete GPU
6. Re-run `.\release\XMirror.exe --self-test` (exit code 0 = bundle complete)
