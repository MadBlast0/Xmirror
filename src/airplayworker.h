#pragma once

#include <QThread>
#include <QStringList>

#include <atomic>

// Runs the XMirror engine on its own thread.
//
// start_xmirror() blocks for the lifetime of a session, so it cannot live on the
// GUI thread. Shutdown is cooperative: requestStop() is callable from any
// thread, sets the stop flag, and asks the engine to unwind. run() then leaves
// its loop instead of immediately re-entering start_xmirror().
class AirPlayWorker : public QThread {
    Q_OBJECT

public:
    explicit AirPlayWorker(QObject *parent = nullptr);

    void setArgs(const QStringList &args);

    // Thread-safe, idempotent. Call directly -- never through a queued
    // invocation, because the caller normally blocks in wait() straight
    // afterwards and a queued call would never be delivered.
    void requestStop();

    // Stops the engine and waits for the thread to finish, re-issuing the stop
    // while it waits.
    //
    // A single request is not enough: stop_xmirror() only unwinds an engine that
    // has already reached its GLib main loop. Asked to stop during start-up,
    // g_main_loop_quit() runs against a loop that does not exist yet, the
    // request is silently lost, and the engine then runs forever. Repeating is
    // safe -- stop_raop_server() and stop_dnssd() are both null-guarded and
    // clear their pointers.
    //
    // Returns true if the thread finished within timeoutMs.
    bool stopAndWait(int timeoutMs);

    bool stopRequested() const { return m_stopRequested.load(); }

signals:
    void started();
    void stopped();
    void errorOccurred(const QString &message);

protected:
    void run() override;

private:
    QStringList m_args;
    std::atomic<bool> m_stopRequested{false};
};
