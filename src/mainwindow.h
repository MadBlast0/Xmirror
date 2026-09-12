#pragma once

#include <functional>
#include <string>

#include <QLabel>
#include <QMainWindow>
#include <QComboBox>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QProcess>
#include <QCheckBox>
#include <QMessageBox>
#include <QFrame>
#include <QHash>
#include <QVector>
#include <QSize>

class QMenu;
class QAction;
class QTimer;
class QListWidget;
class QStackedWidget;
class QLineEdit;
class QSpinBox;
class AirPlayWorker;
class FluentSwitch;
class QProgressBar;
class UpdateChecker;

// The built-in low-latency argument line. Exposed so the --soak-test harness in
// main.cpp exercises the engine with exactly the arguments the app ships with.
QStringList xMirrorDefaultArguments();

// Defined in main.cpp, which owns the single-instance mutex. MainWindow needs it
// for "Restart app": the replacement process must not mistake its predecessor
// for a duplicate instance.
void releaseSingleInstanceLock();

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Called from the WinEvent hook when any window in this process appears or
    // is renamed. Cheap no-op once the mirror window has been adopted.
    void notifyMirrorWindowEvent();

    // Brings the settings window up and focuses it. Used by the tray and by the
    // single-instance guard when a second launch is rejected.
    void showSettingsWindow();

    // Test hook for --test-apply. Toggles a real control so the whole tier-3
    // path runs: signal -> handler -> QSettings -> markEngineSettingChanged()
    // -> engine restart. Returns true if the engine is running again after.
    bool testApplyEngineChange();
    bool engineRunning() const { return m_running; }

    // Also reached from the mirror window's title-bar menu, which runs on a
    // GStreamer thread and posts here.
    void setMirrorAspectLock(bool locked);

    // A persistent explanation strip at the top of the window, used for things
    // the user must see and act on. Unlike a tray balloon it does not vanish
    // after three seconds. Also reached from engine notices.
    void showNotice(const QString &text,
                    const QString &actionText = QString(),
                    std::function<void()> action = nullptr);
    void hideNotice();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void toggleServerFromTray();
    void applyPendingNow();
    void applyPendingWhenIdle();
    void toggleAutostart(bool checked);
    void showLicense();
    void openSettingsFile();
    void openListArgsFile();
    void quit();
    void onAirplayStarted();
    void onAirplayStopped();
    void onAirplayError(const QString &message);
    void toggleBle(bool checked); // bluetooth
    void toggleForceFullscreen(bool checked);
    void onRendererChanged(int index);


private:
    void startServer();
    void stopServer();
    void setupTray();
    void setupUI();
    void updateStatus();
    void startBluetoothBeacon(const QString &path);
    void stopBluetoothBeacon();
    void applyRendererAndFullscreenArgs(QStringList &args);

    QProcess *m_beacon = nullptr;

    QStringList getArgumentsFromFile();
    void ensureSettingsFileExists();
    QString userArgumentsPath() const;
    QString machineArgumentsPath() const;
    QString activeArgumentsPath() const;
    QString expandEnvironmentVariables(const QString &content) const;
    bool ensureBonjourServiceInstalled();
    bool isBonjourServicePresent() const;
    void installBonjourService();

    // A persistent explanation strip at the top of the window, used for things
    // the user must see and act on. Unlike a tray balloon it does not vanish
    // after three seconds.

    // A machine-wide arguments.txt is deliberate administrator policy. Controls
    // whose flags it pins are disabled rather than silently overridden.
    bool machinePolicyActive() const;
    void applyMachinePolicyLocks();

    void resetSettingsToDefaults();
    bool isWindowsServicePresent(const std::wstring &serviceName) const;
    void restartApplication();

    bool isAutostartEnabled() const;
    void setAutostart(bool enabled);

    // --- mirror (GStreamer video) window ---------------------------------
    // The sink creates its window inside this process, so it can be found and
    // adjusted with plain Win32 calls. Adoption is event-driven, with a bounded
    // timer as a fallback in case the hook misses the creation.
    void installMirrorWindowHook();
    void removeMirrorWindowHook();
    void tryAdoptMirrorWindow();
    void forgetMirrorWindow();

    // Position/size/z-order preferences applied to the sink's window once it
    // appears. Only ever moves, resizes, retitles or restacks it -- never
    // reparents it or takes over its painting, which would fight the sink and
    // cost the zero-copy path.
    void applyMirrorWindowPreferences();
    void rememberMirrorWindowGeometry();

    // --- picture shape ----------------------------------------------------
    // The client reports the picture size at connect and on every rotation.
    // With the aspect lock on, the mirror window is sized to that shape and
    // held to it while resizing, so there are no black bars. Unlocked, the
    // window resizes freely. Toggled from the mirror window's own title-bar
    // menu or from the Window settings page.
    void onVideoSizeChanged(int width, int height);
    void fitMirrorWindowToVideo();
    void installMirrorWindowControls();
    void releaseMirrorWindowControls();
    bool mirrorAspectLocked() const;

    QSize m_videoSize;  // current picture size; empty when unknown

    QTimer *m_mirrorGeometryTimer = nullptr;

    void *m_winEventHook = nullptr;   // HWINEVENTHOOK
    void *m_mirrorHwnd = nullptr;     // HWND
    QTimer *m_mirrorFallbackTimer = nullptr;
    int m_mirrorFallbackTicks = 0;

    // --- settings window shell -------------------------------------------
    // Left rail of section names driving a stack of pages. Sections are added
    // only once they have content, so there are no empty placeholder pages.
    void addSection(const QString &name, QWidget *page);
    QWidget *buildConnectionPage();
    QWidget *buildVideoPage();
    QWidget *buildAudioPage();
    QWidget *buildBehaviourPage();
    QWidget *buildWindowPage();
    QWidget *buildAdvancedPage();

    // Tier 1/2 controls: application or window preferences. They apply
    // instantly or at the next connect, and never restart the engine, so they
    // deliberately do not go through settingCheckbox()/settingCombo().
    FluentSwitch *appCheckbox(const QString &key, bool defaultValue);
    QComboBox *appCombo(const QString &key,
                        const QVector<QPair<QString, QString>> &options,
                        const QString &defaultValue);

    // Controls bound to a QSettings key. Each writes its value and then asks
    // for an engine restart through the tier-3 path. Reading happens from
    // QSettings, never from the widgets, so argument building does not depend
    // on the UI existing.
    FluentSwitch *settingCheckbox(const QString &key, bool defaultValue);
    QComboBox *settingCombo(const QString &key,
                            const QVector<QPair<QString, QString>> &options,
                            const QString &defaultValue);
    QLineEdit *settingLineEdit(const QString &key,
                               const QString &placeholder,
                               bool isPassword = false);
    // Resolution is editable rather than a fixed list: the useful value is the
    // client's own panel size, which no preset list can cover. Presets are
    // offered for convenience, anything WxH[@R] is accepted, and a saved custom
    // value is shown rather than silently reset to "Automatic".
    QComboBox *settingResolutionCombo();

    QSpinBox *settingSpinBox(const QString &key, int lo, int hi, int step,
                             int defaultValue, const QString &suffix,
                             const QString &specialText = QString());

    QListWidget *m_sectionList = nullptr;
    QStackedWidget *m_sectionStack = nullptr;

    // --- home page ----------------------------------------------------------
    // Status, the four switches people actually flip, and presets that set
    // several engine settings at once. Built after the section pages, because
    // its quick switches mirror controls those pages own.
    QWidget *buildHomePage();
    void updateHome();
    void applyPreset(const QString &id);
    QString currentPresetId();  // empty when the settings match no preset

    // What the engine will actually do, not only what is stored: when a
    // setting has never been chosen, arguments.txt decides.
    QString effectiveAudioMode();
    QString effectiveDeviceName();
    // arguments.txt parsed once; changing it needs an app restart anyway.
    const QStringList &fileArguments();

    QStringList m_fileArguments;
    bool m_fileArgumentsLoaded = false;

    // Controls by QSettings key, so presets can reflect what they set.
    QHash<QString, FluentSwitch *> m_switches;
    QHash<QString, QComboBox *> m_combos;
    QHash<QString, QSpinBox *> m_spins;
    QComboBox *m_resolutionCombo = nullptr;

    QLabel *m_heroIcon = nullptr;
    QLabel *m_heroTitle = nullptr;
    QLabel *m_heroSub = nullptr;
    QPushButton *m_heroAction = nullptr;
    QVector<QPair<FluentSwitch *, FluentSwitch *>> m_homeTiles;  // (tile, source)
    QVector<QPushButton *> m_presetCards;
    QLabel *m_presetNote = nullptr;

    // --- updates ------------------------------------------------------------
    // A card at the top of Home, a tray item and notification, and a status
    // row on the Behaviour page, all driven by one UpdateChecker. The check
    // runs in the background shortly after launch.
    QWidget *buildUpdateCard();
    void updateUpdateUi();
    void onUpdatePrimaryAction();
    void showHomePage();

    UpdateChecker *m_updater = nullptr;
    QWidget *m_updateBox = nullptr;
    QLabel *m_updateIcon = nullptr;
    QLabel *m_updateTitle = nullptr;
    QLabel *m_updateSub = nullptr;
    QProgressBar *m_updateProgress = nullptr;
    QPushButton *m_updatePrimary = nullptr;
    QPushButton *m_updateNotes = nullptr;
    QPushButton *m_updateSkip = nullptr;
    QLabel *m_updateStatus = nullptr;      // Behaviour page row description
    QPushButton *m_checkNowButton = nullptr;
    QAction *m_updateTrayAction = nullptr;
    bool m_updateBalloonShown = false;     // the last tray balloon was ours

    // --- apply engine -----------------------------------------------------
    // Settings that map to engine arguments need the engine restarted, because
    // start_xmirror() reads its configuration exactly once. Restarting is cheap
    // and invisible when nobody is connected; when someone is mirroring it
    // would drop them, so the change is queued and offered instead.
    void markEngineSettingChanged();
    void restartEngineNow();
    void updateDeferralBar();

    // A live session is exactly "the sink has a window open".
    bool isSessionActive() const { return m_mirrorHwnd != nullptr; }

    QFrame *m_noticeBar = nullptr;
    QLabel *m_noticeLabel = nullptr;
    QPushButton *m_noticeAction = nullptr;
    std::function<void()> m_noticeCallback;
    bool m_bonjourMissing = false;

    QFrame *m_deferralBar = nullptr;
    QLabel *m_deferralLabel = nullptr;
    int m_pendingChanges = 0;
    bool m_applyWhenIdle = false;

    FluentSwitch *m_bleCheckbox = nullptr;
    FluentSwitch *m_fullscreenCheckbox = nullptr;
    QComboBox *m_rendererCombo = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_trayMenu = nullptr;
    QAction *m_statusAction = nullptr;
    QAction *m_toggleServerAction = nullptr;

    FluentSwitch *m_autostartCheckbox = nullptr;
    FluentSwitch *m_openAtLaunchCheckbox = nullptr;
    QPushButton *m_settingsBtn = nullptr;
    QPushButton *m_listargsBtn = nullptr;
    QPushButton *m_licenseBtn = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_statusDot = nullptr;

    AirPlayWorker *m_worker = nullptr;

    bool m_running = false;
    bool m_quitting = false;

    // Set when the engine stopped with an error. Restarting it automatically
    // would only fail again every second, so it waits for Retry (or a settings
    // change) instead.
    bool m_engineFailed = false;
    QString m_engineError;
};
