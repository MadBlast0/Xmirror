#include "mainwindow.h"
#include "airplayworker.h"
#include "mdns_responder.hpp"
#include "fluentswitch.h"
#include "fluenttheme.h"
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
#include <QFontDatabase>
#include <QGridLayout>

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

    // Must precede the native window's creation; falls back to the solid look
    // on Windows 10 and early Windows 11.
    FluentTheme::enableAcrylic(this);

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


// When a change takes effect. Shown as a badge on the row, so the cost of a
// setting is visible before it is touched rather than discovered afterwards.
// Mirrors the three tiers in docs/SETTINGS-WINDOW-PLAN.md section 5.
enum class Tier {
    Restart,  // tier 3: every argv flag; stops and restarts the worker
    Instant,  // tier 1: an app preference, applied on the spot
    Next,     // tier 2: applied to the mirror window at the next connect
    None,     // an action, not a setting
};

static QLabel *makeBadge(Tier tier) {
    QString text;
    QString value;
    switch (tier) {
        case Tier::Restart: text = "Restarts engine"; value = "restart"; break;
        case Tier::Instant: text = "Instant";         value = "instant"; break;
        case Tier::Next:    text = "Next session";    value = "next";    break;
        case Tier::None:    return nullptr;
    }

    auto *badge = new QLabel(text);
    badge->setObjectName("badge");
    // Read by the stylesheet's QLabel#badge[tier="..."] rules.
    badge->setProperty("tier", value);
    badge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return badge;
}

// A single settings row: name, tier badge and one-line explanation on the left,
// the control right-aligned. Rendered as a card by the stylesheet, so rows no
// longer need separators between them.
static QWidget *makeRow(const QString &name,
                        const QString &description,
                        QWidget *control,
                        Tier tier = Tier::Restart) {
    auto *row = new QFrame();
    row->setObjectName("row");

    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(16, 11, 16, 11);
    layout->setSpacing(18);

    auto *textColumn = new QVBoxLayout();
    textColumn->setSpacing(3);

    auto *nameLine = new QHBoxLayout();
    nameLine->setContentsMargins(0, 0, 0, 0);
    nameLine->setSpacing(8);

    auto *nameLabel = new QLabel(name);
    nameLabel->setObjectName("rowName");
    nameLabel->setWordWrap(true);
    nameLine->addWidget(nameLabel, 0, Qt::AlignVCenter);

    if (QLabel *badge = makeBadge(tier)) {
        nameLine->addWidget(badge, 0, Qt::AlignVCenter);
    }
    nameLine->addStretch(1);
    textColumn->addLayout(nameLine);

    if (!description.isEmpty()) {
        auto *desc = new QLabel(description);
        desc->setObjectName("rowDesc");
        desc->setWordWrap(true);
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
    // Painted by the stylesheet rather than by QFrame's own line drawing, which
    // takes its colour from the palette and ignores the theme.
    auto *line = new QFrame();
    line->setObjectName("sep");
    line->setFrameShape(QFrame::NoFrame);
    if (orientation == Qt::Horizontal) {
        line->setFixedHeight(1);
    } else {
        line->setFixedWidth(1);
    }
    return line;
}

static QLabel *makeGroupLabel(const QString &text) {
    auto *label = new QLabel(text.toUpper());
    label->setObjectName("groupLabel");
    // Letter spacing is not expressible in Qt's stylesheet subset.
    QFont f = label->font();
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    label->setFont(f);
    return label;
}

static QWidget *makeScrollPage(QWidget *content) {
    auto *scroll = new QScrollArea();
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

// A dynamic property only changes the rendering after a repolish.
static void repolish(QWidget *widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

// A labelled cluster of related rows. The label is optional; short pages use a
// single unlabelled group.
struct SettingsGroup {
    QString label;
    QList<QWidget *> rows;
};

// Builds a scrollable page from a heading, a one-line introduction and an
// ordered list of groups.
static QWidget *makePage(const QString &heading,
                         const QString &lede,
                         const QList<SettingsGroup> &groups) {
    auto *content = new QFrame();
    content->setObjectName("pageContent");

    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(26, 22, 26, 26);
    layout->setSpacing(0);

    auto *title = new QLabel(heading);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    if (!lede.isEmpty()) {
        auto *sub = new QLabel(lede);
        sub->setObjectName("pageLede");
        sub->setWordWrap(true);
        layout->addSpacing(2);
        layout->addWidget(sub);
    }
    layout->addSpacing(16);

    for (int g = 0; g < groups.size(); ++g) {
        const SettingsGroup &group = groups.at(g);
        if (g > 0) layout->addSpacing(14);

        if (!group.label.isEmpty()) {
            layout->addWidget(makeGroupLabel(group.label));
            layout->addSpacing(6);
        }

        for (int i = 0; i < group.rows.size(); ++i) {
            if (i > 0) layout->addSpacing(3);
            layout->addWidget(group.rows.at(i));
        }
    }

    layout->addStretch();
    return makeScrollPage(content);
}

void MainWindow::addSection(const QString &name, QWidget *page) {
    m_sectionList->addItem(name);
    m_sectionStack->addWidget(page);
}

FluentSwitch *MainWindow::settingCheckbox(const QString &key, bool defaultValue) {
    auto *box = new FluentSwitch();
    m_switches.insert(key, box);
    QSettings settings;
    box->setChecked(settings.value(key, defaultValue).toBool());
    connect(box, &QAbstractButton::toggled, this, [this, key](bool on) {
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
    m_combos.insert(key, combo);
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
    m_spins.insert(key, spin);
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
    m_resolutionCombo = combo;

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
    return makePage("Connection",
                    "How devices find this PC, and who is allowed to mirror to it.",
    {
      {"Identity", {
        makeRow("Device name",
                "Shown in the AirPlay picker. Leave empty to keep the name from "
                "arguments.txt.",
                settingLineEdit("device_name", "XMirror")),
        makeRow("Hide computer name",
                "Drops the \"@hostname\" suffix clients see.",
                settingCheckbox("hide_hostname", true)),
      }},
      {"Access", {
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
      }},
    });
}

QWidget *MainWindow::buildAudioPage() {
    return makePage("Audio",
                    "Sound routed from the device, and how tightly it stays "
                    "clock-locked.",
    {
      {"", {
        makeRow("Audio timing",
                "Stable keeps audio clock-locked and clean. Responsive plays it "
                "sooner but it can drift out over a long session.",
                settingCombo("audio_mode", {
                    {"Stable", "stable"},
                    {"Responsive", "responsive"},
                }, effectiveAudioMode())),
        makeRow("Starting volume",
                "Level used when a device first connects.",
                settingSpinBox("volume_pct", -1, 100, 5, -1, " %", "Automatic")),
        makeRow("Gradual volume curve",
                "Makes the device's volume slider feel more even.",
                settingCheckbox("taper", false)),
        makeRow("Picture only, no sound",
                "Mirrors the screen with audio left on the device.",
                settingCheckbox("no_audio", false)),
      }},
    });
}

QWidget *MainWindow::buildVideoPage() {
    QSettings settings;

    m_fullscreenCheckbox = new FluentSwitch();
    m_fullscreenCheckbox->setChecked(
        settings.value("force_fs_enabled", false).toBool());
    connect(m_fullscreenCheckbox, &QAbstractButton::toggled,
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

    return makePage("Video",
                    "The picture. These defaults are latency-tuned -- see "
                    "docs/LOW-LATENCY-SETUP.md before changing them.",
    {
      {"Pipeline", {
        makeRow("Renderer",
                "Automatic keeps the tuned setting from arguments.txt.",
                m_rendererCombo),
        makeRow("HEVC (H.265)",
                "Same picture at about half the bitrate, decoded on the GPU. "
                "Devices that do not support it keep using H.264.",
                settingCheckbox("hevc_enabled", false)),
      }},
      {"Presentation", {
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
      }},
    });
}

QWidget *MainWindow::buildBehaviourPage() {
    QSettings settings;

    m_bleCheckbox = new FluentSwitch();
    m_bleCheckbox->setChecked(settings.value("ble_enabled", true).toBool());
    connect(m_bleCheckbox, &QAbstractButton::toggled, this, &MainWindow::toggleBle);

    m_autostartCheckbox = new FluentSwitch();
    m_autostartCheckbox->setChecked(isAutostartEnabled());
    connect(m_autostartCheckbox, &QAbstractButton::toggled,
            this, &MainWindow::toggleAutostart);

    // Tier 1: purely an application preference, applied instantly, no engine
    // restart, so it does not go through settingCheckbox().
    m_openAtLaunchCheckbox = new FluentSwitch();
    m_openAtLaunchCheckbox->setChecked(settings.value("open_at_launch", false).toBool());
    connect(m_openAtLaunchCheckbox, &QAbstractButton::toggled, this, [](bool on) {
        QSettings s;
        s.setValue("open_at_launch", on);
    });

    return makePage("Behaviour",
                    "What XMirror does when you are not looking at it.",
    {
      {"Startup", {
        makeRow("Start at login",
                "XMirror runs in the tray when you sign in to Windows.",
                m_autostartCheckbox, Tier::Instant),
        makeRow("Open this window at launch",
                "Off means XMirror starts silently in the tray.",
                m_openAtLaunchCheckbox, Tier::Instant),
      }},
      {"Session", {
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
      }},
    });
}

FluentSwitch *MainWindow::appCheckbox(const QString &key, bool defaultValue) {
    auto *box = new FluentSwitch();
    m_switches.insert(key, box);
    QSettings settings;
    box->setChecked(settings.value(key, defaultValue).toBool());
    connect(box, &QAbstractButton::toggled, this, [key](bool on) {
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

    return makePage("Window",
                    "Where the mirrored screen appears. Applied over Win32, so "
                    "nothing restarts.",
    {
      {"", {
        makeRow("Remember position and size",
                "Reopens the mirrored screen where you last left it.",
                appCheckbox("remember_geometry", false), Tier::Next),
        makeRow("Open on",
                "Which display the mirrored screen appears on.",
                appCombo("target_monitor", monitors, QString()), Tier::Next),
        makeRow("Always on top",
                "Keeps the mirrored screen above other windows.",
                appCheckbox("always_on_top", false), Tier::Next),
      }},
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

    return makePage("Advanced",
                    "The shipped defaults are tuned. Changing them can cost "
                    "latency or leave a black window.",
    {
      {"", {
        makeRow("Streaming arguments",
                "The tuned defaults live here. Changing them can cost latency or "
                "leave a black window. Restart the app to apply.",
                m_settingsBtn, Tier::None),
        makeRow("Available options",
                "Full list of supported streaming arguments.",
                m_listargsBtn, Tier::None),
        makeRow("Licence",
                "XMirror is GPLv3 and bundles third-party components.",
                m_licenseBtn, Tier::None),
        makeRow("Reset settings",
                "Returns every setting to its default. arguments.txt is left "
                "untouched.",
                resetBtn, Tier::None),
      }},
    });
}

// --- Home ------------------------------------------------------------------

const QStringList &MainWindow::fileArguments() {
    if (!m_fileArgumentsLoaded) {
        m_fileArguments = getArgumentsFromFile();
        m_fileArgumentsLoaded = true;
    }
    return m_fileArguments;
}

static QString flagValue(const QStringList &args, const QString &flag) {
    const int at = args.lastIndexOf(flag);
    return (at >= 0 && at + 1 < args.size()) ? args.at(at + 1) : QString();
}

QString MainWindow::effectiveAudioMode() {
    const QString stored = QSettings().value("audio_mode").toString();
    if (stored == "stable" || stored == "responsive") return stored;
    // "-vsync no" is the only unsynced form; absent or numeric means synced.
    return flagValue(fileArguments(), "-vsync") == "no" ? "responsive" : "stable";
}

QString MainWindow::effectiveDeviceName() {
    const QString stored = QSettings().value("device_name").toString().trimmed();
    if (!stored.isEmpty()) return stored;
    const QString fromFile = flagValue(fileArguments(), "-n");
    return fromFile.isEmpty() ? QStringLiteral("XMirror") : fromFile;
}

namespace {

struct Preset {
    const char *id;
    const char *title;
    const char *description;
    const char *audioMode;
    bool hevc;
    const char *resolution;  // empty = Automatic
};

// One choice mapped onto the settings that trade latency against quality.
// Frame rate limit is always Automatic: a cap saves CPU, not latency. Figures
// are the measured ones from the audio timing notes in this file.
const Preset kPresets[] = {
    {"latency", "Lowest latency",
     "Audio plays as it arrives, about 170 ms. The shipped default.",
     "responsive", false, ""},
    {"balanced", "Balanced",
     "Clock-locked audio, about 350 ms. Lip sync holds for hours.",
     "stable", false, ""},
    {"quality", "Best quality",
     "HEVC and a 2560 x 1440 request. Uses more GPU and network.",
     "stable", true, "2560x1440@60"},
};

// Segoe Fluent Icons on Windows 11 and Segoe MDL2 Assets on Windows 10 share
// these code points.
QString heroGlyph(const QString &state) {
    if (state == "error") return QString(QChar(0xE7BA));    // Warning
    if (state == "stopped") return QString(QChar(0xE71A));  // Stop
    return QString(QChar(0xE7F4));                          // TVMonitor
}

}  // namespace

QString MainWindow::currentPresetId() {
    QSettings settings;
    const QString audio = effectiveAudioMode();
    const bool hevc = settings.value("hevc_enabled", false).toBool();
    const QString resolution = settings.value("resolution", QString()).toString().trimmed();
    const int fps = settings.value("fps_limit", 0).toInt();

    for (const Preset &preset : kPresets) {
        if (audio == QLatin1String(preset.audioMode) && hevc == preset.hevc &&
            resolution == QLatin1String(preset.resolution) && fps <= 0) {
            return QString::fromLatin1(preset.id);
        }
    }
    return QString();
}

void MainWindow::applyPreset(const QString &id) {
    const Preset *preset = nullptr;
    for (const Preset &candidate : kPresets) {
        if (id == QLatin1String(candidate.id)) preset = &candidate;
    }
    if (!preset || currentPresetId() == id) {
        updateHome();  // undo the click's own toggle of the card
        return;
    }

    const QString audio = QString::fromLatin1(preset->audioMode);
    const QString resolution = QString::fromLatin1(preset->resolution);

    QSettings settings;
    settings.setValue("audio_mode", audio);
    settings.setValue("hevc_enabled", preset->hevc);
    settings.setValue("resolution", resolution);
    settings.setValue("fps_limit", 0);

    // Mirror the values on the section pages with their signals blocked. Each
    // control's own handler would otherwise ask for a separate engine restart.
    if (QComboBox *combo = m_combos.value("audio_mode")) {
        QSignalBlocker blocker(combo);
        combo->setCurrentIndex(combo->findData(audio));
    }
    if (FluentSwitch *hevc = m_switches.value("hevc_enabled")) {
        QSignalBlocker blocker(hevc);
        hevc->setChecked(preset->hevc);
    }
    if (m_resolutionCombo) {
        QSignalBlocker blocker(m_resolutionCombo);
        if (!resolution.isEmpty() && m_resolutionCombo->findText(resolution) < 0) {
            m_resolutionCombo->addItem(resolution);
        }
        m_resolutionCombo->setCurrentText(resolution.isEmpty()
                                              ? QString(kAutomaticResolution)
                                              : resolution);
    }
    if (QSpinBox *fps = m_spins.value("fps_limit")) {
        QSignalBlocker blocker(fps);
        fps->setValue(0);
    }

    markEngineSettingChanged();  // one restart for the whole preset
}

QWidget *MainWindow::buildHomePage() {
    auto *content = new QFrame();
    content->setObjectName("pageContent");

    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(26, 22, 26, 26);
    layout->setSpacing(0);

    auto *title = new QLabel("Home");
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    auto *lede = new QLabel("Whether XMirror is working, and the few things you "
                            "actually change.");
    lede->setObjectName("pageLede");
    lede->setWordWrap(true);
    layout->addSpacing(2);
    layout->addWidget(lede);
    layout->addSpacing(16);

    // --- status ---
    auto *hero = new QFrame();
    hero->setObjectName("hero");
    auto *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(18, 16, 18, 16);
    heroLayout->setSpacing(16);

    m_heroIcon = new QLabel();
    m_heroIcon->setObjectName("heroIcon");
    m_heroIcon->setAlignment(Qt::AlignCenter);
    const QStringList families = QFontDatabase::families();
    QFont iconFont(families.contains("Segoe Fluent Icons")
                       ? QStringLiteral("Segoe Fluent Icons")
                       : QStringLiteral("Segoe MDL2 Assets"));
    iconFont.setPixelSize(22);
    m_heroIcon->setFont(iconFont);
    heroLayout->addWidget(m_heroIcon, 0, Qt::AlignVCenter);

    auto *heroText = new QVBoxLayout();
    heroText->setSpacing(2);
    m_heroTitle = new QLabel();
    m_heroTitle->setObjectName("heroTitle");
    m_heroSub = new QLabel();
    m_heroSub->setObjectName("heroSub");
    m_heroSub->setWordWrap(true);
    heroText->addWidget(m_heroTitle);
    heroText->addWidget(m_heroSub);
    heroLayout->addLayout(heroText, 1);

    m_heroAction = new QPushButton();
    connect(m_heroAction, &QPushButton::clicked, this, [this]() {
        // Decided at click time, not from the label: the engine state can move
        // between the last repaint and the click.
        if (m_bonjourMissing) {
            installBonjourService();
        } else if (m_running && isSessionActive()) {
            restartEngineNow();
        } else {
            toggleServerFromTray();
        }
    });
    heroLayout->addWidget(m_heroAction, 0, Qt::AlignVCenter);
    layout->addWidget(hero);

    // --- quick settings ---
    // Each tile is a second view of a control on a section page, not a second
    // setting: flipping the tile drives the original control, so its handler,
    // tier and machine-policy lock all still apply.
    layout->addSpacing(18);
    layout->addWidget(makeGroupLabel("Quick settings"));
    layout->addSpacing(6);

    struct Quick {
        const char *name;
        const char *sub;
        FluentSwitch *source;
    };
    const Quick quick[] = {
        {"Ask for a PIN", "A code before mirroring starts", m_switches.value("pin_enabled")},
        {"Open fullscreen", "Fill the display", m_fullscreenCheckbox},
        {"Always on top", "Above other windows", m_switches.value("always_on_top")},
        {"Start at login", "Run in the tray", m_autostartCheckbox},
    };

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(6);
    int cell = 0;
    for (const Quick &item : quick) {
        FluentSwitch *source = item.source;
        if (!source) continue;

        auto *tile = new QFrame();
        tile->setObjectName("tile");
        auto *tileLayout = new QHBoxLayout(tile);
        tileLayout->setContentsMargins(14, 10, 12, 10);
        tileLayout->setSpacing(10);

        auto *text = new QVBoxLayout();
        text->setSpacing(1);
        auto *name = new QLabel(item.name);
        name->setObjectName("rowName");
        auto *sub = new QLabel(item.sub);
        sub->setObjectName("tileSub");
        text->addWidget(name);
        text->addWidget(sub);
        tileLayout->addLayout(text, 1);

        auto *mirror = new FluentSwitch();
        mirror->setChecked(source->isChecked());
        tileLayout->addWidget(mirror, 0, Qt::AlignVCenter);

        connect(mirror, &QAbstractButton::toggled, source,
                [source](bool on) { source->setChecked(on); });
        connect(source, &QAbstractButton::toggled, mirror, [mirror](bool on) {
            QSignalBlocker blocker(mirror);
            mirror->setChecked(on);
        });
        m_homeTiles.append(qMakePair(mirror, source));

        grid->addWidget(tile, cell / 2, cell % 2);
        ++cell;
    }
    layout->addLayout(grid);

    // --- presets ---
    layout->addSpacing(18);
    layout->addWidget(makeGroupLabel("Preset"));
    layout->addSpacing(2);
    auto *presetLede = new QLabel("Sets audio timing, HEVC, resolution and frame "
                                  "rate limit together.");
    presetLede->setObjectName("pageLede");
    presetLede->setWordWrap(true);
    layout->addWidget(presetLede);
    layout->addSpacing(8);

    auto *cards = new QHBoxLayout();
    cards->setSpacing(6);
    for (const Preset &preset : kPresets) {
        auto *card = new QPushButton();
        card->setObjectName("presetCard");
        card->setCheckable(true);
        card->setCursor(Qt::PointingHandCursor);
        card->setProperty("preset", QString::fromLatin1(preset.id));
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        card->setMinimumHeight(96);

        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(13, 11, 13, 12);
        cardLayout->setSpacing(3);
        auto *cardTitle = new QLabel(preset.title);
        cardTitle->setObjectName("presetTitle");
        auto *cardDesc = new QLabel(preset.description);
        cardDesc->setObjectName("presetDesc");
        cardDesc->setWordWrap(true);
        cardDesc->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        // Clicks land on the button, not on the labels drawn inside it.
        cardTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
        cardDesc->setAttribute(Qt::WA_TransparentForMouseEvents);
        cardLayout->addWidget(cardTitle);
        cardLayout->addWidget(cardDesc);
        cardLayout->addStretch();

        connect(card, &QPushButton::clicked, this,
                [this, id = QString::fromLatin1(preset.id)]() { applyPreset(id); });
        m_presetCards.append(card);
        cards->addWidget(card, 1);
    }
    layout->addLayout(cards);

    m_presetNote = new QLabel("Custom: your settings no longer match a preset.");
    m_presetNote->setObjectName("pageLede");
    layout->addSpacing(8);
    layout->addWidget(m_presetNote);

    layout->addStretch();

    updateHome();
    return makeScrollPage(content);
}

void MainWindow::updateHome() {
    if (m_heroIcon && m_heroTitle && m_heroSub && m_heroAction) {
        QString state;
        QString title;
        QString sub;
        QString action;
        bool primary = false;

        if (m_bonjourMissing) {
            state = "error";
            title = "Discovery unavailable";
            sub = "Bonjour Service is not installed, so devices cannot find this PC.";
            action = "Install Bonjour";
            primary = true;
        } else if (!m_running) {
            state = "stopped";
            title = "AirPlay is stopped";
            sub = "Devices cannot see this PC until you start it.";
            action = "Start AirPlay";
            primary = true;
        } else if (isSessionActive()) {
            state = "live";
            title = "Mirroring";
            sub = "A device is connected. Disconnecting restarts the receiver, "
                  "which takes about a second.";
            action = "Disconnect";
        } else {
            state = "ready";
            title = "Ready";
            sub = QString("Visible as “%1” to devices on this network.")
                      .arg(effectiveDeviceName());
            action = "Stop AirPlay";
        }

        m_heroTitle->setText(title);
        m_heroSub->setText(sub);
        m_heroAction->setText(action);
        m_heroIcon->setText(heroGlyph(state));

        if (m_heroIcon->property("state").toString() != state) {
            m_heroIcon->setProperty("state", state);
            repolish(m_heroIcon);
        }
        // Start and Install are the way forward; Stop and Disconnect are not
        // something to invite with an accent fill.
        const QString buttonName = primary ? QStringLiteral("primary") : QString();
        if (m_heroAction->objectName() != buttonName) {
            m_heroAction->setObjectName(buttonName);
            repolish(m_heroAction);
        }
    }

    // Tiles follow their source, including state changed behind their back:
    // the autostart registry, a machine-policy lock, a reset.
    for (const auto &tile : std::as_const(m_homeTiles)) {
        QSignalBlocker blocker(tile.first);
        tile.first->setChecked(tile.second->isChecked());
        tile.first->setEnabled(tile.second->isEnabled());
        tile.first->setToolTip(tile.second->toolTip());
    }

    if (!m_presetCards.isEmpty()) {
        const QString current = currentPresetId();
        for (QPushButton *card : std::as_const(m_presetCards)) {
            QSignalBlocker blocker(card);
            card->setChecked(card->property("preset").toString() == current);
        }
        if (m_presetNote) m_presetNote->setVisible(current.isEmpty());
    }
}

void MainWindow::setupUI() {
    setWindowTitle("XMirror");
    setWindowIcon(QApplication::windowIcon());
    resize(760, 560);
    setMinimumSize(660, 460);

    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- notice bar: persistent, for things that need attention ---
    // A QFrame rather than a bare QWidget: only QFrame paints a stylesheet
    // background without WA_StyledBackground being set by hand.
    m_noticeBar = new QFrame();
    m_noticeBar->setObjectName("noticeBar");
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
    m_deferralBar = new QFrame();
    m_deferralBar->setObjectName("deferralBar");
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
        nowBtn->setObjectName("primary");
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
    m_sectionList->setObjectName("rail");
    m_sectionList->setFixedWidth(200);
    m_sectionList->setFrameShape(QFrame::NoFrame);
    m_sectionList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_sectionStack = new QStackedWidget();

    bodyLayout->addWidget(m_sectionList);
    bodyLayout->addWidget(makeSeparator(Qt::Vertical));
    bodyLayout->addWidget(m_sectionStack, 1);

    // Home is listed first but built last: its quick switches mirror controls
    // that the other pages create.
    QWidget *connectionPage = buildConnectionPage();
    QWidget *videoPage = buildVideoPage();
    QWidget *audioPage = buildAudioPage();
    QWidget *behaviourPage = buildBehaviourPage();
    QWidget *windowPage = buildWindowPage();
    QWidget *advancedPage = buildAdvancedPage();

    addSection("Home", buildHomePage());
    addSection("Connection", connectionPage);
    addSection("Video", videoPage);
    addSection("Audio", audioPage);
    addSection("Behaviour", behaviourPage);
    addSection("Window", windowPage);
    addSection("Advanced", advancedPage);

    connect(m_sectionList, &QListWidget::currentRowChanged,
            m_sectionStack, &QStackedWidget::setCurrentIndex);
    m_sectionList->setCurrentRow(0);

    root->addWidget(body, 1);

    // --- footer: engine status ---
    root->addWidget(makeSeparator());

    auto *footer = new QFrame();
    footer->setObjectName("footer");
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(18, 9, 18, 9);
    footerLayout->setSpacing(9);

    // Engine state as a colour as well as a sentence, so it is readable at a
    // glance without reading.
    m_statusDot = new QLabel();
    m_statusDot->setObjectName("statusDot");
    m_statusDot->setProperty("state", "idle");
    footerLayout->addWidget(m_statusDot, 0, Qt::AlignVCenter);

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
    //
    // Only an explicit choice changes anything. Unset means "leave arguments.txt
    // alone", which for the shipped line is "-vsync no". Previously "stable"
    // was treated the same as unset, so choosing it did nothing on a default
    // install while the combo claimed clock-locked audio.
    const QString audioMode = settings.value("audio_mode").toString();
    if (audioMode == "responsive") {
        stripFlag(args, "-vsync", true);
        args << "-vsync" << "no";
    } else if (audioMode == "stable") {
        // Replace only "no". A numeric -vsync is already synced and carries a
        // deliberate A/V trim that must survive.
        const int at = args.lastIndexOf("-vsync");
        if (at >= 0 && at + 1 < args.size() && args.at(at + 1) == "no") {
            stripFlag(args, "-vsync", true);
            args << "-vsync" << "0";
        }
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
    // The hook delivers destroy events too. A window that has gone while the
    // engine keeps running -- a client that drops without the engine ending
    // its session -- must be released here. The early return below used to
    // keep the stale handle forever, so the app went on believing a device was
    // connected: settings stayed deferred and status said "Mirroring".
    if (m_mirrorHwnd && !IsWindow(reinterpret_cast<HWND>(m_mirrorHwnd))) {
        qDebug() << "[mirror] adopted window destroyed while the engine runs";
        forgetMirrorWindow();
        // Adoption stopped the fallback poll; a later connection still needs it.
        if (m_running && m_mirrorFallbackTimer) m_mirrorFallbackTimer->start(1000);
        updateStatus();
    }

    if (m_mirrorHwnd) return;  // already adopted
    tryAdoptMirrorWindow();
    // Status, tray and Home all key off the session, so tell them it started.
    if (m_mirrorHwnd) updateStatus();
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
    updateHome();  // a changed setting can leave or match a preset
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
        "vsync_set", "audio_mode", "volume_pct", "taper", "no_audio", "screensaver",
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
    const char *dotState = "idle";
    if (m_bonjourMissing) {
        status = "Discovery unavailable - Bonjour not installed";
        dotState = "error";
    } else if (m_running) {
        status = isSessionActive() ? "Mirroring" : "Waiting for a device";
        if (isSessionActive()) dotState = "live";
    } else {
        status = "AirPlay stopped";
    }

    // Every one of these can be null: updateStatus() runs during construction
    // and again after the engine stops, and the UI is built in stages.
    if (m_statusLabel) m_statusLabel->setText(status);
    if (m_statusDot && m_statusDot->property("state").toString() != dotState) {
        // A dynamic property only changes the rendering after a repolish.
        m_statusDot->setProperty("state", dotState);
        m_statusDot->style()->unpolish(m_statusDot);
        m_statusDot->style()->polish(m_statusDot);
    }
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

    updateHome();
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
