#include "mainwindow.h"
#include "fluenttheme.h"
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTextStream>

#include <gst/gst.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <filesystem>
#include <shlobj.h>

#include "airplayworker.h"
#include "xmirror_api.h"
#endif

#ifdef _WIN32
// Single-instance support. A second launch must not start a second AirPlay
// engine: both would fight over the same ports, which presents as a confusing
// connection failure rather than an obvious double-launch.
//
// "Local\" scopes the mutex to this logon session, so different users on the
// same machine each get their own instance.
static const wchar_t *const kSingleInstanceMutex =
    L"Local\\MadBlast.XMirror.SingleInstance";

// Broadcast by a rejected second launch to bring the running instance forward.
// A registered message is used rather than matching on window title so that
// renaming the window cannot silently break this.
static UINT g_showSettingsMessage = 0;

// Held for the lifetime of the process so a second launch can detect this one.
// "Restart app" has to surrender it explicitly -- see releaseSingleInstanceLock().
static HANDLE g_instanceMutex = nullptr;

class ShowSettingsEventFilter : public QAbstractNativeEventFilter {
public:
    explicit ShowSettingsEventFilter(MainWindow *window) : m_window(window) {}

    bool nativeEventFilter(const QByteArray &eventType,
                           void *message,
                           qintptr *) override {
        if (eventType != "windows_generic_MSG") return false;
        MSG *msg = static_cast<MSG *>(message);
        if (msg && g_showSettingsMessage && msg->message == g_showSettingsMessage) {
            if (m_window) m_window->showSettingsWindow();
            return true;
        }
        return false;
    }

private:
    MainWindow *m_window = nullptr;
};
#endif

#ifdef _WIN32
// Drives the engine through N stop/start cycles, checking that every stop is
// clean and that handle and memory counts stay flat. This is the regression
// test for the shutdown path: before it was fixed, requestStop() was delivered
// as a queued call to a thread that was simultaneously blocked in wait(), so it
// never arrived and every stop fell through to QThread::terminate().
static FILE *g_soakLog = nullptr;

// main() reattaches stdout to the parent console, which discards any file
// redirection a caller set up. Mirror the report into a file so results are
// recoverable however the test was launched.
static void soakOut(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    if (g_soakLog) {
        va_start(ap, fmt);
        vfprintf(g_soakLog, fmt, ap);
        va_end(ap);
        fflush(g_soakLog);
    }
    fflush(stdout);
}

// --test-engine-errors : runs broken configurations through the real worker and
// checks that each ends as an error the app can show -- not as the process
// exiting -- and that a pipeline the PC cannot run falls back to software
// decoding. Writes engine-errors-report.txt next to the exe. Exit code 0 = pass.
static std::mutex g_noticeMutex;
static QString g_lastNotice;

static int runEngineErrorTest() {
    const QString logPath =
        QDir(QApplication::applicationDirPath()).filePath("engine-errors-report.txt");
    FILE *log = _wfopen(reinterpret_cast<const wchar_t *>(
                            QDir::toNativeSeparators(logPath).utf16()), L"w");
    int failures = 0;
    const auto say = [log](const QString &line) {
        const QByteArray bytes = (line + "\n").toUtf8();
        fputs(bytes.constData(), stdout);
        if (log) { fputs(bytes.constData(), log); fflush(log); }
    };

    xmirror_set_notice_callback([](const char *message) {
        std::lock_guard<std::mutex> lock(g_noticeMutex);
        g_lastNotice = QString::fromUtf8(message);
    });

    // Replaces the value of `flag` in the shipped arguments, or appends it.
    const auto withOption = [](QStringList args, const QString &flag, const QString &value) {
        const int at = args.indexOf(flag);
        if (at >= 0 && at + 1 < args.size()) {
            args[at + 1] = value;
        } else {
            args << flag << value;
        }
        return args;
    };

    struct Case {
        QString name;
        QStringList args;
        bool expectError;
        QString expectText;  // in the error, or in the notice when no error
    };
    const QStringList shipped = xMirrorDefaultArguments();
    const QList<Case> cases = {
        {"option missing its value", QStringList{"-n"}, true, "\"-n\""},
        {"command-line-only option", QStringList{"-h"}, true, "only works on the command line"},
        {"unknown option", QStringList{"-no-such-option"}, true, "arguments.txt"},
        {"bad resolution", QStringList{"-s", "huge"}, true, "\"-s\""},
        {"unusable decoder falls back to software", withOption(shipped, "-vd", "nosuchdecoder"),
         false, "software"},
        {"unbuildable parser fails cleanly", withOption(shipped, "-vp", "nosuchparser"),
         true, "video pipeline could not start"},
    };

    for (const Case &test : cases) {
        {
            std::lock_guard<std::mutex> lock(g_noticeMutex);
            g_lastNotice.clear();
        }
        QString error;
        bool gotError = false;
        AirPlayWorker worker;
        QObject::connect(&worker, &AirPlayWorker::errorOccurred, &worker,
                         [&](const QString &message) { error = message; gotError = true; },
                         Qt::DirectConnection);
        worker.setArgs(test.args);
        worker.start();

        // An error returns quickly; a working engine keeps running until told.
        const bool finishedAlone = worker.wait(test.expectError ? 20000 : 6000);
        if (!finishedAlone) {
            worker.requestStop();
            if (!worker.wait(20000)) {
                worker.terminate();
                worker.wait(1000);
            }
        }

        QString notice;
        {
            std::lock_guard<std::mutex> lock(g_noticeMutex);
            notice = g_lastNotice;
        }

        bool pass;
        if (test.expectError) {
            pass = gotError && error.contains(test.expectText, Qt::CaseInsensitive);
        } else {
            pass = !gotError && !finishedAlone && notice.contains(test.expectText, Qt::CaseInsensitive);
        }
        if (!pass) ++failures;
        say(QString("%1  %2").arg(pass ? "PASS" : "FAIL", test.name));
        if (gotError) say("      error:  " + error);
        if (!notice.isEmpty()) say("      notice: " + notice);
    }

    xmirror_set_notice_callback(nullptr);
    say(QString("\n%1 failure(s); the process is still running, so nothing called exit().").arg(failures));
    if (log) fclose(log);
    return failures ? 1 : 0;
}

static int runEngineSoakTest(int cycles, int settleMs) {
    const QString logPath =
        QDir(QApplication::applicationDirPath()).filePath("soak-report.txt");
    g_soakLog = _wfopen(reinterpret_cast<const wchar_t *>(
                            QDir::toNativeSeparators(logPath).utf16()),
                        L"w");

    struct Sample { SIZE_T workingSetKb; DWORD handles; };
    auto sample = []() -> Sample {
        Sample s{0, 0};
        PROCESS_MEMORY_COUNTERS pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            s.workingSetKb = pmc.WorkingSetSize / 1024;
        }
        GetProcessHandleCount(GetCurrentProcess(), &s.handles);
        return s;
    };

    const QStringList args = xMirrorDefaultArguments();
    soakOut("SOAK: %d cycles, settle=%dms, args: %s\n",
            cycles, settleMs, args.join(' ').toUtf8().constData());

    const Sample before = sample();
    soakOut("SOAK: baseline  handles=%lu  workingSet=%lluKB\n",
            before.handles, (unsigned long long)before.workingSetKb);

    int dirtyStops = 0;
    const int warmupCycles = (cycles >= 10) ? 5 : 1;
    Sample warm = before;

    for (int i = 1; i <= cycles; ++i) {
        soakOut("SOAK: cycle %d start\n", i);
        AirPlayWorker worker;
        worker.setArgs(args);
        worker.start();
        soakOut("SOAK:   started=%d\n", (int)worker.isRunning());

        // Let the engine actually enter start_xmirror() before asking it to stop,
        // otherwise the test proves nothing about unwinding a running engine.
        if (!worker.isRunning()) worker.wait(50);
        Sleep(settleMs);  // let the engine reach its main loop
        soakOut("SOAK:   settled, requesting stop\n");

        soakOut("SOAK:   -> requestStop()\n");
        worker.requestStop();
        soakOut("SOAK:   <- requestStop() returned\n");

        const bool stoppedOk = worker.wait(20000);
        soakOut("SOAK:   <- wait() = %d\n", (int)stoppedOk);

        if (!stoppedOk) {
            soakOut("SOAK: cycle %d did NOT stop cleanly\n", i);
            ++dirtyStops;
            worker.terminate();
            worker.wait(1000);
        }

        soakOut("SOAK:   stopped\n");

        // First-start GStreamer init (plugin registry, D3D11 device, thread
        // pools) costs a fixed ~1400 handles that are then cached and reused.
        // Measuring against the cold baseline would flag that as a leak, so
        // take a warm baseline and judge growth after it.
        if (i == warmupCycles) {
            warm = sample();
            soakOut("SOAK: warm baseline after %d cycles  handles=%lu  workingSet=%lluKB\n",
                    warmupCycles, warm.handles,
                    (unsigned long long)warm.workingSetKb);
        }

        if (i % 10 == 0) {
            const Sample now = sample();
            soakOut("SOAK: cycle %3d  handles=%lu (%+ld)  workingSet=%lluKB (%+lld)\n",
                    i, now.handles, (long)now.handles - (long)before.handles,
                    (unsigned long long)now.workingSetKb,
                    (long long)now.workingSetKb - (long long)before.workingSetKb);
            fflush(stdout);
        }
    }

    const Sample after = sample();
    const long handleDelta = (long)after.handles - (long)before.handles;
    const long long memDelta =
        (long long)after.workingSetKb - (long long)before.workingSetKb;

    soakOut("SOAK: final     handles=%lu (%+ld)  workingSet=%lluKB (%+lld)\n",
            after.handles, handleDelta,
            (unsigned long long)after.workingSetKb, memDelta);
    soakOut("SOAK: dirty stops = %d / %d\n", dirtyStops, cycles);

    const long warmHandleGrowth = (long)after.handles - (long)warm.handles;
    const long long warmMemGrowth =
        (long long)after.workingSetKb - (long long)warm.workingSetKb;
    soakOut("SOAK: growth since warm baseline: handles %+ld, workingSet %+lldKB\n",
            warmHandleGrowth, warmMemGrowth);

    // A real leak grows with every cycle. Judge the rate after warm-up, not the
    // absolute delta from a cold start.
    const int measured = cycles - warmupCycles;
    const bool handlesOk =
        (measured <= 0) || (warmHandleGrowth < (measured * 2));
    const bool stopsOk = (dirtyStops == 0);

    if (stopsOk && handlesOk) {
        soakOut("SOAK OK\n");
        return 0;
    }
    soakOut("SOAK FAILED (stopsOk=%d handlesOk=%d)\n", stopsOk, handlesOk);
    return 3;
}
#endif

static int runRuntimeSelfTest(const QString &appPath) {
    QStringList requiredFiles = {
        "xmirror-bluetooth-beacon.exe",
        "dnssd.dll",
        "mDNSResponder.exe",
        "platforms/qwindows.dll",
        "libexec/gstreamer-1.0/gst-plugin-scanner.exe",
        "resources/gstreamer-features.txt",
        "resources/gstreamer-plugins.json",
        "resources/build-manifest.json",
        "resources/bundle-files.json"
    };

    bool passed = true;
    for (const QString &relativePath : requiredFiles) {
        if (!QFileInfo::exists(QDir(appPath).filePath(relativePath))) {
            fprintf(stderr, "SELF-TEST ERROR: missing %s\n",
                    relativePath.toUtf8().constData());
            passed = false;
        }
    }

#ifdef _WIN32
    const QString dnssdPath = QDir::toNativeSeparators(
        QDir(appPath).filePath("dnssd.dll")
    );
    HMODULE dnssd = LoadLibraryW(
        reinterpret_cast<LPCWSTR>(dnssdPath.utf16())
    );
    if (!dnssd) {
        fprintf(stderr, "SELF-TEST ERROR: dnssd.dll could not be loaded\n");
        passed = false;
    } else {
        FreeLibrary(dnssd);
    }
#endif

    gst_init(nullptr, nullptr);
    GstRegistry *registry = gst_registry_get();
    QFile featureFile(QDir(appPath).filePath(
        "resources/gstreamer-features.txt"
    ));

    if (!featureFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        fprintf(stderr, "SELF-TEST ERROR: cannot read GStreamer feature list\n");
        passed = false;
    } else {
        QTextStream stream(&featureFile);
        while (!stream.atEnd()) {
            const QString featureName = stream.readLine().trimmed();
            if (featureName.isEmpty() || featureName.startsWith('#')) {
                continue;
            }

            const QByteArray featureUtf8 = featureName.toUtf8();
            GstPluginFeature *feature = gst_registry_find_feature(
                registry,
                featureUtf8.constData(),
                GST_TYPE_ELEMENT_FACTORY
            );
            if (!feature) {
                fprintf(stderr, "SELF-TEST ERROR: GStreamer feature missing: %s\n",
                        featureUtf8.constData());
                passed = false;
            } else {
                gst_object_unref(feature);
            }
        }
    }

    if (passed) {
        fprintf(stdout, "SELF-TEST OK: runtime bundle is complete\n");
        return 0;
    }
    return 2;
}

// "Restart app" launches the replacement while this process is still alive, so
// the replacement would hit ERROR_ALREADY_EXISTS on the single-instance mutex,
// treat its own predecessor as a duplicate, and exit -- leaving nothing running
// once this process quits a moment later. Dropping the mutex before spawning
// closes that window. Safe to call more than once.
void releaseSingleInstanceLock() {
#ifdef _WIN32
    if (!g_instanceMutex) return;
    ReleaseMutex(g_instanceMutex);
    CloseHandle(g_instanceMutex);
    g_instanceMutex = nullptr;
#endif
}

// Application names this app has shipped under, newest first. QSettings keys
// off the application name (HKCU\Software\MadBlast\<name>) and so does
// QStandardPaths::AppDataLocation (%APPDATA%\MadBlast\<name>), so a rename
// silently strands the previous install's configuration.
static const char *const kLegacyApplicationNames[] = {"MadMirror", "Mad-AirPlay"};

// Copies configuration forward from the newest legacy name that still has any.
// Runs on every start but is a no-op once XMirror owns settings of its own, so
// it costs one empty registry read in the steady state.
static void migrateLegacyUserData() {
    QSettings current;
    if (current.allKeys().isEmpty()) {
        for (const char *legacyName : kLegacyApplicationNames) {
            QSettings legacy(QStringLiteral("MadBlast"),
                             QString::fromLatin1(legacyName));
            const QStringList keys = legacy.allKeys();
            if (keys.isEmpty()) continue;

            for (const QString &key : keys) {
                current.setValue(key, legacy.value(key));
            }
            current.sync();
            qDebug() << "Migrated settings from legacy application name"
                     << legacyName << "-" << keys.size() << "keys";
            break;
        }
    }

    // arguments.txt is deliberately NOT activated. A legacy file carries the
    // old "-n <name>" (so the device would advertise under the previous brand)
    // and predates the current latency tuning, so adopting it silently would
    // regress both. Keep a copy next to the live config instead: visible to the
    // user, inert to the engine.
    const QString appDataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (appDataDir.isEmpty()) return;
    if (QFile::exists(appDataDir + "/arguments.txt")) return;
    if (QFile::exists(appDataDir + "/arguments.txt.legacy")) return;

    const QDir organizationDir = QFileInfo(appDataDir).dir();
    for (const char *legacyName : kLegacyApplicationNames) {
        const QString source =
            organizationDir.filePath(QString::fromLatin1(legacyName) +
                                     "/arguments.txt");
        if (!QFile::exists(source)) continue;

        QDir().mkpath(appDataDir);
        if (QFile::copy(source, appDataDir + "/arguments.txt.legacy")) {
            qDebug() << "Preserved legacy arguments.txt from" << legacyName
                     << "as arguments.txt.legacy (not active)";
        }
        break;
    }
}

#ifdef _WIN32
// --remove-user-data: run by the installer on a real uninstall, never on an
// upgrade, as the uninstalling user. Deletes everything XMirror writes outside
// its install folder: QSettings, arguments.txt, the theme cache and any
// downloaded update, under the current and both earlier brand names.
//
// It lives here rather than as declarative MSI rules because Windows Installer
// runs RemoveRegistryKey/RemoveFile rules whenever a component is removed --
// and a major upgrade removes the old product first. Those rules wiped every
// user's settings on every update.
//
// Pure Win32, before any Qt object exists: it must not show UI or claim the
// single-instance mutex from inside an installer.
static int removeUserData() {
    namespace fs = std::filesystem;

    for (const wchar_t *key : {L"Software\\MadBlast\\XMirror",
                               L"Software\\MadBlast\\MadMirror",
                               L"Software\\MadBlast\\Mad-AirPlay"}) {
        RegDeleteTreeW(HKEY_CURRENT_USER, key);
    }
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\MadBlast");  // only once empty

    // Known folders come from the process token, so they resolve to the
    // uninstalling user even though the installer's own environment may not.
    const auto knownFolder = [](REFKNOWNFOLDERID id) {
        fs::path path;
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) path = raw;
        CoTaskMemFree(raw);
        return path;
    };

    std::error_code ignored;
    for (const fs::path &root : {knownFolder(FOLDERID_RoamingAppData),
                                 knownFolder(FOLDERID_LocalAppData)}) {
        if (root.empty()) continue;
        const fs::path org = root / L"MadBlast";
        for (const wchar_t *app : {L"XMirror", L"MadMirror", L"Mad-AirPlay"}) {
            fs::remove_all(org / app, ignored);
        }
        fs::remove(org, ignored);  // removes the directory only if now empty
    }

    wchar_t temp[MAX_PATH + 1];
    if (GetTempPathW(MAX_PATH + 1, temp)) {
        fs::remove_all(fs::path(temp) / L"XMirror-update", ignored);
    }
    return 0;
}
#endif

int main(int argc, char *argv[]) {
#ifdef _WIN32
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--remove-user-data") == 0) return removeUserData();
    }
#endif

#ifdef _WIN32
    // --log <path> sends everything the engine and the GUI print to a file.
    //
    // Needed because the console attach below reopens stdout on CONOUT$, which
    // discards any redirection a caller set up -- so engine diagnostics (notably
    // "-d 1", which logs per-frame network latency) are otherwise impossible to
    // capture from a GUI-subsystem process.
    // freopen_s, not _fsopen + _dup2: this is a GUI-subsystem process, so
    // stdout is not attached to anything and _fileno(stdout) is invalid, which
    // makes _dup2 a silent no-op that loses the log entirely. freopen_s rebinds
    // the stream itself and works. The cost is that the handle denies readers
    // while the app runs, so the log is read after stopping the app.
    bool loggingToFile = false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--log") == 0) {
            FILE *fp = nullptr;
            if (freopen_s(&fp, argv[i + 1], "w", stdout) == 0 && fp) {
                freopen_s(&fp, argv[i + 1], "a", stderr);
                setvbuf(stdout, nullptr, _IOLBF, 4096);
                setvbuf(stderr, nullptr, _IOLBF, 4096);
                loggingToFile = true;
            }
            break;
        }
    }

    // if the process was started from a console (CMD/PowerShell), attach to it so we can see qDebug() output.
    if (!loggingToFile && AttachConsole(ATTACH_PARENT_PROCESS)) {
        // redirect stdout and stderr to the console
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);
        std::ios::sync_with_stdio();
    }
#endif

    QApplication app(argc, argv);
    app.setOrganizationName("MadBlast");
    app.setApplicationName("XMirror");

    // Style, palette, font and stylesheet. Installed before anything builds a
    // widget, and it keeps following the Windows light/dark setting after.
    FluentTheme::install();

    // Both QSettings and the arguments.txt directory are keyed on the
    // application name, so the renames (Mad-AirPlay -> MadMirror -> XMirror)
    // orphaned every existing user's configuration. Recover it before anything
    // reads settings.
    migrateLegacyUserData();
    app.setWindowIcon(QIcon(QApplication::applicationDirPath() + "/resources/icon.ico"));
    
    QString appPath = QApplication::applicationDirPath();
    
    QString pluginPath = QDir::toNativeSeparators(appPath + "/lib/gstreamer-1.0");
    qputenv("GST_PLUGIN_PATH", pluginPath.toUtf8());
    qputenv("GST_PLUGIN_PATH_1_0", pluginPath.toUtf8());
    qputenv("GST_PLUGIN_SYSTEM_PATH", pluginPath.toUtf8());
    qputenv("GST_PLUGIN_SYSTEM_PATH_1_0", pluginPath.toUtf8());

    QString scannerPath = QDir::toNativeSeparators(
        appPath + "/libexec/gstreamer-1.0/gst-plugin-scanner.exe"
    );
    qputenv("GST_PLUGIN_SCANNER", scannerPath.toUtf8());
    qputenv("GST_PLUGIN_SCANNER_1_0", scannerPath.toUtf8());
    qputenv(
        "GIO_EXTRA_MODULES",
        QDir::toNativeSeparators(appPath + "/lib/gio/modules").toUtf8()
    );
    qputenv(
        "FONTCONFIG_PATH",
        QDir::toNativeSeparators(appPath + "/etc/fonts").toUtf8()
    );

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString path = QDir::toNativeSeparators(appPath) + ";" + env.value("PATH");
    qputenv("PATH", path.toUtf8());

    if (app.arguments().contains("--self-test")) {
        return runRuntimeSelfTest(appPath);
    }

#ifdef _WIN32
    if (app.arguments().contains("--test-engine-errors")) {
        return runEngineErrorTest();
    }
#endif

#ifdef _WIN32
    // --soak-test [cycles]  Engine stop/start regression test. Runs headless and
    // exits; deliberately before the single-instance guard so it can be run
    // while a normal instance is live.
    {
        const QStringList argv = app.arguments();
        const int soakIndex = argv.indexOf("--soak-test");
        if (soakIndex >= 0) {
            int cycles = 50;
            if (soakIndex + 1 < argv.size()) {
                bool ok = false;
                const int parsed = argv.at(soakIndex + 1).toInt(&ok);
                if (ok && parsed > 0) cycles = parsed;
            }
            int settleMs = 250;
            const int settleIndex = argv.indexOf("--soak-settle");
            if (settleIndex >= 0 && settleIndex + 1 < argv.size()) {
                bool ok = false;
                const int parsed = argv.at(settleIndex + 1).toInt(&ok);
                if (ok && parsed >= 0) settleMs = parsed;
            }
            return runEngineSoakTest(cycles, settleMs);
        }
    }
#endif

    app.setQuitOnLastWindowClosed(false);

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, "Error", "System tray not available.");
        return 1;
    }

#ifdef _WIN32
    // Claim the single-instance mutex. Deliberately after --self-test, which is
    // a short-lived process that must be able to run alongside a live instance.
    g_showSettingsMessage =
        RegisterWindowMessageW(L"MadBlast.XMirror.ShowSettings");

    g_instanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    const bool alreadyRunning =
        (g_instanceMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS);

    if (alreadyRunning) {
        // Hand focus to the instance that is already running, then step aside.
        if (g_showSettingsMessage) {
            PostMessageW(HWND_BROADCAST, g_showSettingsMessage, 0, 0);
        }
        if (g_instanceMutex) CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        return 0;
    }
#endif

    MainWindow window;

    // Launch behaviour (plan section 7):
    //   first ever run  -> show once, so the app is not invisible on day one
    //   launched by Windows at sign-in -> never show, regardless of preference
    //   otherwise       -> show only if the user asked for it
    {
        const bool fromLogin = app.arguments().contains("--from-login");
        QSettings settings;
        const bool firstRun = !settings.value("has_run_before", false).toBool();

        if (firstRun) {
            settings.setValue("has_run_before", true);
        }
        if (!fromLogin &&
            (firstRun || settings.value("open_at_launch", false).toBool())) {
            window.showSettingsWindow();
        }
    }

#ifdef _WIN32
    ShowSettingsEventFilter showSettingsFilter(&window);
    app.installNativeEventFilter(&showSettingsFilter);

    // --test-apply : exercise the tier-3 apply path (setting change restarts the
    // engine while the application stays up), then report and quit.
    if (app.arguments().contains("--test-apply")) {
        // Give the engine time to come up, toggle a real control, then check
        // well afterwards that the engine came back. m_running is set from a
        // queued cross-thread signal, so it must never be read synchronously
        // straight after the toggle -- it is always false at that instant.
        QTimer::singleShot(7000, &app, [&window, &app]() {
            static FILE *log = _wfopen(
                reinterpret_cast<const wchar_t *>(
                    QDir(QApplication::applicationDirPath())
                        .filePath("apply-report.txt").utf16()),
                L"w");

            auto say = [](const char *text) {
                fprintf(stdout, "%s", text);
                if (log) { fprintf(log, "%s", text); fflush(log); }
                fflush(stdout);
            };

            say(window.engineRunning() ? "APPLY: engine up before   = yes\n"
                                       : "APPLY: engine up before   = NO\n");

            window.testApplyEngineChange();
            say("APPLY: setting toggled, engine restart requested\n");

            QTimer::singleShot(8000, &app, [&window, &app, say]() {
                const bool up = window.engineRunning();
                say(up ? "APPLY: engine up 8s later = yes\n"
                       : "APPLY: engine up 8s later = NO\n");
                say(up ? "APPLY OK\n" : "APPLY FAILED\n");
                if (log) { fclose(log); log = nullptr; }
                app.exit(up ? 0 : 4);
            });
        });
    }
#endif

    const int result = app.exec();

#ifdef _WIN32
    app.removeNativeEventFilter(&showSettingsFilter);
    releaseSingleInstanceLock();
#endif

    return result;
}
