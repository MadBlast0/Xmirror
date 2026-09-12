// UpdateChecker against real GitHub responses.
//
// Needs network access. Uses a separate QSettings scope and removes the files
// and registry key it creates. Exit code 0 = pass.
#include "updatechecker.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>

namespace {

int failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

bool transient(UpdateChecker::State state) {
    return state == UpdateChecker::State::Checking || state == UpdateChecker::State::Downloading;
}

// Runs the event loop until `checker` leaves Checking/Downloading.
void settle(UpdateChecker &checker, int timeoutMs) {
    if (!transient(checker.state())) return;
    QEventLoop loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    QObject::connect(&checker, &UpdateChecker::stateChanged, &loop, [&]() {
        if (!transient(checker.state())) loop.quit();
    });
    loop.exec();
}

QJsonObject fetchJson(const QString &url) {
    QNetworkAccessManager network;
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, "XMirror-test");
    QNetworkReply *reply = network.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    return object;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    // Keeps test state away from the real application's settings.
    app.setOrganizationName("MadBlast-test");
    app.setApplicationName("XMirror-updater-test");
    qunsetenv("XMIRROR_UPDATE_REPO");

    // The version comparisons below are written against 2.1.0 (see CMakeLists).
    expect(UpdateChecker::currentVersion() == QVersionNumber(2, 1, 0), "built as version 2.1.0");

    // 1. A repository with no published release reads as up to date.
    {
        UpdateChecker checker;
        checker.check();
        settle(checker, 20000);
        expect(checker.state() == UpdateChecker::State::UpToDate ||
                   checker.state() == UpdateChecker::State::Available,
               "real repository answers without failing");
        expect(checker.lastChecked().isValid(), "last-checked time recorded");
    }

    // A real release with real digests, reshaped to look like an XMirror one.
    const QJsonObject jq = fetchJson("https://api.github.com/repos/jqlang/jq/releases/latest");
    QJsonObject smallAsset;
    for (const QJsonValue &value : jq.value("assets").toArray()) {
        if (value.toObject().value("name").toString() == "jq-linux-amd64") smallAsset = value.toObject();
    }
    expect(!smallAsset.isEmpty() && smallAsset.value("digest").toString().startsWith("sha256:"),
           "fixture asset with a sha256 digest found");

    const auto makeRelease = [&](const QString &tag, const QString &digest) {
        QJsonObject asset = smallAsset;
        asset["name"] = "XMirror-x64.msi";
        if (!digest.isNull()) asset["digest"] = digest;
        QJsonObject release = jq;
        release["tag_name"] = tag;
        release["assets"] = QJsonArray{asset};
        return release;
    };

    // 2. A newer release downloads and verifies against its digest.
    {
        UpdateChecker checker;
        checker.applyRelease(makeRelease("v99.0.0", QString()));
        expect(checker.state() == UpdateChecker::State::Available, "newer tag reads as Available");
        expect(checker.release().version == QVersionNumber(99, 0, 0), "tag v99.0.0 parsed");
        expect(checker.release().assetSha256.size() == 64, "digest extracted");
        expect(!checker.release().pageUrl.isEmpty(), "release page url accepted");
        checker.download();
        settle(checker, 120000);
        expect(checker.state() == UpdateChecker::State::Ready, "download verified against digest");
    }

    // 3. A wrong digest fails and leaves nothing behind.
    const QString downloadDir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/XMirror-update/";
    {
        UpdateChecker checker;
        checker.applyRelease(makeRelease(
            "v99.0.1", "sha256:0000000000000000000000000000000000000000000000000000000000000000"));
        checker.download();
        settle(checker, 120000);
        expect(checker.state() == UpdateChecker::State::Failed, "checksum mismatch fails");
        expect(!QFile::exists(downloadDir + "XMirror-x64.msi") &&
                   !QFile::exists(downloadDir + "XMirror-x64.msi.part"),
               "mismatched download removed");
    }

    // 4. Parsing rules.
    {
        UpdateChecker checker;
        checker.applyRelease(makeRelease("v2.1.0", QString()));
        expect(checker.state() == UpdateChecker::State::UpToDate, "same version is UpToDate");
        checker.applyRelease(makeRelease("v2.0.9", QString()));
        expect(checker.state() == UpdateChecker::State::UpToDate, "older version is UpToDate");
        checker.applyRelease(makeRelease("v3.0.0-rc1", QString()));
        expect(checker.state() == UpdateChecker::State::Failed, "suffixed tag is refused");
        checker.applyRelease(makeRelease("2.2.0", QString()));
        expect(checker.state() == UpdateChecker::State::Available, "tag without v prefix accepted");

        QJsonObject noDigest = makeRelease("v2.2.0", QString());
        QJsonArray assets = noDigest["assets"].toArray();
        QJsonObject first = assets[0].toObject();
        first.remove("digest");
        assets[0] = first;
        noDigest["assets"] = assets;
        checker.applyRelease(noDigest);
        expect(checker.state() == UpdateChecker::State::Available &&
                   checker.release().assetSha256.isEmpty(),
               "missing digest leaves the checksum empty");
        expect(!checker.canInstallInPlace(), "no digest, no in-place install");
        checker.download();
        expect(checker.state() == UpdateChecker::State::Available, "download refused without digest");

        QJsonObject foreign = makeRelease("v2.2.0", QString());
        QJsonArray foreignAssets = foreign["assets"].toArray();
        QJsonObject foreignAsset = foreignAssets[0].toObject();
        foreignAsset["browser_download_url"] = "https://example.com/XMirror-x64.msi";
        foreignAssets[0] = foreignAsset;
        foreign["assets"] = foreignAssets;
        checker.applyRelease(foreign);
        expect(checker.release().assetUrl.isEmpty(), "non-GitHub asset url rejected");
    }

    // 5. Install detection. The installed-copy check only means something on a
    // machine where the MSI is installed at its default location.
    expect(!UpdateChecker::isMsiInstallation(QCoreApplication::applicationFilePath()),
           "this test program is not an MSI installation");
    if (QFile::exists("C:/Program Files/XMirror/XMirror.exe")) {
        expect(UpdateChecker::isMsiInstallation("C:/Program Files/XMirror/XMirror.exe"),
               "installed copy detected as an MSI installation");
    }

    // Clean up everything this test created.
    QDir(downloadDir).removeRecursively();
    QSettings("HKEY_CURRENT_USER\\Software", QSettings::NativeFormat).remove("MadBlast-test");

    std::printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
