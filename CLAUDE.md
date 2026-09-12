# XMirror

Windows AirPlay receiver. A Qt6 Widgets system-tray app wrapping the XMirror
AirPlay engine as a static library.

## Architecture

```
src/main.cpp          Qt entry point, single-instance guard, tray bootstrap
src/mainwindow.cpp    608 lines. Tray menu, autostart, arguments.txt loading,
                      mDNS beacon supervision. The bulk of the GUI.
src/fluenttheme.*     "Fluent Quiet" look: Fusion + palette + one tokenised
                      stylesheet, follows Windows light/dark and accent.
                      enableAcrylic() adds the Acrylic backdrop on Win11 22H2+
                      and falls back to solid surfaces elsewhere.
src/fluentswitch.*    Windows 11 toggle (painted QAbstractButton). Colours
                      come from the stylesheet via qproperty-.
src/airplayworker.cpp QThread that calls start_xmirror(argc, argv) from
                      libxmirror and loops until interruption.
src/mdns_responder.*  mDNSResponder beacon subprocess wrapper
libxmirror/            Vendored in-tree (GPLv3). The RAOP/AirPlay protocol
                      + the GStreamer pipeline. ALL latency and A/V sync
                      behaviour lives here, not in src/.
```

The GUI does not touch video or audio. It builds an `argv` array and hands it
to `start_xmirror()`. Every tunable is a XMirror command-line flag.

## Configuration

XMirror flags are read from `arguments.txt` at startup. Precedence:

1. `%ProgramData%\XMirror\arguments.txt`   (machine-wide, wins)
2. `%APPDATA%\MadBlast\XMirror\arguments.txt`  (per-user)
3. Built-in default

Environment variables in the file are expanded. Changing it requires an app
restart -- there is no runtime reload.

`MainWindow::applyRendererAndFullscreenArgs()` post-processes the parsed args:
it injects `-fs` from the fullscreen checkbox, and overrides `-vs` when the
renderer combo is set to D3D11/D3D12. On "Auto" it leaves `-vs` from
`arguments.txt` alone.

## Latency (the thing that actually matters here)

Known-good line for low-latency mirroring on Windows:

    -n XMirror -nh -vd d3d11h264dec -vc d3d11convert -vs d3d11videosink -as wasapi2sink -al 0.05 -vsync 0

Why:

- `-vd d3d11h264dec` -- GPU decode. The default `decodebin` falls back to
  `avdec_h264` (libav, CPU-only), which is the main source of added latency and
  CPU load.
- `-vs d3d11videosink` / `-vc d3d11convert` -- keeps frames on the GPU, no
  readback. D3D11 is the correct Windows target. D3D9 is dead in GStreamer,
  `glimagesink` costs an extra copy, and CUDA/`nvh264dec` is NOT bundled.
- `-as wasapi2sink` -- low-latency audio. `wasapi2` is the newer, lower-latency
  WASAPI implementation; plain `wasapisink` (v1) is also bundled but is not the
  preferred sink here.
- `-al 0.05` -- audio latency reported to the client. Default is 0.25 (250 ms).
  Below ~0.03 tends to glitch.
- `-vsync <ms>` -- A/V trim, accepts negatives (`-vsync -40`). Read at startup
  only; there is no live slider yet.

## Bundled GStreamer plugins

`packaging/gstreamer-features.txt` lists stable *feature* names.
`scripts/resolve-gstreamer-plugins.py` resolves each to its owning plugin DLL
and copies the **whole DLL**. So features not listed in that file are still
available at runtime if they share a plugin with one that is -- this is why
`d3d11h264dec` and `d3d11convert` work despite being absent from the list.

Shipped plugin DLLs: app, audioconvert, audioresample, autodetect, coreelements,
d3d11, d3d12, level, libav, playback, videoconvertscale, videoparsersbad,
volume, wasapi, wasapi2.

Not shipped: nvcodec (no CUDA), qsv, va, amf.

## Building

Requires MSYS2 with Qt6, GStreamer, and the dependency set in
`ucrt_x64_dependencies.txt` / `clangarm64_dependencies.txt`. Not a lightweight
setup. See `docs/BUILDING.md`.

    .\build.ps1 package -Architecture x64
    .\build.ps1 package -Architecture arm64

Output lands in `out\<arch>\artifacts` (portable ZIP + MSI).

`libxmirror/` is vendored in-tree, so a plain clone is enough -- there is no
submodule to initialise. Upstream fixes must be merged in by hand.

## Conventions

- C++17, Qt6 Widgets, `CMAKE_AUTOMOC` on. No .ui files -- all widgets are
  constructed in code.
- Existing code has some Italian comments (upstream). New comments in English.
- Do not add XMirror flags to the GUI as hardcoded strings; they belong in
  `arguments.txt` so users can override them.
