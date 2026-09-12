# XMirror

Windows AirPlay receiver. A Qt6 Widgets system-tray app wrapping the XMirror
AirPlay engine as a static library.

## Architecture

```
src/main.cpp          Qt entry point, single-instance guard, tray bootstrap,
                      the test modes, and --remove-user-data for the MSI.
src/mainwindow.cpp    The bulk of the GUI: settings pages, Home, tray, apply
                      engine, arguments.txt loading, settings -> flags, mirror
                      window adoption, update UI, beacon supervision.
src/fluenttheme.*     "Fluent Quiet" look: Fusion + palette + one tokenised
                      stylesheet on solid grey surfaces, follows Windows
                      light/dark and accent.
src/fluentswitch.*    Windows 11 toggle (painted QAbstractButton). Colours
                      come from the stylesheet via qproperty-.
src/mirrorshape.*     Keeps the GStreamer mirror window shaped like the picture
                      (fit on connect/rotation, resize lock, title-bar menu
                      toggle). Fed by xmirror_set_video_size_callback().
src/updatechecker.*   GitHub Releases update check, verified MSI download
                      and installer launch. UI lives in mainwindow.cpp.
src/airplayworker.cpp QThread that calls start_xmirror(argc, argv) from
                      libxmirror and loops until interruption.
src/mdns_responder.*  mDNSResponder beacon subprocess wrapper
libxmirror/            Vendored in-tree (GPLv3). The RAOP/AirPlay protocol
                      + the GStreamer pipeline. ALL latency and A/V sync
                      behaviour lives here, not in src/.
```

The GUI does not touch video or audio. It builds an `argv` array and hands it
to `start_xmirror()`; every tunable is a XMirror command-line flag. The one
exception is the mirror window itself, which the GUI moves, sizes and restacks
through Win32 -- never repaints or reparents.

## Configuration

XMirror flags are read from `arguments.txt` at startup. Precedence:

1. `%ProgramData%\XMirror\arguments.txt`   (machine-wide, wins)
2. `%APPDATA%\MadBlast\XMirror\arguments.txt`  (per-user)
3. Built-in default

Environment variables in the file are expanded. Changing it requires an app
restart -- there is no runtime reload.

`MainWindow::applyRendererAndFullscreenArgs()` layers the settings window over
the parsed file: each setting strips and replaces its own flag, and a setting
left at its default ("Automatic", or never chosen) leaves the file's value
alone. Audio timing only acts when chosen explicitly: `stable` gives
`-vsync 0` (replacing only `-vsync no`, so a numeric trim survives) and
`responsive` gives `-vsync no`.

## Latency (the thing that actually matters here)

The shipped line is `kDefaultArguments` in `src/mainwindow.cpp`, measured in
`docs/LATENCY-INVESTIGATION.md`. In short:

    -n XMirror -nh -vd d3d11h264dec -vc d3d11convert
    -vs "d3d11videosink processing-deadline=0 ts-offset=-100000000 ..."
    -as "wasapi2sink low-latency=true processing-deadline=0" -al 0.05 -vsync no

`-vsync no` plays audio unsynced (~170 ms instead of ~350 ms) at the cost of
clock discipline over long sessions; the Audio timing setting switches it.

Why the pieces:

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

The version lives in `VERSION` (major.minor.patch) and nowhere else: CMake
compiles it into the app as `XMIRROR_VERSION`, and `build.ps1` passes it to WiX.
MSI major upgrades ignore a fourth version field, so every release must change
one of the three. A tag-driven release build should write the tag into
`VERSION` rather than override one side only.

## Updates

`UpdateChecker` asks `api.github.com/repos/MadBlast0/Xmirror/releases/latest`
5 s after launch and daily while the app runs (setting `check_updates`, default
on). A release is offered when its tag (`v0.2.0` or `0.2.0`, no suffix) is
newer than `VERSION`. In-place
install needs all of: an asset named `XMirror-x64.msi` / `XMirror-arm64.msi`,
the GitHub-published sha256 `digest` on it, and this copy being the MSI install
(detected via the UpgradeCode). Otherwise the app links to the release page.
`XMIRROR_UPDATE_REPO=owner/name` points the check at another repository for
testing.

## Engine errors

The engine is a library inside a GUI app, so it must never call `exit()`.
Fatal start-up errors (bad `arguments.txt` options, GStreamer failing) throw
`EngineExit` inside `libxmirror/xmirror.cpp`; `start_xmirror()` catches it and
returns non-zero, and `xmirror_last_error()` says why. `AirPlayWorker` shows
that in the notice bar, and the app then waits for Retry instead of restarting
a failing engine every second. Callbacks run under C frames and must not
throw; they raise `video_pipeline_failed` and let the main loop unwind.

If the video pipeline cannot start (a missing element such as `d3d11h264dec`
on a GPU without H.264 decoding, or a sink that cannot reach READY),
`init_video_renderer()` retries with software decoding, then an automatic
sink, and reports the fallback through `xmirror_set_notice_callback()`.

## Testing

    XMirror.exe --self-test                        bundle completeness
    XMirror.exe --soak-test 50 --soak-settle 500   engine stop/start leaks
    XMirror.exe --test-apply                       settings change restarts engine
    XMirror.exe --test-engine-errors               bad options and pipelines end as errors

Reports are written next to the exe. The engine modes need the packaged runtime
(`release\`) and a running Bonjour Service. Standalone tests build with
`-DXMIRROR_BUILD_TESTS=ON` and run with `ctest`: `updatechecker_test` needs
network access, `mirrorshape_test` needs a desktop session with Direct3D 11.

## Installer

Never put `RemoveRegistryKey`/`RemoveFile` rules for user data back into
`product.wxs`. Windows Installer runs them on every removal of the component,
and a major upgrade removes the old product first, so they wiped every user's
settings on every update. User data is removed by the `RemoveUserData` custom
action (`XMirror.exe --remove-user-data`), conditioned on a real uninstall.

`libxmirror/` is vendored in-tree, so a plain clone is enough -- there is no
submodule to initialise. Upstream fixes must be merged in by hand.

## Conventions

- C++17, Qt6 Widgets, `CMAKE_AUTOMOC` on. No .ui files -- all widgets are
  constructed in code.
- Existing code has some Italian comments (upstream). New comments in English.
- Do not add XMirror flags to the GUI as hardcoded strings; they belong in
  `arguments.txt` so users can override them.
