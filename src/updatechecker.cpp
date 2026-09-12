#include "updatechecker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#include <windows.h>
#include <msi.h>

namespace {

// Where releases are published. XMIRROR_UPDATE_REPO=owner/name points the
// check elsewhere, for testing the flow against a repository that has releases.
QString releaseRepository() {
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$"));
    const QString override = qEnvironmentVariable("XMIRROR_UPDATE_REPO");
    return valid.match(override).hasMatch() ? override : QStringLiteral("MadBlast0/Xmirror");
}

// Matches the artifact names build.ps1 produces: XMirror-x64.msi, XMirror-arm64.msi.
QString installerAssetName() {
    const QString arch = QSysInfo::buildCpuArchitecture();
    return QStringLiteral("XMirror-%1.msi").arg(arch == QLatin1String("arm64") ? "arm64" : "x64");
}

// Release JSON is data from the network. Only follow links that are plainly
// GitHub over HTTPS, whatever the payload says.
bool isGitHubUrl(const QString &text) {
    const QUrl url(text);
    return url.isValid() && url.scheme() == QLatin1String("https") &&
           url.host() == QLatin1String("github.com");
}

QString friendlyNetworkError(QNetworkReply *reply) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 403 || status == 429) {
        return QStringLiteral("GitHub is limiting requests right now. Try again later.");
    }
    switch (reply->error()) {
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::TimeoutError:
        case QNetworkReply::OperationCanceledError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::UnknownNetworkError:
            return QStringLiteral("Could not reach GitHub. Check the internet connection.");
        default:
            return reply->errorString();
    }
}

QNetworkRequest githubRequest(const QUrl &url) {
    QNetworkRequest request(url);
    // GitHub rejects API requests without a User-Agent.
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("XMirror/%1").arg(UpdateChecker::currentVersionString()));
    return request;
}

}  // namespace

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent) {}

UpdateChecker::~UpdateChecker() {
    if (m_reply) {
        m_cancelled = true;
        m_reply->abort();
    }
}

QVersionNumber UpdateChecker::currentVersion() {
    return QVersionNumber::fromString(currentVersionString());
}

QString UpdateChecker::currentVersionString() {
    return QStringLiteral(XMIRROR_VERSION);
}

QDateTime UpdateChecker::lastChecked() const {
    return QSettings().value("update_last_checked").toDateTime();
}

QNetworkAccessManager *UpdateChecker::network() {
    // Created on first use, so nothing network-related loads at start-up.
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
        m_network->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    }
    return m_network;
}

void UpdateChecker::setState(State state, const QString &error) {
    m_state = state;
    m_error = error;
    emit stateChanged();
}

bool UpdateChecker::canInstallInPlace() const {
    return !m_release.assetUrl.isEmpty() && !m_release.assetSha256.isEmpty() &&
           isMsiInstallation(QCoreApplication::applicationFilePath());
}

bool UpdateChecker::isMsiInstallation(const QString &exePath) {
    // The UpgradeCode from product.wxs. Stable across every XMirror MSI.
    static const wchar_t kUpgradeCode[] = L"{00000EAF-DCCA-458F-AB77-AAEF11CE4BA8}";

    const QString wanted = QDir::cleanPath(QFileInfo(exePath).absoluteFilePath());
    wchar_t productCode[39];
    for (DWORD i = 0;
         MsiEnumRelatedProductsW(kUpgradeCode, 0, i, productCode) == ERROR_SUCCESS; ++i) {
        // The package publishes no install location, and the user may change
        // it in the installer, but it does write DisplayIcon as
        // "[INSTALLDIR]XMirror.exe" -- which is exactly the path to compare.
        QSettings uninstall(
            QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\"
                           "CurrentVersion\\Uninstall\\") +
                QString::fromWCharArray(productCode),
            QSettings::Registry64Format);
        QString icon = uninstall.value("DisplayIcon").toString().trimmed();
        if (icon.isEmpty()) continue;
        icon.remove(QLatin1Char('"'));
        const int comma = icon.lastIndexOf(QLatin1Char(','));  // "path,0" icon index
        if (comma > icon.lastIndexOf(QLatin1Char('\\'))) icon.truncate(comma);

        const QString installed = QDir::cleanPath(QFileInfo(icon).absoluteFilePath());
        if (installed.compare(wanted, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

void UpdateChecker::check() {
    // A download in progress or a verified installer is kept, not rediscovered.
    if (m_state == State::Checking || m_state == State::Downloading ||
        m_state == State::Ready) {
        return;
    }

    QNetworkRequest request = githubRequest(QUrl(
        QStringLiteral("https://api.github.com/repos/%1/releases/latest")
            .arg(releaseRepository())));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setTransferTimeout(15000);

    QNetworkReply *reply = network()->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { finishCheck(reply); });
    setState(State::Checking);
}

void UpdateChecker::finishCheck(QNetworkReply *reply) {
    reply->deleteLater();
    if (m_reply == reply) m_reply = nullptr;

    QSettings().setValue("update_last_checked", QDateTime::currentDateTimeUtc());

    // "latest" answers 404 until the first non-draft, non-prerelease release is
    // published. That is "nothing newer", not a failure.
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 404) {
        m_release = UpdateRelease();
        setState(State::UpToDate);
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        m_release = UpdateRelease();
        setState(State::Failed, friendlyNetworkError(reply));
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        m_release = UpdateRelease();
        setState(State::Failed, QStringLiteral("GitHub returned a response XMirror could not read."));
        return;
    }
    applyRelease(doc.object());
}

void UpdateChecker::applyRelease(const QJsonObject &json) {
    UpdateRelease release;
    release.tag = json.value("tag_name").toString();

    // Tags are v2.2.0 or 2.2.0. Anything with a suffix (v2.2.0-rc1) is not
    // offered, even if it was not marked as a prerelease.
    QString number = release.tag;
    if (number.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) number.remove(0, 1);
    qsizetype suffixIndex = 0;
    release.version = QVersionNumber::fromString(number, &suffixIndex);
    if (release.version.isNull() || suffixIndex != number.size() ||
        json.value("draft").toBool() || json.value("prerelease").toBool()) {
        m_release = UpdateRelease();
        setState(State::Failed,
                 QStringLiteral("The latest release (%1) is not a version XMirror recognises.")
                     .arg(release.tag));
        return;
    }

    const QString page = json.value("html_url").toString();
    if (isGitHubUrl(page)) release.pageUrl = page;
    release.published = QDateTime::fromString(json.value("published_at").toString(), Qt::ISODate);

    const QString wanted = installerAssetName();
    static const QRegularExpression sha256Hex(QStringLiteral("^[0-9a-f]{64}$"));
    for (const QJsonValue &value : json.value("assets").toArray()) {
        const QJsonObject asset = value.toObject();
        if (asset.value("name").toString().compare(wanted, Qt::CaseInsensitive) != 0) continue;

        const QString url = asset.value("browser_download_url").toString();
        if (!isGitHubUrl(url)) break;

        release.assetName = wanted;  // our own spelling, never a name from the payload
        release.assetUrl = url;
        release.assetSize = static_cast<qint64>(asset.value("size").toDouble());

        const QString digest = asset.value("digest").toString();
        if (digest.startsWith(QLatin1String("sha256:"))) {
            const QString hex = digest.mid(7).toLower();
            if (sha256Hex.match(hex).hasMatch()) release.assetSha256 = hex;
        }
        break;
    }

    m_release = release;
    setState(release.version > currentVersion() ? State::Available : State::UpToDate);
}

void UpdateChecker::download() {
    if (m_state != State::Available && m_state != State::Failed) return;
    // No digest, no download: an installer that cannot be verified is only
    // ever offered through the release page.
    if (m_release.assetUrl.isEmpty() || m_release.assetSha256.isEmpty()) return;

    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/XMirror-update";
    QDir().mkpath(dir);
    m_installerPath = QDir(dir).filePath(m_release.assetName);
    QFile::remove(m_installerPath);

    m_partFile.setFileName(m_installerPath + ".part");
    if (!m_partFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setState(State::Failed, QStringLiteral("Could not save the download in %1.")
                                    .arg(QDir::toNativeSeparators(dir)));
        return;
    }

    m_hash.reset();
    m_received = 0;
    m_total = m_release.assetSize;
    m_cancelled = false;
    m_writeFailed = false;

    QNetworkRequest request = githubRequest(QUrl(m_release.assetUrl));
    request.setRawHeader("Accept", "application/octet-stream");
    // An inactivity timeout, not a total one: a slow link still finishes.
    request.setTransferTimeout(30000);

    QNetworkReply *reply = network()->get(request);
    m_reply = reply;

    // Written and hashed as it arrives, so a 100 MB installer is never held in
    // memory and never read twice.
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        const QByteArray chunk = reply->readAll();
        if (m_partFile.write(chunk) != chunk.size()) {
            m_writeFailed = true;
            reply->abort();
            return;
        }
        m_hash.addData(chunk);
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                m_received = received;
                if (total > 0) m_total = total;
                emit progressChanged();
            });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { finishDownload(reply); });

    setState(State::Downloading);
}

void UpdateChecker::cancelDownload() {
    if (m_state != State::Downloading || !m_reply) return;
    m_cancelled = true;
    m_reply->abort();
}

void UpdateChecker::finishDownload(QNetworkReply *reply) {
    reply->deleteLater();
    if (m_reply == reply) m_reply = nullptr;

    // Drain anything that arrived with the final signal.
    if (reply->error() == QNetworkReply::NoError) {
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty() && m_partFile.write(tail) == tail.size()) m_hash.addData(tail);
    }
    const qint64 written = m_partFile.size();
    m_partFile.close();
    const QString part = m_partFile.fileName();

    if (m_cancelled) {
        QFile::remove(part);
        setState(State::Available);
        return;
    }
    if (m_writeFailed || reply->error() != QNetworkReply::NoError) {
        QFile::remove(part);
        // A transfer timeout also surfaces as OperationCanceledError, so a disk
        // failure is told apart by its own flag rather than by the error code.
        setState(State::Failed, m_writeFailed
                                    ? QStringLiteral("Could not write the download to disk.")
                                    : friendlyNetworkError(reply));
        return;
    }
    if (m_release.assetSize > 0 && written != m_release.assetSize) {
        QFile::remove(part);
        setState(State::Failed, QStringLiteral("The download was incomplete. Try again."));
        return;
    }
    if (QString::fromLatin1(m_hash.result().toHex()) != m_release.assetSha256) {
        QFile::remove(part);
        setState(State::Failed, QStringLiteral(
            "The download did not match its published checksum and was discarded."));
        return;
    }

    QFile::remove(m_installerPath);
    if (!QFile::rename(part, m_installerPath)) {
        QFile::remove(part);
        setState(State::Failed, QStringLiteral("Could not save the downloaded installer."));
        return;
    }
    setState(State::Ready);
}

bool UpdateChecker::fileMatchesSha256(const QString &path, const QString &sha256Hex) {
    QFile file(path);
    if (sha256Hex.isEmpty() || !file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return false;
    return QString::fromLatin1(hash.result().toHex()) == sha256Hex.toLower();
}

bool UpdateChecker::launchInstaller() {
    if (m_state != State::Ready) return false;

    // Checked again immediately before running: the file sat in a user-writable
    // temp folder between download and this click.
    if (!fileMatchesSha256(m_installerPath, m_release.assetSha256)) {
        QFile::remove(m_installerPath);
        setState(State::Failed, QStringLiteral(
            "The downloaded installer changed on disk and was discarded. Try again."));
        return false;
    }

    // An absolute path, so a stray msiexec.exe earlier on PATH is never run.
    const QString msiexec =
        QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")))
            .filePath(QStringLiteral("System32/msiexec.exe"));
    const bool started = QProcess::startDetached(
        QDir::toNativeSeparators(msiexec),
        {QStringLiteral("/i"), QDir::toNativeSeparators(m_installerPath)});
    if (!started) {
        setState(State::Failed, QStringLiteral("Could not start the installer."));
    }
    return started;
}
