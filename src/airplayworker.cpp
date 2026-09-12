#include "airplayworker.h"
#include "xmirror_api.h"

#include <QDebug>

#include <vector>

AirPlayWorker::AirPlayWorker(QObject *parent) : QThread(parent) {}

void AirPlayWorker::setArgs(const QStringList &args) {
    m_args = args;
}

void AirPlayWorker::requestStop() {
    // Order matters: raise the flag before unwinding the engine, so that if
    // start_xmirror() returns immediately the loop below already knows to exit.
    m_stopRequested.store(true);
    requestInterruption();

    // Safe to call from another thread: it sets the engine's shutdown flags and
    // quits the GLib main loop, which is what makes start_xmirror() return.
    stop_xmirror();
}

bool AirPlayWorker::stopAndWait(int timeoutMs) {
    if (!isRunning()) return true;

    m_stopRequested.store(true);
    requestInterruption();

    constexpr int kSliceMs = 100;
    for (int waited = 0; waited < timeoutMs; waited += kSliceMs) {
        // Re-issue every slice. If the engine was still starting up when the
        // previous request landed, that one was lost.
        stop_xmirror();
        if (wait(kSliceMs)) return true;
    }

    return !isRunning();
}

void AirPlayWorker::run() {
    // Build the whole argument buffer first, then take pointers into it.
    // Taking .data() while still appending would leave dangling pointers
    // behind every vector reallocation.
    std::vector<QByteArray> argBytes;
    argBytes.reserve(static_cast<size_t>(m_args.size()) + 1);
    // argv[0] is not just cosmetic: GStreamer derives g_get_prgname() from it,
    // and the D3D11 video window uses that as its taskbar title until
    // g_set_application_name() lands. Lowercase here shows up as a lowercase
    // window title, so keep the display spelling.
    argBytes.push_back(QByteArray("XMirror"));
    for (const QString &arg : m_args) {
        if (arg.trimmed().isEmpty()) continue;
        argBytes.push_back(arg.toUtf8());
    }

    std::vector<char *> argv;
    argv.reserve(argBytes.size());
    for (QByteArray &bytes : argBytes) {
        argv.push_back(bytes.data());
    }

    qDebug() << "Starting XMirror engine with arguments:" << m_args;
    emit started();

    while (!m_stopRequested.load() && !isInterruptionRequested()) {
        const int ret = start_xmirror(static_cast<int>(argv.size()), argv.data());

        // A deliberate stop also makes start_xmirror() return. Check before
        // treating the return value as an error or looping round again.
        if (m_stopRequested.load() || isInterruptionRequested()) {
            break;
        }

        if (ret != 0) {
            // The engine says why in plain language when it knows.
            const QString reason = QString::fromUtf8(xmirror_last_error()).trimmed();
            emit errorOccurred(reason.isEmpty()
                                   ? QString("The AirPlay engine stopped unexpectedly (code %1).").arg(ret)
                                   : reason);
            break;
        }
    }

    qDebug() << "XMirror engine thread finished";
    emit stopped();
}
