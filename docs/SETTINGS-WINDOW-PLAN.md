# Settings window — implementation plan

A behavioural and architectural plan for replacing the current 300×260 panel:
how it coexists with the mirrored screen, how settings apply, and what has to be
fixed underneath first.

Written 26 August 2026. Target: `src/mainwindow.cpp`, Qt 6.11.2, GStreamer 1.28.6.

---

## Implementation status

**All phases COMPLETE and verified (26 August 2026).**

| Phase | State | Verified by |
|---|---|---|
| 0 Foundations | Done | `--soak-test 50`: 0 dirty stops, handle growth -8 |
| 1 Shell + tray | Done | Window 734x538 resizable; 6 sections; tray reworked |
| 2 Apply engine | Done | `--test-apply`: engine restarts, app stays up |
| 3 Settings rows | Done | 30 rows across 6 sections (6/6/5/6/3/4) |
| 4 Window control | Done | Geometry memory, monitor targeting, always-on-top |
| 5 States + polish | Done | Section 7 table below |

Regression commands:

```
XMirror.exe --self-test                       bundle completeness
XMirror.exe --soak-test 50 --soak-settle 500  engine stop/start
XMirror.exe --test-apply                      tier-3 apply path
```

### Section 7 states, as implemented

| Situation | Implemented behaviour |
|---|---|
| First ever launch | Window shown once; `has_run_before` then suppresses it |
| Normal launch | Tray only, unless "Open this window at launch" is on |
| Launched at login | Never shown. The autostart registry entry carries `--from-login` |
| Bonjour missing | **No longer quits.** Stays in the tray with a notice and an Install button |
| Engine fails to start | Persistent notice bar with a Retry button, not just a tray balloon |
| Engine crashes mid-session | Existing auto-restart, plus the notice |
| Settings changed while connected | Queued; deferral bar offers "Apply now" or "When they disconnect" |
| Second instance | Named mutex; the second exits and focuses the first |
| Settings window closed | Hides to tray (unchanged) |
| Mirror window closed | Ends session; engine restarts and waits |
| Machine-wide policy | Affected controls disabled with an explanatory notice |
| Display disconnected | Saved geometry ignored unless it still intersects a live screen |

### Deferred, with reasons

- **Live status (device name, negotiated resolution, frame rate).** Needs the
  engine's stdout parsed. The status line distinguishes stopped / waiting /
  mirroring, which covers the useful cases without a log parser.
- **Presets and log viewer.** Not required by any section 7 state; the Advanced
  page plus `arguments.txt` covers the same ground.
- **4K / HEVC.** Still no HEVC decoder in the bundle. Unchanged.

---

**Phase 0 detail — verified (26 August 2026).**

Verified by `XMirror.exe --soak-test 50`: **0 dirty stops in 50 cycles**,
handle growth after warm-up **-8**, memory growth +1 MB across 45 cycles.
Single-instance guard verified: second launch exits in 176 ms leaving one
instance. `--self-test` exits 0. App runs normally (41 threads, port bound).

| Item | State | Notes |
|---|---|---|
| D1 mirror rename on all renderers | Done | Now applied on Auto too |
| D2 event-driven HWND hook | Done | `SetWinEventHook` + bounded fallback poll |
| D3 stop always stops | Done | See engine bugs below |
| D4 clean shutdown, no `terminate()` | Done | 0/50 dirty stops |
| D5 engine start decoupled from widgets | Done | |
| Single-instance guard | Done | Named mutex + broadcast focus message |

### Defects found during Phase 0 that the plan did not anticipate

Fixing D3 exposed four further bugs. The first three are in the vendored engine
and were *masked* by D3: because the stop request was never delivered, the
engine's shutdown path had never actually executed.

| # | Where | Defect |
|---|---|---|
| E1 | `libxmirror/xmirror.cpp` `cleanup()` | Ended with an unconditional `exit(0)`. Written for the standalone CLI, where cleanup and program-end are the same thing. Embedded as a library it killed the host application on **every** engine shutdown. Removed; the three error paths that relied on it now `return 1`. |
| E2 | `libxmirror/xmirror.cpp` `stop_xmirror()` | Destroyed the RAOP server and dnssd **on the caller's thread** while the engine's main loop was still using them, and quit the loop through `g_loop`, which `main_loop()` leaves dangling (it creates a fresh loop per session). Now only raises flags; all teardown happens on the owning thread. |
| E3 | `libxmirror/xmirror.cpp` flags | `do_shutdown` and `reset_loop` were plain `bool` written cross-thread. At `-O2` the read can be cached, so a stop was never observed. Now `std::atomic<bool>`. Also `reset_callback()` now honours `do_shutdown`, because `main_loop()` clears `reset_loop` on entry and would discard a stop requested during start-up. |
| E4 | `src/airplayworker.cpp` `run()` | Built `argv` by storing `argBytes.back().data()` while still appending to the vector. Any reallocation left every previously stored pointer dangling. |

Also corrected: `MainWindow::stopServer()` called `m_worker->disconnect()`, which
severed `finished -> deleteLater` and leaked the QThread on every stop.

### Correction to this plan

Section 5 claimed restarting the engine was safe because `onAirplayStopped()`
already does it every session. That was **wrong**. `onAirplayStopped()` fires
when the *engine* ends a session; it never went through `stop_xmirror()`. The
externally-driven stop path had never once executed successfully, which is why
E1-E3 sat undetected. The premise was right for the wrong reason.

### Notes for later phases

- The Bonjour service was installed from `release\`, so `mDNSResponder.exe` is
  locked by the running service and `build.ps1 package` fails at the staging
  step. Stop the service to package, or reinstall it from the installed location.
- Killing the app leaves `xmirror-bluetooth-beacon.exe` processes orphaned.
- `XMirror.exe --soak-test <n> [--soak-settle <ms>]` is the regression test
  for the shutdown path. It writes `soak-report.txt` next to the exe, because
  `main()` reattaches stdout to the parent console and discards redirection.

---

## 1. Where things stand

The two-window split already exists — it just isn't designed. Establishing this
precisely matters, because several assumptions about what needs building turn
out to be wrong.

| Fact | Evidence |
|---|---|
| Nothing appears at launch; only a tray icon | `MainWindow window;` is constructed with no `.show()` |
| The existing window *is* a settings window — 300×260, 4 controls, 3 buttons | `setupUI()` |
| Closing the window hides it rather than quitting | `closeEvent()` calls `hide()` and `event->ignore()` |
| Single-clicking the tray toggles visibility | `onTrayActivated()` |
| The mirror window is created by GStreamer **inside this same process** | `windowPid == GetCurrentProcessId()` matches in `EnumWindowsProcRename` |
| The engine is already restarted routinely, every session end | `onAirplayStopped()` → `startServer()` after 1 s |
| Settings are already split across two stores | `QSettings` for 3 toggles; `arguments.txt` for everything else |

**Consequence.** Because `libxmirror` is linked statically, the mirrored window is
an **in-process Win32 window**, not a child process. We can find it, move it,
retitle it, hide it and watch it with ordinary Win32 calls — no IPC, no
window-manager hacks. This is the single most important fact in this plan.

### Defects to fix first

These are pre-existing, and each one undermines the settings window if left alone.

| # | Defect | Impact |
|---|---|---|
| **D1** | The mirror-window rename returns early unless the renderer is explicitly D3D11/D3D12. On **Auto** — the recommended setting — the window keeps GStreamer's default title. | Anything keying off the window title is unreliable in the default configuration. It also breaks `tools/sync-probe/probe.py`, whose `--title` defaults to `"AirPlay Video Stream"` — a title that never gets set on Auto. |
| **D2** | The rename runs on a `QTimer` polling `EnumWindows` every 5 seconds. | The title is wrong for up to 5 s after connect, and we poll forever for a one-shot event. |
| **D3** | `AirPlayWorker::stopAirplay()` calls `stop_xmirror()` but nothing ever calls `requestInterruption()`, so `run()`'s `while (!isInterruptionRequested())` loop can immediately re-enter `start_xmirror()`. | **Blocks the whole apply mechanism.** A stop that doesn't reliably stop cannot be the basis for "apply settings". |
| **D4** | `stopServer()` waits 1 s then calls `QThread::terminate()`. | Terminating a thread mid-GStreamer leaks pipeline resources and can corrupt state. With settings applied often, this goes from rare to routine. |
| **D5** | `startServer()` returns early via `if (!m_bleCheckbox) return;` — before the worker is created. | Couples engine startup to a specific widget existing. Any UI restructuring can silently stop the server starting at all. |

> **D3 and D4 are prerequisites, not nice-to-haves.** The settings window's
> central promise is "change a setting, it takes effect, the app stays open".
> That rests entirely on a clean engine stop/start. Build the UI on top of a stop
> that sometimes doesn't stop and you get a settings window that intermittently
> does nothing — the worst failure mode, because it reads as "this setting is
> broken" rather than "the plumbing is broken".

---

## 2. Principles

- **P1 — Today's behaviour is the default.** A user who never opens the window
  gets exactly the current app. Every control ships in the state that reproduces
  current behaviour.
- **P2 — `arguments.txt` stays the source of truth.** Per project convention,
  flags are not hardcoded in the GUI. Controls *override* the file; they don't
  replace it.
- **P3 — "Automatic" means don't touch.** Already how the renderer combo works:
  Auto leaves `-vs` from the file alone. Every new control gets the same escape
  hatch.
- **P4 — No silent restarts.** If a change interrupts an active mirroring
  session, say so before doing it.
- **P5 — The settings window never owns video.** It configures; it does not host,
  embed or reparent the mirrored picture.

---

## 3. The two windows

Separate top-level windows, different owners, different lifetimes, different
jobs. Keeping them separate is deliberate.

```
XMirror.exe  (one process)
│
├── Qt  ──────────────  Settings window     owned by us, persistent, hidden by default
│                       Tray icon           always present
│
└── GStreamer  ───────  Mirror window       owned by d3d11videosink,
                                            created on connect, destroyed on disconnect
```

| | Settings window | Mirror window |
|---|---|---|
| Created by | Qt, at app start | GStreamer, on client connect |
| Lifetime | Whole app session | One mirroring session |
| Visible at launch | Configurable (new) | No |
| On close | Hides to tray | Ends the session |
| Contains video | Never | Only video |
| We can restyle it | Fully | Only via Win32 on its `HWND` |

### Mirror window behaviour

Because the window is in-process we have real control over it — but we must
never fight GStreamer for ownership.

**On connect**

- The mirror window appears wherever Windows places it, at the stream's native size.
- **It takes focus.** That is correct: someone just started mirroring and wants to see it.
- Retitle it immediately, on *every* renderer setting including Auto (fixes D1).
- Optionally apply saved geometry, monitor placement, always-on-top and fullscreen.
- The settings window does **not** auto-hide. If it's open it stays open — hiding
  a window the user deliberately opened is a surprise. It simply falls behind the
  mirror in z-order, naturally.

**While connected**

- The settings window's status line goes live: device name, negotiated
  resolution, frame rate.
- The tray tooltip and icon reflect the connected state.
- Settings needing an engine restart are visibly marked as disruptive (§5).
- **Alt+Enter** fullscreen is a `d3d11videosink` feature, not ours — which is why
  the current title advertises it. Keep that, and stop hiding it behind a
  non-Auto renderer.

**On disconnect**

- By default the mirror window closes and the engine restarts within a second,
  ready for the next client. Existing behaviour, worth preserving.
- Save the window's last geometry before it is destroyed, so the next session can
  restore it.
- `-nc` keeps the window open after disconnect; `-nofreeze` stops it holding a
  frozen final frame. Both belong in Video settings.

> **Deliberately excluded: embedding the mirror inside the settings window.**
> Technically possible — reparent the `HWND` into a
> `QWidget::createWindowContainer`. **Don't.** It fights `d3d11videosink` for
> surface ownership, breaks its Alt+Enter fullscreen handling, adds a compositing
> step to a pipeline explicitly tuned for zero-copy, and risks exactly the
> latency documented in [LOW-LATENCY-SETUP.md](LOW-LATENCY-SETUP.md). The
> two-window split is the correct architecture, not a limitation to work around.

**Window management features worth adding**

| Feature | Mechanism | Risk |
|---|---|---|
| Remember size & position | `GetWindowRect` on destroy → `QSettings`; `SetWindowPos` on create | Low |
| Open on a chosen monitor | `SetWindowPos` to that monitor's work area | Low |
| Always on top | `SetWindowPos` with `HWND_TOPMOST` | Low |
| Start fullscreen | Existing `-fs` flag | Low — already implemented |
| Hide window border | `SetWindowLong` style change | Medium — can leave an unclosable window |
| Keep display awake while mirroring | `SetThreadExecutionState`, or `-scrsv` | Low |

All of these need the window's `HWND`, which means replacing the 5-second polling
loop (D2) with a hook that fires the moment the window exists. A
`SetWinEventHook` on `EVENT_OBJECT_CREATE` filtered to our own process is the
clean approach; a short-interval timer that stops on first success is the
pragmatic fallback.

---

## 4. Settings window structure

Left rail, hairline-divided rows, one control per row. Roughly 720×520,
resizable with a minimum size — not the current fixed 300×260.

| Section | Contains | Restart needed? |
|---|---|---|
| Connection | Device name, hide computer name, PIN, password, let a new device take over | Yes — all engine flags |
| Video | Renderer, fullscreen, resolution, frame rate, rotation, keep window after disconnect | Yes |
| Audio | Audio buffer, lip-sync trim, starting volume, gradual curve, picture-only | Yes |
| Window | Remember position, target monitor, always on top, keep display awake | **No** — applied via Win32 at next connect |
| Behaviour | Start at login, Bluetooth discovery, open settings at launch, silence timeout, performance figures | Mixed |
| Advanced | Decoder, converter, audio sink, ports, raw `arguments.txt` editor | Yes — with warnings |

The **Window** section is the interesting one: it needs no engine restart,
because it acts on the `HWND` rather than on `argv`. Grouping by *what applies
instantly* versus *what interrupts the session* is more useful to a user than
grouping by subsystem.

**Advanced, and the honest warning.** Decoder and converter are the two settings
most likely to leave someone staring at a black window. They belong behind an
Advanced section that states plainly that the shipped defaults are tuned, and
offers a one-click restore. The `-h265` / 4K option stays **out entirely** until
someone verifies an HEVC decoder is actually in the bundle — right now there
isn't one, so the control would silently produce no picture.

---

## 5. How settings apply

The architectural heart of the plan, and where it either feels like a real
settings window or like a form with a reboot button.

### Three tiers

**Tier 1 — Instant, no engine involvement.** Start at login (registry), open
settings at launch, window geometry, always-on-top, target monitor, theme.
Applied on the spot. No prompt, no interruption.

**Tier 2 — Next session, deferred, no interruption.** Window placement rules that
only matter when a mirror window is next created. Store now, apply at next
connect. The user sees "applies to the next connection" rather than being asked
to interrupt anything.

**Tier 3 — Engine restart, every `argv` flag.** Because
`start_xmirror(argc, argv)` reads its configuration exactly once, changing any
flag means stopping and restarting the engine — **not the application**, just the
worker thread.

- Nobody connected: apply immediately and silently. The restart takes about a
  second and is invisible.
- Someone *is* connected: do not restart behind their back. Show a single
  non-modal bar — *"3 changes waiting · Apply now (disconnects the current
  device) / When they disconnect"* — defaulting to the non-destructive option.

> **Why this is safer than it sounds.** `onAirplayStopped()` already tears down
> and restarts the engine after *every* session, on a 1-second timer. Restarting
> the worker to apply settings uses the same code path that already runs
> constantly in normal operation. We are not inventing a risky new mechanism — we
> are reusing a proven one. Which is precisely why D3 and D4 must be fixed first:
> they are latent faults in a path the app already depends on.

### Apply model

**Apply on change, with deferral** — not an OK/Cancel dialog. Toggling something
applies it, unless a session is live, in which case it queues visibly. This suits
a tray utility where most changes are single adjustments, and it removes the
"did I remember to press Save?" problem entirely. Reset returns everything to
defaults.

The one exception is the raw `arguments.txt` editor, which is inherently a
save-then-apply flow — it opens in the system editor, as it does today.

---

## 6. Persistence

Three stores exist. Their precedence must be explicit or this becomes
unpredictable.

| Store | Holds | Precedence |
|---|---|---|
| `%ProgramData%\XMirror\arguments.txt` | Machine-wide flags, set by an admin | Highest, if present |
| `%APPDATA%\MadBlast\XMirror\arguments.txt` | Per-user flags | Used when no machine file |
| `QSettings` (registry, `Software\MadBlast\XMirror`) | GUI overrides and app preferences | Layered on top of whichever file won |

The composition rule, unchanged in spirit from what
`applyRendererAndFullscreenArgs()` already does: **read the file, then let
non-default GUI settings strip and replace their corresponding flags.** A GUI
control at its default contributes nothing and leaves the file's value intact.

> **Edge case worth handling.** If a machine-wide `arguments.txt` exists, an
> administrator has deliberately set policy. Controls whose flags that file pins
> should show as locked with a short explanation, rather than silently letting a
> user change something that gets overridden. This is the difference between a
> settings window that is trusted and one that isn't.

---

## 7. States and edge cases

| Situation | Behaviour |
|---|---|
| First ever launch | Show the settings window once, so the app isn't invisible on day one. Never again unless asked. |
| Normal launch | Tray only, unless "open settings at launch" is on. |
| Launched at login | Always tray-only, regardless of that setting. Nobody wants a window at sign-in. |
| Bonjour Service missing | Currently the app **quits** if you decline. Change to: stay in the tray, show a persistent banner with an Install button, and disable engine controls. Quitting is too harsh a response to "not now". |
| Engine fails to start | Surface the actual error in the window, not only a 3-second tray balloon that is easily missed. |
| Engine crashes mid-session | Existing auto-restart handles it. Show it happened rather than hiding it. |
| Settings changed while connected | Queue, show the deferral bar. Never interrupt silently. |
| Second instance launched | Focus the existing settings window and exit. Requires the single-instance guard that doesn't yet exist. |
| User closes settings window | Hides to tray. Already correct. |
| User closes the mirror window | Ends the session; engine restarts and waits. |
| Machine-wide policy present | Affected controls locked with explanation. |
| Display disconnected while mirroring | Saved monitor target may no longer exist — fall back to primary rather than placing the window off-screen. |

> **The single-instance guard is now a real requirement.** Currently absent,
> despite `CLAUDE.md` claiming otherwise. Today it's tolerable because launching
> does nothing visible, so nobody double-launches. **The moment the settings
> window opens at launch, users will double-click the exe again** — and get a
> second engine competing for the same ports, presenting as a mysterious
> connection failure. A named mutex plus focusing the existing window is roughly
> twenty lines and stops a whole class of confusing bug reports.

---

## 8. Tray

The tray menu currently offers only Quit and Restart. There is no way to stop the
server — and the README's claim that you can right-click to start or stop AirPlay
is inherited from upstream and simply untrue. Proposed menu:

- **Status line** (disabled) — "Waiting for a device" / "Mirroring from Ben's iPhone"
- **Settings…** — show and focus the window
- **Stop / Start AirPlay** — the missing control
- *separator*
- **Restart engine** — the existing Restart, renamed to say what it restarts
- **Quit**

Single-click keeps toggling the window. The tooltip and icon should distinguish
idle from connected, so tray state is readable without opening anything.

---

## 9. Build order

Each phase leaves the app in a working, shippable state.

**Phase 0 — Foundations, no visible change.**
Fix D3 (interruption actually requested), D4 (clean stop, no `terminate()`), D5
(decouple startup from widgets). Add the single-instance guard. Add an `HWND`
hook to replace the 5-second poll, and retitle on every renderer setting (D1, D2).
*Verifiable:* repeated stop/start of the engine leaks nothing and always stops;
the mirror window is correctly titled on Auto, which also un-breaks the sync probe.

**Phase 1 — The shell.**
New resizable window, rail navigation, sections, tray menu rework. Move the
existing four controls in unchanged. Nothing new to configure yet.
*Verifiable:* everything that worked before still works, in a better container.

**Phase 2 — The apply engine.**
Three-tier apply, the deferral bar, changed-state tracking, Reset. Still only the
original settings — but they now apply without restarting the application.
*Verifiable:* change the renderer mid-session, get offered a choice, see it
applied without the app disappearing from the tray.

**Phase 3 — The settings themselves.**
Connection, Video, Audio and Behaviour rows. Each is now a small, uniform
addition, because the hard parts are already built.

**Phase 4 — Window control.**
The Window section: geometry memory, monitor targeting, always-on-top,
keep-awake. Depends entirely on phase 0's `HWND` hook.

**Phase 5 — Polish.**
Live status (device, resolution, frame rate — needs parsing engine output),
Advanced section, presets, log viewer, machine-policy locking.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| **Losing the tuned latency.** The biggest risk in the whole project. | Every control defaults to today's behaviour. The `d3d11` flag set is treated as one unit behind Advanced. Re-measure after each phase. |
| Repeated engine restarts leak GStreamer resources | Phase 0 fixes the unclean stop first. Soak-test 50 consecutive restarts watching handle and memory counts. |
| `start_xmirror` may hold global state that doesn't fully reset | Already re-entered on every disconnect, so it works in practice — but verify explicitly under repeated restarts before relying on it more heavily. |
| Win32 manipulation fights GStreamer's own window handling | Only touch position, size, title and z-order. Never reparent, never take over painting. |
| Settings window becomes a dumping ground | The 21-row list is the ceiling, not the floor. Anything not earning its place goes to Advanced or `arguments.txt`. |

---

## 11. Explicitly not doing

- **Embedding video in the settings window** — costs latency, fights the sink, no benefit.
- **Live `arguments.txt` reload** — a file watcher restarting the engine mid-edit
  is worse than restarting on demand.
- **4K / HEVC controls** — no HEVC decoder is bundled. Not until that's verified.
- **Per-device profiles** — needs device identity plumbing that doesn't exist.
- **Replacing `arguments.txt`** — it stays the source of truth, per project convention.

---

## 12. Open questions

1. Which of the 21 rows actually ship? The list is a menu, not a commitment.
2. Should the settings window open at launch by default, or stay opt-in after the
   first run?
3. Live status needs the engine's stdout parsed for resolution and frame rate —
   worth the parsing, or is a simple connected/idle state enough?
4. Is a machine-wide policy file a real scenario here, or can the locking
   behaviour be dropped?
5. The unspecified "broken bits" in the mockup — these need naming before phase 1.
