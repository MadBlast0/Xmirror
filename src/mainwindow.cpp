#include "mainwindow.h"
#include "airplayworker.h"
#include "mdns_responder.hpp"
#include <windows.h>
#include <winsvc.h>

#include <QProcess>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QComboBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextStream>
#include <QFont>
#include <QSignalBlocker>
#include <QFrame>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QLineEdit>
#include <QScreen>
#include <QListWidget>
#include <QSpinBox>
#include <QScrollArea>
#include <QStackedWidget>

// Low-latency defaults, verified by measurement (docs/LATENCY-INVESTIGATION.md).
//
// -vd d3d11h264dec  GPU decode. decodebin falls back to CPU avdec_h264, which
//                   is the single biggest source of added latency.
// -vc/-vs d3d11*    keeps frames on the GPU end to end, no readback.
// processing-deadline=0  the sink defaults to 15 ms and adds it to live
//                   pipeline latency.
// ts-offset=-100ms  frames carry a PTS derived from the client's NTP capture
//                   time, and that conversion runs ~106 ms fast, so sync=true
//                   held every frame. Cancelling it took video 144 ms -> 38 ms
//                   while leaving A/V clock discipline intact.
// -vsync no         audio sink runs sync=false: 171 ms instead of 351 ms. This
//                   disables clock discipline on audio (audio_renderer.c:180),
//                   which upstream abandoned in 1.64 because it drifts over
//                   long sessions. We ship it anyway because 351 ms is audible
//                   lip-sync error and the drift only matters on multi-hour
//                   sessions. If drift is reported, "-vsync 0" in arguments.txt
//                   restores clock discipline at the cost of the latency.
// -vsync no         sync=false on both sinks: the audio sink plays each packet
//                   on arrival instead of at the time the client scheduled it.
//                   Measured 185 ms against 351 ms for "-vsync 0". The cost is
//                   clock discipline: this is the pre-1.64 method upstream
//                   abandoned because the iPad's audio clock and the sound
//                   card's are never exactly equal, so long sessions can drift.
//                   "-vsync 0" in arguments.txt restores discipline if drift is
//                   ever reported. See docs/AUDIO-LATENCY-OPTIONS.md.
// -as low-latency=true processing-deadline=0
//                   Both measured as having no effect on the number (27 Aug;
//                   the GST_TRACER output confirms low-latency reaches the ring
//                   buffer). Kept because they are harmless and correct, not
//                   because they buy anything.
// -al 0.05          advertised to the client, which ignores it. Retained
//                   because it costs nothing, not because it does anything.
static const char *const kDefaultArguments =
    "-n XMirror -nh "
    "-vd d3d11h264dec -vc d3d11convert "
    "-vs \"d3d11videosink processing-deadline=0 ts-offset=-100000000 "
    "fullscreen-toggle-mode=GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_ALT_ENTER\" "
    "-as \"wasapi2sink low-latency=true processing-deadline=0\" "
    "-al 0.05 -vsync no";

// How long to wait for the engine thread to unwind before force-terminating.
// A clean stop returns well inside this; hitting the timeout means a real bug,
// so it is logged rather than passed over silently.
// Tuning that must ride along with whichever D3D sink is selected. Without it,
// choosing a renderer in the settings window strips the tuned "-vs" line from
// arguments.txt and replaces it with a bare sink name -- silently discarding the
// latency fix and returning video to ~144 ms.
static const char *const kVideoSinkOptions =
    " processing-deadline=0 ts-offset=-100000000"
    " fullscreen-toggle-mode=GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_ALT_ENTER";

static constexpr int kEngineStopTimeoutMs = 5000;

// Title applied to the GStreamer video window once it appears.
static const char *const kMirrorWindowTitle =
    "AirPlay Video Stream (ALT+ENTER for Fullscreen)";

QStringList xMirrorDefaultArguments() {
    return QProcess::splitCommand(QString::fromLatin1(kDefaultArguments));
}

// The only MainWindow instance, so the free-function WinEvent callback can
// reach it. Set in the constructor, cleared in the destructor.
static MainWindow *g_mainWindow = nullptr;

// GStreamer's D3D11/D3D12 sinks title their window something like
// "Direct3D11 renderer". Match loosely: the exact string varies by sink and by
// GStreamer version.
// The video window belongs to GStreamer's D3D11/D3D12 sink, not to Qt, so it is
// created with no icon and shows a blank tile in the taskbar and Alt-Tab. Push
// the app icon onto it from the executable's own resource (ID 1, resources.rc),
// which is embedded in the binary and therefore available even when the
// resources/ directory beside the exe is not.
static void applyAppIconToWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    // Loaded once and reused: adoption can run again after the sink destroys
    // and recreates its window, and reloading each time would leak a handle.
    static HICON bigIcon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                   LR_DEFAULTCOLOR));
    static HICON smallIcon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                   LR_DEFAULTCOLOR));

    if (bigIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(bigIcon));
    }
    if (smallIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(smallIcon));
    }
}

static bool isMirrorWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid != GetCurrentProcessId()) return false;

    wchar_t buffer[512];
    const int len = GetWindowTextW(hwnd, buffer, 512);
    if (len <= 0) return false;

    const QString title = QString::fromWCharArray(buffer, len);
    return title.contains("Direct", Qt::CaseInsensitive) &&
           title.contains("enderer", Qt::CaseInsensitive);
}

static BOOL CALLBACK FindMirrorWindowProc(HWND hwnd, LPARAM lParam) {
    if (isMirrorWindow(hwnd)) {
        *reinterpret_cast<HWND *>(lParam) = hwnd;
        return FALSE;  // found it; stop enumerating
    }
    return TRUE;
}

// Fires on window creation and rename events in this process. The sink creates
// its window before setting a title, so both event types matter.
static void CALLBACK MirrorWinEventProc(HWINEVENTHOOK, DWORD, HWND hwnd,
                                        LONG idObject, LONG idChild,
                                        DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    if (g_mainWindow) g_mainWindow->notifyMirrorWindowEvent();
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    g_mainWindow = this;

    ensureSettingsFileExists();
    setupTray();
    setupUI();

    applyMachinePolicyLocks();

    // Declining the Bonjour install no longer exits. The app stays in the tray
    // and explains itself, which is recoverable; quitting was not.
    if (ensureBonjourServiceInstalled()) {
        startServer();
    }
    updateStatus();
}

MainWindow::~MainWindow() {
    m_quitting = true;
    removeMirrorWindowHook();
    stopServer();
    g_mainWindow = nullptr;
}

bool MainWindow::testApplyEngineChange() {
    if (!m_fullscreenCheckbox) return false;

    // Drive the real control so the whole chain runs, rather than calling the
    // apply function directly and proving nothing about the wiring.
    m_fullscreenCheckbox->toggle();
    return m_running;
}

void MainWindow::showSettingsWindow() {
    show();
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    raise();
    activateWindow();
}

QString MainWindow::userArgumentsPath() const {
    QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return appDataPath + "/arguments.txt";
}

QString MainWindow::machineArgumentsPath() const {
    QString programDataPath =
        QProcessEnvironment::systemEnvironment().value("ProgramData");
    if (programDataPath.isEmpty()) {
        programDataPath = "C:/ProgramData";
    }
    return programDataPath + "/XMirror/arguments.txt";
}

QString MainWindow::activeArgumentsPath() const {
    QString machinePath = machineArgumentsPath();
    if (QFile::exists(machinePath)) {
        return machinePath;
    }

    return userArgumentsPath();
}

QString MainWindow::expandEnvironmentVariables(const QString &content) const {
    QString expanded = content;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    for (const QString &key : env.keys()) {
        expanded.replace("%" + key + "%", env.value(key), Qt::CaseInsensitive);
    }

    return expanded;
}

void MainWindow::ensureSettingsFileExists() {
    if (QFile::exists(userArgumentsPath()) || QFile::exists(machineArgumentsPath())) {
        return;
    }

    QFileInfo info(userArgumentsPath());
    QDir().mkpath(info.absolutePath());

    QFile file(userArgumentsPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << kDefaultArguments;
        file.close();
    }
}

QStringList MainWindow::getArgumentsFromFile() {
    const QString userPath = userArgumentsPath();
    const QString machinePath = machineArgumentsPath();
    const QString selectedPath = activeArgumentsPath();

    qInfo().noquote() << "[arguments] Machine file:"
                      << QDir::toNativeSeparators(machinePath)
                      << (QFile::exists(machinePath) ? "(found)" : "(not found)");
    qInfo().noquote() << "[arguments] User file:"
                      << QDir::toNativeSeparators(userPath)
                      << (QFile::exists(userPath) ? "(found)" : "(not found)");
    qInfo().noquote() << "[arguments] Reading:"
                      << QDir::toNativeSeparators(selectedPath);

    QFile file(selectedPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString content = expandEnvironmentVariables(QTextStream(&file).readAll().trimmed());
        file.close();
        return QProcess::splitCommand(content);
    }

    qWarning().noquote()
        << "[arguments] Unable to read the selected file; using built-in defaults.";
    return QProcess::splitCommand(QString(kDefaultArguments));
}


// A single settings row: name (and optional one-line explanation) on the left,
// its control right-aligned. Keeps rows visually consistent regardless of what
// kind of control they hold.
static QWidget *makeRow(const QString &name,
                        const QString &description,
                        QWidget *control) {
    auto *row = new QWidget();
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 9, 0, 9);
    layout->setSpacing(18);

    auto *textColumn = new QVBoxLayout();
    textColumn->setSpacing(2);

    auto *nameLabel = new QLabel(name);
    nameLabel->setWordWrap(true);
    textColumn->addWidget(nameLabel);

    if (!description.isEmpty()) {
        auto *desc = new QLabel(description);
        desc->setWordWrap(true);
        desc->setEnabled(false);  // renders muted in every Qt style
        textColumn->addWidget(desc);
    }

    layout->addLayout(textColumn, 1);
    if (control) {
        control->setParent(row);
        layout->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
    }
    return row;
}

static QFrame *makeSeparator(Qt::Orientation orientation = Qt::Horizontal) {
    auto *line = new QFrame();
    if (orientation == Qt::Horizontal) {
        line->setFrameShape(QFrame::HLine);
        line->setFixedHeight(1);
    } else {
        line->setFrameShape(QFrame::VLine);
        line->setFixedWidth(1);
    }
    line->setFrameShadow(QFrame::Plain);
    return line;
}

// Builds a scrollable page from a heading and an ordered list of rows.
static QWidget *makePage(const QString &heading, const QList<QWidget *> &rows) {
    auto *content = new QWidget();
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(0);

    auto *title = new QLabel(heading);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 3.0);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);
    layout->addSpacing(12);

    for (int i = 0; i < rows.size(); ++i) {
        if (i > 0) layout->addWidget(makeSeparator());
        layout->addWidget(rows.at(i));
    }

    layout->addStretch();

    auto *scroll = new QScrollArea();
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

void MainWindow::addSection(const QString &name, QWidget *page) {
    m_sectionList->addItem(name);
    m_sectionStack->addWidget(page);
}

QCheckBox *MainWindow::settingCheckbox(const QString &key, bool defaultValue) {
    auto *box = new QCheckBox();
    QSettings settings;
    box->setChecked(settings.value(key, defaultValue).toBool());
    connect(box, &QCheckBox::toggled, this, [this, key](bool on) {
        QSettings s;
        if (s.value(key).isValid() && s.value(key).toBool() == on) return;
        s.setValue(key, on);
        markEngineSettingChanged();
    });
    return box;
}

QComboBox *MainWindow::settingCombo(const QString &key,
                                    const QVector<QPair<QString, QString>> &options,
                                    const QString &defaultValue) {
    auto *combo = new QComboBox();
    combo->setMinimumWidth(170);
    for (const auto &option : options) {
        combo->addItem(option.first, option.second);
    }

    QSettings settings;
    const QString saved = settings.value(key, defaultValue).toString();
    const int index = combo->findData(saved);
    if (index >= 0) combo->setCurrentIndex(index);

    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, combo, key](int) {
        const QString value = combo->currentData().toString();
        QSettings s;
        if (s.value(key).toString() == value) return;
        s.setValue(key, value);
        markEngineSettingChanged();
    });
    return combo;
}

QLineEdit *MainWindow::settingLineEdit(const QString &key,
                                       const QString &placeholder,
                                       bool isPassword) {
    auto *edit = new QLineEdit();
    edit->setMinimumWidth(170);
    edit->setPlaceholderText(placeholder);
    if (isPassword) edit->setEchoMode(QLineEdit::Password);

    QSettings settings;
    edit->setText(settings.value(key, QString()).toString());

    // Applied on editingFinished, not on every keystroke: restarting the engine
    // per character typed into the device name would be absurd.
    connect(edit, &QLineEdit::editingFinished, this, [this, edit, key]() {
        const QString value = edit->text().trimmed();
        QSettings s;
        if (s.value(key, QString()).toString() == value) return;
        s.setValue(key, value);
        markEngineSettingChanged();
    });
    return edit;
}

QSpinBox *MainWindow::settingSpinBox(const QString &key, int lo, int hi, int step,
                                     int defaultValue, const QString &suffix,
                                     const QString &specialText) {
    auto *spin = new QSpinBox();
    spin->setRange(lo, hi);
    spin->setSingleStep(step);
    spin->setMinimumWidth(140);
    if (!suffix.isEmpty()) spin->setSuffix(suffix);
    if (!specialText.isEmpty()) spin->setSpecialValueText(specialText);

    QSettings settings;
    spin->setValue(settings.value(key, defaultValue).toInt());

    connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, key](int value) {
        QSettings s;
        if (s.value(key).isValid() && s.value(key).toInt() == value) return;
        s.setValue(key, value);
        // The lip-sync trim's neutral value is 0, which is also its default, so
        // a separate flag records that the user has actually set it.
        if (key == QStringLiteral("vsync_ms")) s.setValue("vsync_set", true);
        markEngineSettingChanged();
    });
    return spin;
}

// "Automatic" is stored as an empty string, meaning "leave -s out and let the
// engine use its own default".
static const char *const kAutomaticResolution = "Automatic";

static bool isValidResolution(const QString &text) {
    static const QRegularExpression pattern(
        QStringLiteral("^\\d{3,5}x\\d{3,5}(@\\d{1,3})?$"));
    return pattern.match(text).hasMatch();
}

QComboBox *MainWindow::settingResolutionCombo() {
    auto *combo = new QComboBox();
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setMinimumWidth(190);

    // Presets are plain text, so a typed value and a chosen one are the same
    // kind of thing. 3:2 entries matter for iPads, whose panels are not 16:9 --
    // a 16:9 request gets height-limited and pillarboxed, wasting pixels on
    // black bars.
    combo->addItem(kAutomaticResolution);
    combo->addItem("1280x720@60");
    combo->addItem("1920x1080@60");
    combo->addItem("1920x1344@60");
    combo->addItem("2388x1668@60");
    combo->addItem("2560x1440@60");

    QSettings settings;
    const QString saved = settings.value("resolution", QString()).toString().trimmed();
    if (saved.isEmpty()) {
        combo->setCurrentIndex(0);
    } else {
        // Show a custom value instead of falling back to index 0, which would
        // display "Automatic" while a different value was actually in force.
        if (combo->findText(saved) < 0) combo->addItem(saved);
        combo->setCurrentText(saved);
    }

    const auto commit = [this, combo]() {
        QString text = combo->currentText().trimmed();
        QString value;

        if (text.isEmpty() || text.compare(kAutomaticResolution, Qt::CaseInsensitive) == 0) {
            value.clear();
        } else if (isValidResolution(text)) {
            value = text;
        } else {
            // Reject rather than pass a malformed -s to the engine, which would
            // make it refuse to start.
            QSettings s;
            const QString previous = s.value("resolution", QString()).toString();
            combo->setCurrentText(previous.isEmpty() ? kAutomaticResolution : previous);
            if (m_tray) {
                m_tray->showMessage(
                    "XMirror",
                    "Resolution must look like 1920x1080 or 1920x1080@60.",
                    QSystemTrayIcon::Warning, 4000);
            }
            return;
        }

        QSettings s;
        if (s.value("resolution", QString()).toString() == value) return;
        s.setValue("resolution", value);
        markEngineSettingChanged();
    };

    // Typed text commits on Enter or focus loss; picking from the list commits
    // immediately.
    connect(combo->lineEdit(), &QLineEdit::editingFinished, this, commit);
    connect(combo, QOverload<int>::of(&QComboBox::activated), this,
            [commit](int) { commit(); });

    return combo;
}

QWidget *MainWindow::buildConnectionPage() {
    return makePage("Connection", {
        makeRow("Device name",
                "Shown in the AirPlay picker. Leave empty to keep the name from "
                "arguments.txt.",
                settingLineEdit("device_name", "XMirror")),
        makeRow("Hide computer name",
                "Drops the \"@hostname\" suffix clients see.",
                settingCheckbox("hide_hostname", true)),
        makeRow("Ask for a PIN",
                "A four-digit code must be entered on the device before "
                "mirroring starts.",
                settingCheckbox("pin_enabled", false)),
        makeRow("Require a password",
                "Overrides the PIN setting when both are on.",
                settingCheckbox("password_enabled", false)),
        makeRow("Password",
                "Only used when \"Require a password\" is on.",
                settingLineEdit("password", "Not set", true)),
        makeRow("Let a new device take over",
                "Off keeps the first device connected.",
                settingCheckbox("nohold", false)),
    });
}

QWidget *MainWindow::buildAudioPage() {
    return makePage("Audio", {
        makeRow("Audio timing",
                "Stable keeps audio clock-locked and clean. Responsive plays it "
                "sooner but it can drift out over a long session.",
                settingCombo("audio_mode", {
                    {"Stable", "stable"},
                    {"Responsive", "responsive"},
                }, "stable")),
        makeRow("Starting volume",
                "Level used when a device first connects.",
                settingSpinBox("volume_pct", -1, 100, 5, -1, " %", "Automatic")),
        makeRow("Gradual volume curve",
                "Makes the device's volume slider feel more even.",
                settingCheckbox("taper", false)),
        makeRow("Picture only, no sound",
                "Mirrors the screen with audio left on the device.",
                settingCheckbox("no_audio", false)),
    });
}

QWidget *MainWindow::buildVideoPage() {
    QSettings settings;

    m_fullscreenCheckbox = new QCheckBox();
    m_fullscreenCheckbox->setChecked(
        settings.value("force_fs_enabled", false).toBool());
    connect(m_fullscreenCheckbox, &QCheckBox::toggled,
            this, &MainWindow::toggleForceFullscreen);

    m_rendererCombo = new QComboBox();
    m_rendererCombo->addItem("Automatic", "auto");
    m_rendererCombo->addItem("D3D11", "d3d11");
    m_rendererCombo->addItem("D3D12", "d3d12");
    m_rendererCombo->setMinimumWidth(150);
    {
        const QString saved = settings.value("renderer_mode", "auto").toString();
        const int idx = m_rendererCombo->findData(saved);
        if (idx >= 0) m_rendererCombo->setCurrentIndex(idx);
    }
    connect(m_rendererCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onRendererChanged);

    return makePage("Video", {
        makeRow("Renderer",
                "Automatic keeps the tuned setting from arguments.txt.",
                m_rendererCombo),
        makeRow("HEVC (H.265)",
                "Same picture at about half the bitrate, decoded on the GPU. "
                "Devices that do not support it keep using H.264.",
                settingCheckbox("hevc_enabled", false)),
        makeRow("Open fullscreen",
                "Requires a renderer other than Automatic.",
                m_fullscreenCheckbox),
        makeRow("Resolution",
                "A request only - the device decides. Type any size, e.g. "
                "2388x1668@60 for an 11-inch iPad. Matching the device's own "
                "panel avoids both upscaling and black bars.",
                settingResolutionCombo()),
        makeRow("Frame rate limit",
                "Lower uses less CPU and network.",
                settingSpinBox("fps_limit", 0, 60, 5, 0, " fps", "Automatic")),
        makeRow("Rotation",
                "For a display mounted sideways.",
                settingCombo("rotation", {
                    {"None", ""},
                    {"90 degrees left", "L"},
                    {"90 degrees right", "R"},
                }, "")),
        makeRow("Keep window open after disconnect",
                "Leaves the mirror window on screen when the device stops.",
                settingCheckbox("keep_window", false)),
    });
}

QWidget *MainWindow::buildBehaviourPage() {
    QSettings settings;

    m_bleCheckbox = new QCheckBox();
    m_bleCheckbox->setChecked(settings.value("ble_enabled", true).toBool());
    connect(m_bleCheckbox, &QCheckBox::toggled, this, &MainWindow::toggleBle);

    m_autostartCheckbox = new QCheckBox();
    m_autostartCheckbox->setChecked(isAutostartEnabled());
    connect(m_autostartCheckbox, &QCheckBox::toggled,
            this, &MainWindow::toggleAutostart);

    // Tier 1: purely an application preference, applied instantly, no engine
    // restart, so it does not go through settingCheckbox().
    m_openAtLaunchCheckbox = new QCheckBox();
    m_openAtLaunchCheckbox->setChecked(settings.value("open_at_launch", false).toBool());
    connect(m_openAtLaunchCheckbox, &QCheckBox::toggled, this, [](bool on) {
        QSettings s;
        s.setValue("open_at_launch", on);
    });

    return makePage("Behaviour", {
        makeRow("Start at login",
                "XMirror runs in the tray when you sign in to Windows.",
                m_autostartCheckbox),
        makeRow("Open this window at launch",
                "Off means XMirror starts silently in the tray.",
                m_openAtLaunchCheckbox),
        makeRow("Bluetooth discovery",
                "Helps nearby devices find this PC faster.",
                m_bleCheckbox),
        makeRow("Keep the screen awake",
                "Stops the screensaver interrupting a session.",
                settingCombo("screensaver", {
                    {"Automatic", ""},
                    {"While mirroring", "1"},
                    {"Always", "2"},
                    {"Never", "0"},
                }, "")),
        makeRow("Give up after silence",
                "How long to wait on a quiet device before ending the session. "
                "0 means never.",
                settingSpinBox("reset_secs", -1, 120, 5, -1, " s", "Automatic")),
        makeRow("Show performance figures",
                "Reports the frame rate sent by the device. Useful when chasing "
                "stutter.",
                settingCheckbox("fps_data", false)),
    });
}

QCheckBox *MainWindow::appCheckbox(const QString &key, bool defaultValue) {
    auto *box = new QCheckBox();
    QSettings settings;
    box->setChecked(settings.value(key, defaultValue).toBool());
    connect(box, &QCheckBox::toggled, this, [key](bool on) {
        QSettings s;
        s.setValue(key, on);
    });
    return box;
}

QComboBox *MainWindow::appCombo(const QString &key,
                                const QVector<QPair<QString, QString>> &options,
                                const QString &defaultValue) {
    auto *combo = new QComboBox();
    combo->setMinimumWidth(170);
    for (const auto &option : options) {
        combo->addItem(option.first, option.second);
    }

    QSettings settings;
    const int index = combo->findData(settings.value(key, defaultValue).toString());
    if (index >= 0) combo->setCurrentIndex(index);

    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [combo, key](int) {
        QSettings s;
        s.setValue(key, combo->currentData().toString());
    });
    return combo;
}

QWidget *MainWindow::buildWindowPage() {
    // Monitors are listed as they are now. If the chosen one is gone by the
    // time a session starts, applyMirrorWindowPreferences() falls back to the
    // primary rather than placing the window off-screen.
    QVector<QPair<QString, QString>> monitors;
    monitors.append({QStringLiteral("Automatic"), QString()});
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        const QScreen *screen = screens.at(i);
        monitors.append({QString("Monitor %1 (%2 x %3)")
                             .arg(i + 1)
                             .arg(screen->geometry().width())
                             .arg(screen->geometry().height()),
                         QString::number(i)});
    }

    return makePage("Window", {
        makeRow("Remember position and size",
                "Reopens the mirrored screen where you last left it.",
                appCheckbox("remember_geometry", false)),
        makeRow("Open on",
                "Which display the mirrored screen appears on.",
                appCombo("target_monitor", monitors, QString())),
        makeRow("Always on top",
                "Keeps the mirrored screen above other windows.",
                appCheckbox("always_on_top", false)),
    });
}

// Applied when the sink's window is adopted, and only ever through
// SetWindowPos: position, size and z-order. Never reparenting.
void MainWindow::applyMirrorWindowPreferences() {
    HWND hwnd = reinterpret_cast<HWND>(m_mirrorHwnd);
    if (!hwnd || !IsWindow(hwnd)) return;

    QSettings settings;

    if (settings.value("always_on_top", false).toBool()) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Restoring a saved rectangle takes precedence over a monitor preference,
    // because the saved rectangle already encodes which monitor it was on.
    if (settings.value("remember_geometry", false).toBool()) {
        const int x = settings.value("mirror_x", INT_MIN).toInt();
        const int y = settings.value("mirror_y", INT_MIN).toInt();
        const int w = settings.value("mirror_w", 0).toInt();
        const int h = settings.value("mirror_h", 0).toInt();

        if (x != INT_MIN && y != INT_MIN && w > 0 && h > 0) {
            // Only honour it if it still lands on a screen that exists.
            bool onScreen = false;
            const QList<QScreen *> screens = QGuiApplication::screens();
            for (const QScreen *screen : screens) {
                if (screen->geometry().intersects(QRect(x, y, w, h))) {
                    onScreen = true;
                    break;
                }
            }
            if (onScreen) {
                SetWindowPos(hwnd, nullptr, x, y, w, h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                qDebug() << "[mirror] restored geometry" << x << y << w << h;
                return;
            }
            qDebug() << "[mirror] saved geometry is off-screen; ignoring";
        }
    }

    const QString monitorIndex =
        settings.value("target_monitor", QString()).toString();
    if (!monitorIndex.isEmpty()) {
        bool ok = false;
        const int index = monitorIndex.toInt(&ok);
        const QList<QScreen *> screens = QGuiApplication::screens();
        if (ok && index >= 0 && index < screens.size()) {
            const QRect area = screens.at(index)->availableGeometry();
            SetWindowPos(hwnd, nullptr, area.x() + 40, area.y() + 40, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            qDebug() << "[mirror] moved to monitor" << index + 1;
        } else {
            qDebug() << "[mirror] chosen monitor no longer exists; leaving in place";
        }
    }
}

// The sink destroys its window without warning, so the rectangle is sampled
// while the session is alive rather than read at the end.
void MainWindow::rememberMirrorWindowGeometry() {
    HWND hwnd = reinterpret_cast<HWND>(m_mirrorHwnd);
    if (!hwnd || !IsWindow(hwnd)) return;
    if (IsIconic(hwnd) || IsZoomed(hwnd)) return;  // do not persist a minimised or maximised frame

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return;

    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return;

    QSettings settings;
    settings.setValue("mirror_x", (int)rect.left);
    settings.setValue("mirror_y", (int)rect.top);
    settings.setValue("mirror_w", w);
    settings.setValue("mirror_h", h);
}

QWidget *MainWindow::buildAdvancedPage() {
    m_settingsBtn = new QPushButton("Edit arguments.txt");
    connect(m_settingsBtn, &QPushButton::clicked, this, &MainWindow::openSettingsFile);

    m_listargsBtn = new QPushButton("View reference");
    connect(m_listargsBtn, &QPushButton::clicked, this, &MainWindow::openListArgsFile);

    m_licenseBtn = new QPushButton("View licence");
    connect(m_licenseBtn, &QPushButton::clicked, this, &MainWindow::showLicense);

    auto *resetBtn = new QPushButton("Reset to defaults");
    connect(resetBtn, &QPushButton::clicked, this, &MainWindow::resetSettingsToDefaults);

    return makePage("Advanced", {
        makeRow("Streaming arguments",
                "The tuned defaults live here. Changing them can cost latency or "
                "leave a black window. Restart the app to apply.",
                m_settingsBtn),
        makeRow("Available options",
                "Full list of supported streaming arguments.",
                m_listargsBtn),
        makeRow("Licence",
                "XMirror is GPLv3 and bundles third-party components.",
                m_licenseBtn),
        makeRow("Reset settings",
                "Returns every setting to its default. arguments.txt is left "
                "untouched.",
                resetBtn),
    });
}

void MainWindow::setupUI() {
    setWindowTitle("XMirror");
    setWindowIcon(QApplication::windowIcon());
    resize(720, 500);
    setMinimumSize(600, 420);

    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- notice bar: persistent, for things that need attention ---
    m_noticeBar = new QWidget();
    {
        auto *noticeLayout = new QHBoxLayout(m_noticeBar);
        noticeLayout->setContentsMargins(16, 10, 16, 10);
        noticeLayout->setSpacing(10);

        m_noticeLabel = new QLabel();
        m_noticeLabel->setWordWrap(true);
        noticeLayout->addWidget(m_noticeLabel, 1);

        m_noticeAction = new QPushButton();
        connect(m_noticeAction, &QPushButton::clicked, this, [this]() {
            if (m_noticeCallback) m_noticeCallback();
        });
        noticeLayout->addWidget(m_noticeAction);
    }
    m_noticeBar->setVisible(false);
    root->addWidget(m_noticeBar);

    // --- deferral bar: shown only while changes are queued ---
    m_deferralBar = new QWidget();
    {
        auto *barLayout = new QHBoxLayout(m_deferralBar);
        barLayout->setContentsMargins(16, 10, 16, 10);
        barLayout->setSpacing(10);

        m_deferralLabel = new QLabel();
        m_deferralLabel->setWordWrap(true);
        barLayout->addWidget(m_deferralLabel, 1);

        auto *laterBtn = new QPushButton("When they disconnect");
        connect(laterBtn, &QPushButton::clicked, this, &MainWindow::applyPendingWhenIdle);
        barLayout->addWidget(laterBtn);

        auto *nowBtn = new QPushButton("Apply now");
        connect(nowBtn, &QPushButton::clicked, this, &MainWindow::applyPendingNow);
        barLayout->addWidget(nowBtn);
    }
    m_deferralBar->setVisible(false);
    root->addWidget(m_deferralBar);

    // --- body: section rail + page stack ---
    auto *body = new QWidget();
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    m_sectionList = new QListWidget();
    m_sectionList->setFixedWidth(160);
    m_sectionList->setFrameShape(QFrame::NoFrame);
    m_sectionList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_sectionStack = new QStackedWidget();

    bodyLayout->addWidget(m_sectionList);
    bodyLayout->addWidget(makeSeparator(Qt::Vertical));
    bodyLayout->addWidget(m_sectionStack, 1);

    // Sections are added only once they have content. Connection, Audio and
    // Window arrive in later phases.
    addSection("Connection", buildConnectionPage());
    addSection("Video", buildVideoPage());
    addSection("Audio", buildAudioPage());
    addSection("Behaviour", buildBehaviourPage());
    addSection("Window", buildWindowPage());
    addSection("Advanced", buildAdvancedPage());

    connect(m_sectionList, &QListWidget::currentRowChanged,
            m_sectionStack, &QStackedWidget::setCurrentIndex);
    m_sectionList->setCurrentRow(0);

    root->addWidget(body, 1);

    // --- footer: engine status ---
    root->addWidget(makeSeparator());

    auto *footer = new QWidget();
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(16, 9, 16, 9);

    m_statusLabel = new QLabel("Starting...");
    footerLayout->addWidget(m_statusLabel);
    footerLayout->addStretch();

    root->addWidget(footer);

    updateStatus();
}


void MainWindow::openSettingsFile() {
    QString filePath = activeArgumentsPath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    
    m_tray->showMessage("Settings", "Restart the app to apply new arguments.", 
                        QSystemTrayIcon::Information, 3000);
}

void MainWindow::openListArgsFile() {
    QString filePath = QApplication::applicationDirPath() + "/resources/xmirror_arguments_list.txt";
    QFile::setPermissions(filePath, QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther);
    QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
}

void MainWindow::setupTray() {
    m_tray = new QSystemTrayIcon(this);
    QIcon trayIcon;
    QString icoPath = QApplication::applicationDirPath() + "/resources/icon.ico";
    trayIcon = QIcon(icoPath);
    if (trayIcon.isNull()) {
        trayIcon = QApplication::style()->standardIcon(QStyle::SP_MediaPlay);
    }
    m_tray->setIcon(trayIcon);
    m_tray->setToolTip("XMirror");

    m_trayMenu = new QMenu(this);

    // Non-interactive status line at the top of the menu.
    m_statusAction = m_trayMenu->addAction("Starting...");
    m_statusAction->setEnabled(false);
    m_trayMenu->addSeparator();

    m_trayMenu->addAction("Settings...", this, &MainWindow::showSettingsWindow);

    // Stopping the server was previously impossible from the tray, despite the
    // README claiming otherwise.
    m_toggleServerAction =
        m_trayMenu->addAction("Stop AirPlay", this, &MainWindow::toggleServerFromTray);

    m_trayMenu->addSeparator();
    m_trayMenu->addAction("Restart app", this, &MainWindow::restartApplication);
    m_trayMenu->addAction("Quit", this, &MainWindow::quit);

    m_tray->setContextMenu(m_trayMenu);
    
    connect(m_tray, &QSystemTrayIcon::activated, this, &MainWindow::onTrayActivated);
    m_tray->show();
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger) {
        if (isVisible()) hide(); else showSettingsWindow();
    }
}

void MainWindow::toggleBle(bool checked) {
    QSettings settings;
    if (settings.value("ble_enabled", true).toBool() == checked) {
        return;
    }
    settings.setValue("ble_enabled", checked);
    markEngineSettingChanged();
}

void MainWindow::toggleForceFullscreen(bool checked) {
    QSettings settings;
    if (settings.value("force_fs_enabled", false).toBool() == checked) {
        return;
    }
    settings.setValue("force_fs_enabled", checked);
    markEngineSettingChanged();
}

void MainWindow::onRendererChanged(int /*index*/) {
    if (!m_rendererCombo) return;

    const QString mode = m_rendererCombo->currentData().toString();

    QSettings settings;
    if (settings.value("renderer_mode", "auto").toString() == mode) return;

    settings.setValue("renderer_mode", mode);
    markEngineSettingChanged();
}

// Removes every occurrence of a flag from the argument list. When the flag
// takes a value, its value is removed with it.
static void stripFlag(QStringList &args, const QString &flag, bool takesValue) {
    for (int i = 0; i < args.size();) {
        if (args.at(i) == flag) {
            args.removeAt(i);
            if (takesValue && i < args.size()) {
                args.removeAt(i);
            }
            continue;
        }
        ++i;
    }
}

// Applies GUI overrides on top of the arguments read from arguments.txt.
//
// Values are read from QSettings rather than from widgets, so building the
// argument list never depends on any part of the UI having been constructed.
// Every setting has a "leave it alone" state -- an empty string, or a sentinel
// number -- and in that state the corresponding flag from arguments.txt is left
// exactly as the file had it. A user who changes nothing gets the tuned line
// verbatim.
void MainWindow::applyRendererAndFullscreenArgs(QStringList &args) {
    QSettings settings;

    const auto boolOpt = [&settings](const QString &key, bool fallback) {
        return settings.value(key, fallback).toBool();
    };
    const auto strOpt = [&settings](const QString &key) {
        return settings.value(key, QString()).toString().trimmed();
    };
    const auto intOpt = [&settings](const QString &key, int sentinel) {
        return settings.value(key, sentinel).toInt();
    };

    // --- Connection ---
    const QString deviceName = strOpt("device_name");
    if (!deviceName.isEmpty()) {
        stripFlag(args, "-n", true);
        args << "-n" << deviceName;
    }

    // -nh is present in the shipped default line, so the override is only
    // meaningful in the "off" direction.
    if (!boolOpt("hide_hostname", true)) {
        stripFlag(args, "-nh", false);
    }

    stripFlag(args, "-pin", false);
    if (boolOpt("pin_enabled", false)) {
        args << "-pin";
    }

    const QString password = strOpt("password");
    stripFlag(args, "-pw", true);
    if (boolOpt("password_enabled", false) && !password.isEmpty()) {
        args << "-pw" << password;
    }

    stripFlag(args, "-nohold", false);
    if (boolOpt("nohold", false)) {
        args << "-nohold";
    }

    // --- Video ---
    // HEVC roughly halves the bitrate for the same picture. The engine builds a
    // second pipeline for it and advertises the capability; a device that does
    // not offer HEVC simply keeps using H.264, so enabling this cannot break
    // playback. XMirror substitutes h264 -> h265 in the decoder and parser names
    // itself, so "-vd d3d11h264dec" becomes d3d11h265dec for that pipeline.
    stripFlag(args, "-h265", false);
    if (boolOpt("hevc_enabled", false)) {
        args << "-h265";
    }

    stripFlag(args, "-fs", false);
    if (boolOpt("force_fs_enabled", false)) {
        args << "-fs";
    }

    const QString renderer = settings.value("renderer_mode", "auto").toString();
    if (renderer == "d3d11" || renderer == "d3d12") {
        stripFlag(args, "-vs", true);
        const QString sink = (renderer == "d3d11") ? "d3d11videosink" : "d3d12videosink";
        args << "-vs" << (sink + QString::fromLatin1(kVideoSinkOptions));
    }

    const QString resolution = strOpt("resolution");
    if (!resolution.isEmpty()) {
        stripFlag(args, "-s", true);
        args << "-s" << resolution;
    }

    const int fpsLimit = intOpt("fps_limit", 0);
    if (fpsLimit > 0) {
        stripFlag(args, "-fps", true);
        args << "-fps" << QString::number(fpsLimit);
    }

    const QString rotation = strOpt("rotation");
    if (rotation == "L" || rotation == "R") {
        stripFlag(args, "-r", true);
        args << "-r" << rotation;
    }

    stripFlag(args, "-nc", false);
    if (boolOpt("keep_window", false)) {
        args << "-nc";
    }

    // --- Audio ---
    const int volumePct = intOpt("volume_pct", -1);
    if (volumePct >= 0 && volumePct <= 100) {
        stripFlag(args, "-vol", true);
        args << "-vol" << QString::number(volumePct / 100.0, 'f', 2);
    }

    // The single most consequential audio setting, and previously only reachable
    // by hand-editing arguments.txt.
    //
    // "stable"     -> -vsync 0  : sink sync=true. Honours the timestamps the
    //                 client schedules, so audio stays clock-disciplined and
    //                 clean, at ~350 ms.
    // "responsive" -> -vsync no : sink sync=false. Plays on arrival at ~171 ms,
    //                 but audio then has no clock discipline and can drift.
    //                 This is the method upstream abandoned in 1.64.
    //
    // Note this also flips the VIDEO sink's sync, because audio_renderer.c:180
    // ties them to the same flag.
    const QString audioMode = settings.value("audio_mode", "stable").toString();
    if (audioMode == "responsive") {
        stripFlag(args, "-vsync", true);
        args << "-vsync" << "no";
    }

    stripFlag(args, "-taper", false);
    if (boolOpt("taper", false)) {
        args << "-taper";
    }

    if (boolOpt("no_audio", false)) {
        stripFlag(args, "-as", true);
        args << "-as" << "0";
    }

    // --- Behaviour ---
    const QString screensaver = strOpt("screensaver");
    if (!screensaver.isEmpty()) {
        stripFlag(args, "-scrsv", true);
        args << "-scrsv" << screensaver;
    }

    const int resetSecs = intOpt("reset_secs", -1);
    if (resetSecs >= 0) {
        stripFlag(args, "-reset", true);
        args << "-reset" << QString::number(resetSecs);
    }

    stripFlag(args, "-FPSdata", false);
    if (boolOpt("fps_data", false)) {
        args << "-FPSdata";
    }
}

void MainWindow::startServer() {
    if (m_worker && m_worker->isRunning()) return;

    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(appData);
    
    QStringList args = getArgumentsFromFile();
    
    int bleIdx = args.indexOf("-ble");
    if (bleIdx != -1) {
        args.removeAt(bleIdx);
        if (bleIdx < args.size() && !args[bleIdx].startsWith("-")) {
            args.removeAt(bleIdx);
        }
    }

    applyRendererAndFullscreenArgs(args);

    // Bluetooth discovery is optional. Its absence must never stop the engine
    // from starting, so this must not return early.
    const bool bleEnabled = m_bleCheckbox && m_bleCheckbox->isChecked();
    if (bleEnabled) {
        QString bleFilePath = QDir::toNativeSeparators(appData + "/xmirror_status.ble");
        args << "-ble" << bleFilePath;
        startBluetoothBeacon(bleFilePath);
    } else {
        stopBluetoothBeacon();
    }

    m_worker = new AirPlayWorker(this);
    m_worker->setArgs(args);

    connect(m_worker, &AirPlayWorker::started, this, &MainWindow::onAirplayStarted);
    connect(m_worker, &AirPlayWorker::stopped, this, &MainWindow::onAirplayStopped);
    connect(m_worker, &AirPlayWorker::finished, m_worker, &QObject::deleteLater);

    m_worker->start();
}

void MainWindow::stopServer() {
    stopBluetoothBeacon();

    // A deliberate stop detaches the worker's signals, so onAirplayStopped()
    // will not run -- and that is the only other place the mirror window is
    // released. Without this, m_mirrorHwnd keeps pointing at a window the sink
    // has already destroyed, and every later adoption attempt returns early:
    // the next session's window is never retitled or positioned.
    removeMirrorWindowHook();
    forgetMirrorWindow();

    if (m_worker) {
        AirPlayWorker *worker = m_worker;

        // Clear the member first: onAirplayStopped() must not see a worker that
        // is on its way out and schedule a restart against it.
        m_worker = nullptr;

        // Drop only our own slots. A blanket disconnect() would also sever
        // finished -> deleteLater and leak the thread object on every stop.
        disconnect(worker, &AirPlayWorker::started,       this, nullptr);
        disconnect(worker, &AirPlayWorker::stopped,       this, nullptr);
        disconnect(worker, &AirPlayWorker::errorOccurred, this, nullptr);

        if (worker->isRunning()) {
            // Called directly, not queued. The worker object lives on the GUI
            // thread, and we block in wait() immediately below -- a queued call
            // would sit in an event loop that never gets to run.
            if (!worker->stopAndWait(kEngineStopTimeoutMs)) {
                qWarning().noquote()
                    << "[engine] thread did not stop within"
                    << kEngineStopTimeoutMs << "ms; terminating";
                worker->terminate();
                worker->wait(1000);
            }
        }
    }

    m_running = false;
    updateStatus();
}


void MainWindow::onAirplayStarted() {
    m_running = true;
    updateStatus();

    qDebug() << "[engine] started";

    // Adopt the mirror window on every renderer setting, including Auto.
    // Gating this on a non-Auto renderer left the window untitled in the
    // default configuration, which also broke tools/sync-probe/probe.py.
    installMirrorWindowHook();
    tryAdoptMirrorWindow();
}

void MainWindow::installMirrorWindowHook() {
    if (!m_winEventHook) {
        // WINEVENT_OUTOFCONTEXT delivers through this thread's message queue,
        // which Qt already pumps. Scoped to our own process id.
        HWINEVENTHOOK hook = SetWinEventHook(
            EVENT_OBJECT_CREATE, EVENT_OBJECT_NAMECHANGE,
            nullptr, MirrorWinEventProc,
            GetCurrentProcessId(), 0,
            WINEVENT_OUTOFCONTEXT);
        m_winEventHook = reinterpret_cast<void *>(hook);
        if (!hook) {
            qWarning() << "[mirror] SetWinEventHook failed; using fallback poll";
        }
    }

    // Fallback for when the hook does not fire. The window is created only when
    // a client connects, which can be hours after the engine starts, so this
    // must keep looking for as long as the engine runs -- an earlier 60 s cap
    // meant a later connection was never adopted and the window kept
    // GStreamer's own title. It stops the moment a window is adopted, which is
    // the part the original 5 s poll got wrong.
    if (!m_mirrorFallbackTimer) {
        m_mirrorFallbackTimer = new QTimer(this);
        connect(m_mirrorFallbackTimer, &QTimer::timeout, this, [this]() {
            if (m_mirrorHwnd) {
                m_mirrorFallbackTimer->stop();
                return;
            }
            tryAdoptMirrorWindow();
        });
    }
    m_mirrorFallbackTicks = 0;
    m_mirrorFallbackTimer->start(1000);
}

void MainWindow::removeMirrorWindowHook() {
    if (m_winEventHook) {
        UnhookWinEvent(reinterpret_cast<HWINEVENTHOOK>(m_winEventHook));
        m_winEventHook = nullptr;
    }
    if (m_mirrorFallbackTimer) {
        m_mirrorFallbackTimer->stop();
    }
}

void MainWindow::notifyMirrorWindowEvent() {
    if (m_mirrorHwnd) return;  // already adopted
    tryAdoptMirrorWindow();
}

void MainWindow::tryAdoptMirrorWindow() {
    // Defence in depth: a handle we still hold may belong to a window the sink
    // has already destroyed. Holding a stale one would block every future
    // adoption, so drop it and carry on looking.
    if (m_mirrorHwnd && !IsWindow(reinterpret_cast<HWND>(m_mirrorHwnd))) {
        qDebug() << "[mirror] previously adopted window is gone; releasing it";
        m_mirrorHwnd = nullptr;
    }

    if (m_mirrorHwnd) return;

    HWND found = nullptr;
    EnumWindows(FindMirrorWindowProc, reinterpret_cast<LPARAM>(&found));
    if (!found) return;

    m_mirrorHwnd = reinterpret_cast<void *>(found);

    const QString title = QString::fromLatin1(kMirrorWindowTitle);
    SetWindowTextW(found, reinterpret_cast<const wchar_t *>(title.utf16()));
    applyAppIconToWindow(found);

    qDebug() << "[mirror] window adopted, retitled and given the app icon";

    applyMirrorWindowPreferences();

    // Sample the window rectangle while the session is alive; the sink destroys
    // its window without warning, so reading it at the end is too late.
    if (!m_mirrorGeometryTimer) {
        m_mirrorGeometryTimer = new QTimer(this);
        connect(m_mirrorGeometryTimer, &QTimer::timeout, this, [this]() {
            QSettings s;
            if (!s.value("remember_geometry", false).toBool()) return;
            rememberMirrorWindowGeometry();
        });
    }
    m_mirrorGeometryTimer->start(2000);

    if (m_mirrorFallbackTimer) m_mirrorFallbackTimer->stop();
}

void MainWindow::forgetMirrorWindow() {
    // Take one last sample before dropping the handle.
    QSettings settings;
    if (settings.value("remember_geometry", false).toBool()) {
        rememberMirrorWindowGeometry();
    }
    if (m_mirrorGeometryTimer) m_mirrorGeometryTimer->stop();

    m_mirrorHwnd = nullptr;
    m_mirrorFallbackTicks = 0;
}

void MainWindow::onAirplayStopped() {
    m_running = false;

    // The sink destroys its window when the session ends, so the handle we
    // adopted is stale from here on.
    removeMirrorWindowHook();
    forgetMirrorWindow();

    updateStatus();

    if (!m_quitting) {
        // Anything deferred while a device was connected takes effect now,
        // folded into the restart that was going to happen anyway.
        if (m_pendingChanges > 0) {
            qDebug() << "Session ended; applying" << m_pendingChanges
                     << "deferred change(s)";
            m_pendingChanges = 0;
            m_applyWhenIdle = false;
            updateDeferralBar();
        }
        qDebug() << "Session ended, restarting server to stay ready...";
        QTimer::singleShot(1000, this, &MainWindow::startServer);
    }
}

void MainWindow::onAirplayError(const QString &message) {
    // A three-second tray balloon is easy to miss, and this is the one message
    // that explains why nothing works. Put it where it stays.
    showNotice("AirPlay engine error: " + message, "Retry", [this]() {
        hideNotice();
        restartEngineNow();
    });
    if (m_tray) {
        m_tray->showMessage("XMirror", message, QSystemTrayIcon::Warning, 5000);
    }
}

void MainWindow::toggleAutostart(bool checked) {
    setAutostart(checked);
    updateStatus();
}

bool MainWindow::isAutostartEnabled() const {
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    return reg.contains("XMirror");
}

void MainWindow::setAutostart(bool enabled) {
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    if (enabled) {
        QString path = QDir::toNativeSeparators(QApplication::applicationFilePath());
        // The marker lets main() tell a login launch from a manual one, so the
        // settings window never appears while someone is signing in.
        reg.setValue("XMirror", "\"" + path + "\" --from-login");
    } else {
        reg.remove("XMirror");
    }
}

void MainWindow::startBluetoothBeacon(const QString &path) {
    if (m_beacon && m_beacon->state() != QProcess::NotRunning)
        return;

    QString exe = QApplication::applicationDirPath() + "/xmirror-bluetooth-beacon.exe";
    if (!QFile::exists(exe)) {
        qDebug() << "xmirror-bluetooth-beacon.exe not found";
        return;
    }

    m_beacon = new QProcess(this);
    m_beacon->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_beacon, &QProcess::readyRead, this, [this]() {
        // Forward beacon logs to our debug console
        qDebug() << "[beacon output]" << m_beacon->readAll().trimmed();
    });

    // Pass the explicit path to the beacon file
    m_beacon->start(exe, {"--path", path});
    qDebug() << "Beacon process started watching:" << path;
}

void MainWindow::stopBluetoothBeacon() {
    if (!m_beacon) return;
    
    qDebug() << "Stopping beacon process";
    if (m_beacon->state() != QProcess::NotRunning) {
        m_beacon->terminate();
        if (!m_beacon->waitForFinished(500)) {
            qDebug() << "Beacon didn't terminate, killing";
            m_beacon->kill();
            m_beacon->waitForFinished(100);
        }
    }
    
    delete m_beacon;
    m_beacon = nullptr;
    qDebug() << "Beacon stopped and cleaned up";
}

void MainWindow::showLicense() {
    QString path = QApplication::applicationDirPath() + "/LICENSE.rtf";
    if (QFile::exists(path)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    } else qDebug() << "License file not found:" << path;
}

void MainWindow::quit() {
    m_quitting = true;
    stopServer();
    QApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (!m_quitting) {
        hide();
        event->ignore();
    } else {
        event->accept();
    }
}

// Tier 3 apply: the setting maps to an engine argument, so the engine must be
// restarted to pick it up. Tier 1 settings (autostart, window preferences) take
// effect on the spot and never come through here.
void MainWindow::markEngineSettingChanged() {
    ++m_pendingChanges;

    // Nothing running: the new value is simply read at the next start.
    if (!m_running) {
        m_pendingChanges = 0;
        updateDeferralBar();
        return;
    }

    if (isSessionActive()) {
        // Someone is mirroring. Restarting would drop them, so offer the choice
        // rather than deciding for them.
        updateDeferralBar();
        return;
    }

    // Idle: apply straight away. Takes about a second and nobody notices.
    restartEngineNow();
}

void MainWindow::restartEngineNow() {
    m_pendingChanges = 0;
    m_applyWhenIdle = false;
    updateDeferralBar();

    if (m_statusLabel) m_statusLabel->setText("Applying changes...");
    if (m_statusAction) m_statusAction->setText("Applying changes...");

    // stopServer() detaches the worker's signals, so onAirplayStopped() will not
    // fire and will not schedule a restart of its own. We restart explicitly.
    stopServer();
    startServer();
}

void MainWindow::applyPendingNow() {
    restartEngineNow();
}

void MainWindow::applyPendingWhenIdle() {
    m_applyWhenIdle = true;
    updateDeferralBar();
}

void MainWindow::showNotice(const QString &text,
                            const QString &actionText,
                            std::function<void()> action) {
    if (!m_noticeBar || !m_noticeLabel || !m_noticeAction) return;

    m_noticeLabel->setText(text);
    m_noticeCallback = std::move(action);

    m_noticeAction->setVisible(!actionText.isEmpty());
    if (!actionText.isEmpty()) m_noticeAction->setText(actionText);

    m_noticeBar->setVisible(true);
}

void MainWindow::hideNotice() {
    m_noticeCallback = nullptr;
    if (m_noticeBar) m_noticeBar->setVisible(false);
}

bool MainWindow::machinePolicyActive() const {
    return QFile::exists(machineArgumentsPath());
}

// With a machine-wide arguments.txt in place an administrator has set policy.
// Disabling the affected controls is honest; letting someone change a value
// that is then overridden is not.
void MainWindow::applyMachinePolicyLocks() {
    if (!machinePolicyActive()) return;

    const QList<QWidget *> locked = {
        m_rendererCombo, m_fullscreenCheckbox, m_bleCheckbox,
    };
    for (QWidget *widget : locked) {
        if (!widget) continue;
        widget->setEnabled(false);
        widget->setToolTip("Set by a machine-wide arguments.txt policy.");
    }

    showNotice("Some settings are managed by a machine-wide arguments.txt and "
               "cannot be changed here.");
}

void MainWindow::resetSettingsToDefaults() {
    const int choice = QMessageBox::question(
        this, "Reset settings",
        "Return every setting to its default?\n\n"
        "Your arguments.txt is not touched.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes) return;

    QSettings settings;
    const QStringList engineKeys = {
        "device_name", "hide_hostname", "pin_enabled", "password_enabled",
        "password", "nohold", "force_fs_enabled", "renderer_mode", "resolution",
        "hevc_enabled",
        "fps_limit", "rotation", "keep_window", "audio_latency_ms", "vsync_ms",
        "vsync_set", "volume_pct", "taper", "no_audio", "screensaver",
        "reset_secs", "fps_data", "ble_enabled",
        "remember_geometry", "target_monitor", "always_on_top",
        "mirror_x", "mirror_y", "mirror_w", "mirror_h",
    };
    for (const QString &key : engineKeys) settings.remove(key);
    settings.sync();

    QMessageBox::information(
        this, "Settings reset",
        "Defaults restored. XMirror will restart to apply them.");
    restartApplication();
}

void MainWindow::updateDeferralBar() {
    if (!m_deferralBar || !m_deferralLabel) return;

    if (m_pendingChanges <= 0) {
        m_deferralBar->setVisible(false);
        return;
    }

    const QString what = (m_pendingChanges == 1)
        ? QString("1 change")
        : QString("%1 changes").arg(m_pendingChanges);

    m_deferralLabel->setText(m_applyWhenIdle
        ? what + " will be applied when the device disconnects."
        : what + " waiting. Applying now disconnects the current device.");

    m_deferralBar->setVisible(true);
}

void MainWindow::toggleServerFromTray() {
    if (m_running) {
        stopServer();
    } else {
        startServer();
    }
    updateStatus();
}

void MainWindow::updateStatus() {
    QString status;
    if (m_bonjourMissing) {
        status = "Discovery unavailable - Bonjour not installed";
    } else if (m_running) {
        status = isSessionActive() ? "Mirroring" : "Waiting for a device";
    } else {
        status = "AirPlay stopped";
    }

    // Every one of these can be null: updateStatus() runs during construction
    // and again after the engine stops, and the UI is built in stages.
    if (m_statusLabel) m_statusLabel->setText(status);
    if (m_statusAction) m_statusAction->setText(status);
    if (m_tray) m_tray->setToolTip("XMirror - " + status);

    if (m_toggleServerAction) {
        m_toggleServerAction->setText(m_running ? "Stop AirPlay" : "Start AirPlay");
    }

    if (m_autostartCheckbox) {
        // Reflect the registry, which can change outside this app.
        QSignalBlocker blocker(m_autostartCheckbox);
        m_autostartCheckbox->setChecked(isAutostartEnabled());
    }
}

bool MainWindow::isWindowsServicePresent(const std::wstring& serviceName) const {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return false;

    SC_HANDLE hSvc = OpenServiceW(hSCM, serviceName.c_str(), SERVICE_QUERY_STATUS);
    if (!hSvc) {
        DWORD err = GetLastError();
        CloseServiceHandle(hSCM);
        return err != ERROR_SERVICE_DOES_NOT_EXIST;
    }

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return true;
}

bool MainWindow::isBonjourServicePresent() const {
    return isWindowsServicePresent(L"Bonjour Service");
}

void MainWindow::installBonjourService() {
    QMessageBox::information(
        this,
        "Installing Bonjour Service",
        "Starting installation of 'Bonjour Service'. Follow any UAC prompt.");

    const int rc = mdns::MdnsResponder::install();

    if (rc == 0 && isBonjourServicePresent()) {
        m_bonjourMissing = false;
        hideNotice();
        QMessageBox::information(
            this,
            "Installation Complete",
            "Bonjour Service installed. Starting AirPlay.");
        startServer();
        updateStatus();
        return;
    }

    QMessageBox::critical(
        this,
        "Installation Failed",
        "Could not install 'Bonjour Service'. XMirror will keep running, "
        "but devices will not be able to discover this PC.");
}

// Returns true when the engine may start. Declining no longer quits the
// application: it stays in the tray with a notice explaining what is missing,
// so the situation is recoverable without relaunching.
bool MainWindow::ensureBonjourServiceInstalled() {
    if (isBonjourServicePresent()) {
        m_bonjourMissing = false;
        return true;
    }

    const int choice = QMessageBox::question(
        this,
        "Bonjour Service Required",
        "Bonjour Service is required for devices to discover this PC. It is "
        "not installed.\n\nInstall it now?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);

    if (choice == QMessageBox::Yes) {
        installBonjourService();
        return isBonjourServicePresent();
    }

    m_bonjourMissing = true;
    showNotice(
        "Bonjour Service is not installed, so devices cannot find this PC.",
        "Install",
        [this]() { installBonjourService(); });
    return false;
}

void MainWindow::restartApplication() {
    m_quitting = true;
    stopServer();

    QString exePath = QApplication::applicationFilePath();
    QStringList args = QCoreApplication::arguments();
    if (!args.isEmpty()) args.removeFirst(); // remove exe path

    // Surrender the single-instance mutex before spawning. The replacement
    // starts while this process is still alive, and would otherwise see the
    // mutex, conclude an instance is already running, and exit immediately --
    // so "Restart app" would quit the app instead of restarting it.
    releaseSingleInstanceLock();

    QProcess::startDetached(exePath, args);

    // close GUI of current process after a short delay
    QTimer::singleShot(200, qApp, &QCoreApplication::quit);
}
