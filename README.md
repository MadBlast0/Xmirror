# XMirror

Free AirPlay receiver for Windows. Mirror an iPhone, iPad or Mac to your PC,
with sound. \
Free as in "freedom" and as in "free beer".

## Features

- **Screen mirroring and audio** from Apple devices on the same network.
- **Low latency** by default: decoding and display stay on the GPU (Direct3D 11).
- **Fits the picture**: the mirror window takes the shape of the device's
  screen, including when you rotate it, so there are no black bars. Resize it
  freely, or lock the aspect ratio from the window's title-bar menu.
- **Settings window** with a Home page for status, quick switches and presets
  (Lowest latency, Balanced, Best quality).
- **Updates itself**: checks GitHub for new releases and installs them when you
  choose to.
- Runs quietly in the system tray, and can start when you sign in.

## Installation

Download **XMirror-x64.msi** from the
[latest release](https://github.com/MadBlast0/Xmirror/releases/latest) and run it.

Requires 64-bit Windows 10 or 11. The installer includes everything XMirror
needs, including the Bonjour service used for AirPlay discovery. A portable ZIP
is also available if you prefer not to install; portable copies do not update
themselves.

> [!IMPORTANT]
> **Windows SmartScreen warns about the installer.**
>
> ![Windows Defender SmartScreen prompt](./stuff/defender.png "SmartScreen")
>
> XMirror is not code-signed yet, so choose **More info**, then **Run anyway**.
> If you would rather not trust a prebuilt installer, you can
> [build it yourself](./docs/BUILDING.md).
>
> If Windows Firewall asks, **allow** XMirror on private networks, or devices
> will not be able to find it.

## Using XMirror

1. Start XMirror. It appears in the system tray.
2. On your iPhone or iPad, open **Control Center** and tap **Screen Mirroring**,
   then choose **XMirror**. On a Mac, use **Screen Mirroring** in Control Center.
3. The mirrored screen opens in its own window. Press **Alt+Enter** for
   fullscreen.

Click the tray icon to open the settings window. Right-click it to start or
stop AirPlay, restart, or quit.

<details>
<summary><strong>Advanced configuration</strong></summary>

<br>

The settings window covers the common options. Everything else is a streaming
engine argument read from `arguments.txt`; **Advanced → Edit arguments.txt**
opens it, and **View reference** lists every option. Changes to the file take
effect when XMirror restarts.

Configuration precedence:

1. `%ProgramData%\XMirror\arguments.txt`
2. `%APPDATA%\MadBlast\XMirror\arguments.txt`
3. built-in default

The machine-wide file takes precedence when it exists, allowing administrators
to enforce a shared configuration. When it is absent, each user can maintain
their own configuration under `%APPDATA%`.

Environment variables are expanded when the app starts. For example:

```text
-n %COMPUTERNAME% -nh
```

If an option in the file is invalid, XMirror says which one in its settings
window instead of starting.

</details>

<details>
<summary><strong>Building from source</strong></summary>

<br>

The simplest way is GitHub Actions on your own fork; see
[BUILDING.md](./docs/BUILDING.md), which also describes how releases are
published.

To build locally, install MSYS2 and run:

```powershell
.\build.ps1 package -Architecture x64
```

This produces a verified portable ZIP and MSI under `out\x64\artifacts`. See the
[developer guide](./docs/DEVELOPERS-GUIDE.md) for prerequisites and additional
commands. An ARM64 build (`-Architecture arm64`) is supported by the build
script but has not been tested yet.

</details>

<details>
<summary><strong>Roadmap</strong></summary>

<br>

- Code-sign release builds
- Test and publish ARM64 builds

</details>

<details>
<summary><strong>Known issues</strong></summary>

<br>

- Sometimes, just after an iPhone connects, the stream appears but is frozen.
  Connecting a second time fixes it.

</details>

## Reporting issues

Please report problems with XMirror on the
[issue tracker](https://github.com/MadBlast0/Xmirror/issues). For issues in
bundled third-party components, report them in their own repositories.

## Credits and license

XMirror's streaming engine is derived from
[UxPlay](https://github.com/FDH2/UxPlay) by F. Duncanh, itself based on
[RPiPlay](https://github.com/FD-/RPiPlay) by Florian Draschbacher.

XMirror is licensed under the GNU General Public License v3.0 (GPLv3), and
bundles third-party components under their own licenses. See the
[LICENSE](./docs/LICENSE.rtf).
