#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVersionNumber>

class QNetworkAccessManager;
class QNetworkReply;

// The newest published GitHub release, as far as updating is concerned.
struct UpdateRelease {
    QVersionNumber version;
    QString tag;
    QString pageUrl;       // the release page, for notes and manual download
    QDateTime published;

    // The MSI for this build's architecture. Empty when the release has none,
    // in which case the only way to update is from the release page.
    QString assetName;
    QString assetUrl;
    QString assetSha256;   // lowercase hex; empty when GitHub published no digest
    qint64 assetSize = 0;
};

// Checks GitHub Releases for a newer XMirror and, when asked, downloads and
// verifies its installer.
//
// Everything is asynchronous on the calling thread's event loop: a check costs
// one small HTTPS request and never blocks start-up. Nothing is downloaded
// without an explicit download() call, and nothing is run without an explicit
// launchInstaller() call.
class UpdateChecker : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,         // not checked this session
        Checking,
        UpToDate,     // includes "no release published yet"
        Available,    // a newer release exists
        Downloading,
        Ready,        // installer downloaded and verified
        Failed,       // check or download failed; see errorText()
    };

    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    static QVersionNumber currentVersion();
    static QString currentVersionString();

    State state() const { return m_state; }
    const UpdateRelease &release() const { return m_release; }
    QString errorText() const { return m_error; }
    QDateTime lastChecked() const;
    qint64 bytesReceived() const { return m_received; }
    qint64 bytesTotal() const { return m_total; }

    // True when this copy of XMirror can be upgraded in place: it was installed
    // from the MSI (not unzipped from the portable build), and the release
    // carries an MSI for this architecture with a digest to verify it against.
    bool canInstallInPlace() const;

    // Whether `exePath` is the executable of an MSI installation of XMirror.
    static bool isMsiInstallation(const QString &exePath);

    // Applies a GitHub "release" JSON object. Public so the parsing can be
    // exercised without the network.
    void applyRelease(const QJsonObject &json);

    // A file matches only if its SHA-256 equals `sha256Hex`.
    static bool fileMatchesSha256(const QString &path, const QString &sha256Hex);

public slots:
    void check();
    void download();
    void cancelDownload();
    // Starts the verified installer. The caller should quit straight after, so
    // the installer can replace the running files. Returns false if the file is
    // missing, no longer matches its digest, or could not be started.
    bool launchInstaller();

signals:
    void stateChanged();
    void progressChanged();

private:
    void setState(State state, const QString &error = QString());
    void finishCheck(QNetworkReply *reply);
    void finishDownload(QNetworkReply *reply);
    QNetworkAccessManager *network();

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    State m_state = State::Idle;
    UpdateRelease m_release;
    QString m_error;

    QFile m_partFile;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_installerPath;
    qint64 m_received = 0;
    qint64 m_total = 0;
    bool m_cancelled = false;
    bool m_writeFailed = false;
};
