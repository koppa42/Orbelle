#include "../src/viewer/application_update.hpp"
#include "../src/viewer/application_update_ui.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "support/ui_test_support.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QNetworkReply>
#include <QProgressBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextDocument>
#include <cstring>
#include <gtest/gtest.h>
#include <minizip/zip.h>

namespace orbelle::desktop {
namespace {
struct Response {
    int code = 200;
    QByteArray body;
    QByteArray etag;
    bool hold = false;
};
class Reply final : public QNetworkReply {
  public:
    Reply(const QNetworkRequest &request, Response response, QObject *parent)
        : QNetworkReply(parent), bytes_(std::move(response.body)) {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.code);
        setRawHeader("ETag", response.etag);
        if (response.code >= 400)
            setError(ContentAccessDenied, "simulated failure");
        open(QIODevice::ReadOnly);
        if (!response.hold)
            QTimer::singleShot(0, this, [this] {
                if (isFinished())
                    return;
                emit readyRead();
                if (isFinished())
                    return;
                setFinished(true);
                emit finished();
            });
    }
    void abort() override {
        if (isFinished())
            return;
        setError(OperationCanceledError, "cancelled");
        setFinished(true);
        emit finished();
    }
    void deliver(const QByteArray &bytes) {
        bytes_.append(bytes);
        emit readyRead();
    }
    void complete() {
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override {
        return bytes_.size() - offset_ + QIODevice::bytesAvailable();
    }

  protected:
    qint64 readData(char *data, qint64 maximum) override {
        const auto count = std::min(maximum, static_cast<qint64>(bytes_.size()) - offset_);
        if (count <= 0)
            return -1;
        std::memcpy(data, bytes_.constData() + offset_, static_cast<std::size_t>(count));
        offset_ += count;
        return count;
    }

  private:
    QByteArray bytes_;
    qint64 offset_ = 0;
};
class Network final : public QNetworkAccessManager {
  public:
    QList<Response> responses;
    QList<QNetworkRequest> requests;
    QPointer<Reply> last_reply;

  protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        requests.append(request);
        const auto response = responses.isEmpty() ? Response{503, {}, {}} : responses.takeFirst();
        last_reply = new Reply(request, response, this);
        return last_reply;
    }
};
QByteArray release_json(QString version = "0.3.2", QJsonArray assets = {}) {
    return QJsonDocument(QJsonObject{{"tag_name", "v" + version},
                                     {"draft", false},
                                     {"prerelease", false},
                                     {"body", "Release notes"},
                                     {"assets", assets}})
        .toJson();
}
bool finish(ApplicationUpdate &update) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (update.busy() && elapsed.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return !update.busy();
}
class ApplicationUpdateTest : public ::testing::Test {
  protected:
    void SetUp() override {
        static int argc = 1;
        static char name[] = "update_test";
        static char *argv[] = {name, nullptr};
        static std::unique_ptr<QApplication> app;
        if (!QCoreApplication::instance())
            app = std::make_unique<QApplication>(argc, argv);
        ASSERT_TRUE(temp.isValid());
    }
    QString settings() const { return temp.path() + "/preferences.ini"; }
    QTemporaryDir temp;
};
TEST_F(ApplicationUpdateTest, VersionsAreNumericAndOnlyCanonicalStableVersionsAreAccepted) {
    EXPECT_TRUE(newer_application_version("0.10.0", "0.9.99"));
    EXPECT_FALSE(newer_application_version("0.3.1", "0.3.1"));
    EXPECT_FALSE(newer_application_version("0.3.0", "0.3.1"));
    for (const auto *bad :
         {"01.2.3", "1.2", "1.2.3-beta", "v1.2.3", "1.2.3\n", "x1.2.3", "9999999999.0.0"}) {
        EXPECT_FALSE(newer_application_version(bad, "0.3.1")) << bad;
        EXPECT_THROW(parse_application_release(release_json(bad)), std::runtime_error) << bad;
    }
    auto object = QJsonDocument::fromJson(release_json()).object();
    object["prerelease"] = true;
    EXPECT_FALSE(parse_application_release(QJsonDocument(object).toJson()));
    EXPECT_THROW(parse_application_release("{}"), std::runtime_error);
}
TEST_F(ApplicationUpdateTest, ReleaseAssetsCannotRedirectTheUserToAnotherRepositoryOrProtocol) {
    const QString name = "Orbelle-0.3.2-win-x64-update.json";
    QJsonObject asset{{"name", name},
                      {"size", 100},
                      {"browser_download_url",
                       "https://github.com/koppa42/Orbelle/releases/download/v0.3.2/" + name}};
    EXPECT_EQ(parse_application_release(release_json("0.3.2", {asset}))->assets.size(), 1);
    EXPECT_THROW(parse_application_release(release_json("0.3.2", {asset, asset})),
                 std::runtime_error);
    for (const auto *url :
         {"http://github.com/koppa42/Orbelle/x", "https://example.com/update.json",
          "https://github.com/other/Orbelle/releases/download/v0.3.2/a"}) {
        asset["browser_download_url"] = url;
        EXPECT_THROW(parse_application_release(release_json("0.3.2", {asset})), std::runtime_error);
    }
}
TEST_F(ApplicationUpdateTest, AsyncCheckCachesMetadataAndManualCheckOverridesSkippedVersion) {
    Network network;
    network.responses = {{200, release_json(), "\"r1\""}, {304, {}, {}}, {304, {}, {}}};
    ApplicationUpdate update("0.3.1", settings(), temp.path(), nullptr, &network);
    update.check(false);
    EXPECT_EQ(update.state(), ApplicationUpdateState::checking);
    update.check(false);
    EXPECT_EQ(network.requests.size(), 1);
    ASSERT_TRUE(finish(update));
    EXPECT_EQ(update.state(), ApplicationUpdateState::available);
    update.skip_version();
    update.check(false);
    ASSERT_TRUE(finish(update));
    EXPECT_EQ(update.state(), ApplicationUpdateState::idle);
    EXPECT_EQ(network.requests[1].rawHeader("If-None-Match"), "\"r1\"");
    update.check(true);
    ASSERT_TRUE(finish(update));
    EXPECT_EQ(update.state(), ApplicationUpdateState::available);
    EXPECT_TRUE(update.manual());
    EXPECT_EQ(network.requests[0].url().host(), "api.github.com");
    EXPECT_TRUE(network.requests[0].rawHeader("Authorization").isEmpty());
}
TEST_F(ApplicationUpdateTest, Invalid304CacheRetriesOnceWithoutConditionalHeader) {
    QSettings stored(settings(), QSettings::IniFormat);
    stored.setValue("updates/release", QByteArray("damaged"));
    stored.setValue("updates/etag", QByteArray("old"));
    stored.sync();
    Network network;
    network.responses = {{304, {}, {}}, {200, release_json(), {}}};
    ApplicationUpdate update("0.3.1", settings(), temp.path(), nullptr, &network);
    update.check(true);
    ASSERT_TRUE(finish(update));
    ASSERT_EQ(network.requests.size(), 2);
    EXPECT_TRUE(network.requests[1].rawHeader("If-None-Match").isEmpty());
    EXPECT_EQ(update.state(), ApplicationUpdateState::available);
}
TEST_F(ApplicationUpdateTest, CancelledRepliesCannotChangeTheNextCheckAndFailuresRemainNonmodal) {
    Network network;
    network.responses = {{200, release_json(), {}, true}, {503, {}, {}}, {200, release_json(), {}}};
    ApplicationUpdate update("0.3.1", settings(), temp.path(), nullptr, &network);
    update.check(false);
    update.cancel();
    EXPECT_EQ(update.state(), ApplicationUpdateState::idle);
    update.check(false);
    ASSERT_TRUE(finish(update));
    EXPECT_EQ(update.state(), ApplicationUpdateState::error);
    EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
    update.check(true);
    ASSERT_TRUE(finish(update));
    EXPECT_EQ(update.state(), ApplicationUpdateState::available);
}
TEST_F(ApplicationUpdateTest, DisabledStartupNeverMakesARequestButManualCheckStillWorks) {
    Network network;
    network.responses = {{200, release_json("0.3.1"), {}}};
    ApplicationUpdate update("0.3.1", settings(), temp.path(), nullptr, &network);
    // A centrally applied preference must reach an already constructed updater.
    QSettings preferences(settings(), QSettings::IniFormat);
    preferences.setValue("updates/startup", false);
    preferences.sync();
    EXPECT_FALSE(update.startup_enabled());
    update.check(false);
    EXPECT_TRUE(network.requests.empty());
    update.check(true);
    ASSERT_TRUE(finish(update));
    EXPECT_EQ(update.state(), ApplicationUpdateState::idle);
    EXPECT_EQ(network.requests.size(), 1);
}
QByteArray fixture_hash(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
void write_fixture(const QString &path, const QByteArray &bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        throw std::runtime_error("Cannot create download fixture directory");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write download fixture file");
}
QByteArray read_fixture(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read download fixture file");
    return file.readAll();
}
void fixture_ownership(QMap<QString, QByteArray> &files) {
    files.remove("orbelle-managed-files.txt");
    files.remove("orbelle-managed.txt");
    QByteArray listing;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        listing += fixture_hash(it.value()).toHex() + "  " + it.key().toUtf8() + '\n';
    files.insert("orbelle-managed-files.txt", listing);
    files.insert("orbelle-managed.txt", "1\n" + fixture_hash(listing).toHex() + '\n');
}
struct DownloadFixture {
    QString root, current, prepared;
    QMap<QString, QByteArray> files, old_files, archives;
    QByteArray manifest;

    explicit DownloadFixture(const QString &directory)
        : root(directory + "/portable"), current(root + "/versions/0.3.1"),
          prepared(root + "/versions/0.3.2") {
        files = {{"OrbelleApp.exe", "new application"},
                 {"OrbelleLauncher.exe", "new launcher"},
                 {"OrbelleUpdater.exe", "new updater"},
                 {"qt.conf", "[Paths]\nPlugins=plugins\n"},
                 {"manifest.json", "{\"version\":\"0.3.2\"}\n"},
                 {"Qt6Core.dll", "shared Qt runtime"},
                 {"plugins/platforms/qwindows.dll", "shared Windows plugin"},
                 {"resources/earth/map.png", "shared Earth image"},
                 {"notices/LICENSE.txt", "shared license"}};
        fixture_ownership(files);
        old_files = files;
        old_files["OrbelleApp.exe"] = "old application";
        old_files["OrbelleUpdater.exe"] = "old updater";
        old_files["manifest.json"] = "{\"version\":\"0.3.1\"}\n";
        fixture_ownership(old_files);
        for (auto it = old_files.cbegin(); it != old_files.cend(); ++it)
            write_fixture(current + '/' + it.key(), it.value());
        write_fixture(root + "/Orbelle.exe", "stable launcher");
        write_fixture(root + "/orbelle-layout.txt", "1\n");
        write_fixture(root + "/active-version.txt", "0.3.1\n");
        write_fixture(root + "/.orbelle-running.lock", {});
        write_fixture(root + "/user-project.orbelle", "user project");

        QJsonArray entries, bundles;
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            entries.append(
                QJsonObject{{"path", it.key()},
                            {"bundle", bundle_for(it.key())},
                            {"size", qint64(it.value().size())},
                            {"sha256", QString::fromLatin1(fixture_hash(it.value()).toHex())}});
        for (const auto *id : {"app", "runtime", "resources"}) {
            const auto path = directory + '/' + id + ".zip";
            const auto encoded = QFile::encodeName(path);
            const auto zip = zipOpen64(encoded.constData(), APPEND_STATUS_CREATE);
            if (!zip)
                throw std::runtime_error("Cannot create download fixture ZIP");
            for (auto it = files.cbegin(); it != files.cend(); ++it) {
                if (bundle_for(it.key()) != id)
                    continue;
                zip_fileinfo info{};
                info.external_fa = uLong(0100644) << 16;
                const auto name = it.key().toUtf8();
                if (zipOpenNewFileInZip4_64(zip, name.constData(), &info, nullptr, 0, nullptr, 0,
                                            nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION, 0,
                                            -MAX_WBITS, 8, Z_DEFAULT_STRATEGY, nullptr, 0,
                                            (3 << 8) | 45, 1 << 11, 1) != ZIP_OK ||
                    zipWriteInFileInZip(zip, it.value().constData(), unsigned(it.value().size())) !=
                        ZIP_OK ||
                    zipCloseFileInZip(zip) != ZIP_OK) {
                    zipClose(zip, nullptr);
                    throw std::runtime_error("Cannot write download fixture ZIP");
                }
            }
            if (zipClose(zip, nullptr) != ZIP_OK)
                throw std::runtime_error("Cannot close download fixture ZIP");
            const auto bytes = read_fixture(path);
            archives.insert(id, bytes);
            bundles.append(
                QJsonObject{{"id", id},
                            {"name", bundle_name(id)},
                            {"size", qint64(bytes.size())},
                            {"sha256", QString::fromLatin1(fixture_hash(bytes).toHex())}});
        }
        manifest = QJsonDocument(QJsonObject{{"schema", 1},
                                             {"layout", 1},
                                             {"platform", "win-x64"},
                                             {"version", "0.3.2"},
                                             {"files", entries},
                                             {"bundles", bundles}})
                       .toJson(QJsonDocument::Compact);
    }
    static QString bundle_for(const QString &path) {
        return path.startsWith("resources/") || path.startsWith("notices/") ? "resources"
               : path.startsWith("plugins/") || path.endsWith(".dll")       ? "runtime"
                                                                            : "app";
    }
    static QString bundle_name(const QString &id) { return "Orbelle-0.3.2-win-x64-" + id + ".zip"; }
    static QString manifest_name() { return "Orbelle-0.3.2-win-x64-update.json"; }
    static QJsonObject asset(const QString &name, const QByteArray &bytes) {
        return {{"name", name},
                {"size", qint64(bytes.size())},
                {"digest", "sha256:" + QString::fromLatin1(fixture_hash(bytes).toHex())},
                {"browser_download_url",
                 "https://github.com/koppa42/Orbelle/releases/download/v0.3.2/" + name}};
    }
    QByteArray release() const {
        QJsonArray assets{asset(manifest_name(), manifest)};
        for (auto it = archives.cbegin(); it != archives.cend(); ++it)
            assets.append(asset(bundle_name(it.key()), it.value()));
        return release_json("0.3.2", assets);
    }
    QStringList downloads() const {
        return QDir(root).entryList({".update-download-*"},
                                    QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    }
    void unchanged() const {
        for (auto it = old_files.cbegin(); it != old_files.cend(); ++it)
            EXPECT_EQ(read_fixture(current + '/' + it.key()), it.value()) << it.key().toStdString();
        EXPECT_EQ(read_fixture(root + "/Orbelle.exe"), "stable launcher");
        EXPECT_EQ(read_fixture(root + "/active-version.txt"), "0.3.1\n");
        EXPECT_EQ(read_fixture(root + "/user-project.orbelle"), "user project");
        EXPECT_TRUE(QDir(root + "/versions")
                        .entryList({".staging-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                        .isEmpty());
        EXPECT_TRUE(downloads().isEmpty());
    }
    void ready() const {
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            EXPECT_EQ(read_fixture(prepared + '/' + it.key()), it.value())
                << it.key().toStdString();
        unchanged();
    }
};
TEST_F(ApplicationUpdateTest, DownloadsOnlyChangedApplicationBundleAndPreservesActiveVersion) {
    DownloadFixture fixture(temp.path());
    Network network;
    network.responses = {{200, fixture.release(), {}},
                         {200, fixture.manifest, {}},
                         {200, fixture.archives["app"], {}}};
    ApplicationUpdate update("0.3.1", settings(), fixture.current, nullptr, &network);
    ASSERT_TRUE(update.portable());
    update.check(true);
    ASSERT_TRUE(finish(update));
    ASSERT_EQ(update.state(), ApplicationUpdateState::available);
    update.download();
    ASSERT_TRUE(finish(update));
    ASSERT_EQ(update.state(), ApplicationUpdateState::ready) << update.error().toStdString();
    ASSERT_EQ(network.requests.size(), 3);
    EXPECT_EQ(network.requests[1].url().fileName(), fixture.manifest_name());
    EXPECT_EQ(network.requests[2].url().fileName(), fixture.bundle_name("app"));
    fixture.ready();
}
TEST_F(ApplicationUpdateTest, RecreatedServiceReusesPreparedVersionWithoutDownloadingBundles) {
    DownloadFixture fixture(temp.path());
    {
        Network network;
        network.responses = {{200, fixture.release(), {}},
                             {200, fixture.manifest, {}},
                             {200, fixture.archives["app"], {}}};
        ApplicationUpdate update("0.3.1", settings(), fixture.current, nullptr, &network);
        update.check(true);
        ASSERT_TRUE(finish(update));
        update.download();
        ASSERT_TRUE(finish(update));
        ASSERT_EQ(update.state(), ApplicationUpdateState::ready) << update.error().toStdString();
    }
    Network network;
    network.responses = {{200, fixture.release(), {}}, {200, fixture.manifest, {}}};
    ApplicationUpdate update("0.3.1", settings(), fixture.current, nullptr, &network);
    update.check(true);
    ASSERT_TRUE(finish(update));
    update.download();
    ASSERT_TRUE(finish(update));
    ASSERT_EQ(update.state(), ApplicationUpdateState::ready) << update.error().toStdString();
    ASSERT_EQ(network.requests.size(), 2);
    EXPECT_EQ(network.requests.back().url().fileName(), fixture.manifest_name());
    fixture.ready();
}
void expect_rejected_download(DownloadFixture &fixture, const QString &preferences,
                              const QByteArray &manifest, const QByteArray &application,
                              qsizetype expected_requests) {
    Network network;
    network.responses = {{200, fixture.release(), {}}, {200, manifest, {}}, {200, application, {}}};
    ApplicationUpdate update("0.3.1", preferences, fixture.current, nullptr, &network);
    update.check(true);
    ASSERT_TRUE(finish(update));
    ASSERT_EQ(update.state(), ApplicationUpdateState::available);
    update.download();
    ASSERT_TRUE(finish(update));
    EXPECT_EQ(update.state(), ApplicationUpdateState::error);
    EXPECT_FALSE(update.error().isEmpty());
    EXPECT_EQ(network.requests.size(), expected_requests);
    EXPECT_FALSE(QFileInfo::exists(fixture.prepared));
    fixture.unchanged();
}
TEST_F(ApplicationUpdateTest, InvalidManifestIsRejectedBeforeAnyBundleIsDownloaded) {
    DownloadFixture fixture(temp.path());
    auto manifest = QJsonDocument::fromJson(fixture.manifest).object();
    auto files = manifest["files"].toArray();
    auto file = files[0].toObject();
    file["path"] = "../outside.exe";
    files[0] = file;
    manifest["files"] = files;
    fixture.manifest = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    expect_rejected_download(fixture, settings(), fixture.manifest, {}, 2);
}
TEST_F(ApplicationUpdateTest, ManifestDigestMismatchPreservesTheCurrentInstallation) {
    DownloadFixture fixture(temp.path());
    auto changed = fixture.manifest;
    changed[0] = '[';
    expect_rejected_download(fixture, settings(), changed, {}, 2);
}
TEST_F(ApplicationUpdateTest, BundleDigestMismatchPreservesTheCurrentInstallation) {
    DownloadFixture fixture(temp.path());
    auto changed = fixture.archives["app"];
    changed[0] = char(changed[0] ^ 1);
    expect_rejected_download(fixture, settings(), fixture.manifest, changed, 3);
}
TEST_F(ApplicationUpdateTest, TruncatedBundlePreservesTheCurrentInstallation) {
    DownloadFixture fixture(temp.path());
    const auto truncated = fixture.archives["app"].first(fixture.archives["app"].size() / 2);
    expect_rejected_download(fixture, settings(), fixture.manifest, truncated, 3);
}
TEST_F(ApplicationUpdateTest, CancelledPartialBundleRemovesDownloadsAndAllowsRetry) {
    DownloadFixture fixture(temp.path());
    Network network;
    const auto partial = fixture.archives["app"].first(fixture.archives["app"].size() / 2);
    network.responses = {
        {200, fixture.release(), {}}, {200, fixture.manifest, {}}, {200, partial, {}, true}};
    ApplicationUpdate update("0.3.1", settings(), fixture.current, nullptr, &network);
    update.check(true);
    ASSERT_TRUE(finish(update));
    update.download();
    QElapsedTimer elapsed;
    elapsed.start();
    while (network.requests.size() < 3 && update.busy() && elapsed.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_EQ(network.requests.size(), 3);
    ASSERT_EQ(update.state(), ApplicationUpdateState::downloading);
    const auto replies = network.findChildren<QNetworkReply *>();
    ASSERT_FALSE(replies.empty());
    replies.back()->readyRead();
    EXPECT_EQ(update.received(), partial.size());
    EXPECT_FALSE(fixture.downloads().isEmpty());
    update.cancel();
    EXPECT_FALSE(update.busy());
    EXPECT_EQ(update.state(), ApplicationUpdateState::available);
    EXPECT_FALSE(QFileInfo::exists(fixture.prepared));
    fixture.unchanged();
    network.responses = {{200, fixture.manifest, {}}, {200, fixture.archives["app"], {}}};
    update.download();
    ASSERT_TRUE(finish(update));
    ASSERT_EQ(update.state(), ApplicationUpdateState::ready) << update.error().toStdString();
    EXPECT_EQ(network.requests.size(), 5);
    fixture.ready();
}
TEST_F(ApplicationUpdateTest, UpdateDialogDownloadsAndShowsReadyStateAtNarrowWidth) {
    const auto previous_version = QCoreApplication::applicationVersion();
    const auto restore_version = qScopeGuard(
        [previous_version] { QCoreApplication::setApplicationVersion(previous_version); });
    QCoreApplication::setApplicationVersion("0.3.1");
    apply_workbench_style(*qApp);
    DownloadFixture fixture(temp.path());
    Network network;
    network.responses = {{200, fixture.release(), {}},
                         {200, fixture.manifest, {}},
                         {200, fixture.archives["app"], {}}};
    QMainWindow window;
    QMenuBar menus;
    ApplicationUpdateUi dialog(settings(), &menus, &window, &network, fixture.current);
    dialog.resize(420, 380);
    auto *check = menus.findChild<QAction *>("checkApplicationUpdates");
    auto *download = dialog.findChild<QPushButton *>("downloadApplicationUpdate");
    ASSERT_NE(check, nullptr);
    ASSERT_NE(download, nullptr);
    EXPECT_TRUE(network.requests.empty());
    const auto wait_for_button = [download](const QString &text) {
        QElapsedTimer elapsed;
        elapsed.start();
        while ((!download->isVisible() || download->text() != text) && elapsed.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return download->isVisible() && download->text() == text;
    };
    const auto verify_layout_and_capture = [&dialog](const QString &name) {
        QCoreApplication::processEvents();
        EXPECT_EQ(dialog.width(), 420);
        QList<QRect> buttons;
        for (auto *button : dialog.findChildren<QPushButton *>()) {
            if (!button->isVisible())
                continue;
            const QRect bounds(button->mapTo(&dialog, QPoint{}), button->size());
            EXPECT_TRUE(dialog.rect().contains(bounds)) << button->text().toStdString();
            for (const auto &other : buttons)
                EXPECT_FALSE(bounds.intersects(other)) << button->text().toStdString();
            buttons.append(bounds);
        }
        const auto screenshots = qEnvironmentVariable("ORBELLE_UPDATE_SCREENSHOTS");
        if (!screenshots.isEmpty()) {
            ASSERT_TRUE(QDir().mkpath(screenshots));
            EXPECT_TRUE(dialog.grab().save(QDir(screenshots).filePath(name + ".png")));
        }
    };
    check->trigger();
    ASSERT_TRUE(wait_for_button(QString::fromUtf8("下载更新")));
    ASSERT_EQ(network.requests.size(), 1);
    verify_layout_and_capture("available");
    download->click();
    ASSERT_TRUE(wait_for_button(QString::fromUtf8("重启并更新")));
    ASSERT_EQ(network.requests.size(), 3);
    EXPECT_EQ(network.requests.back().url().fileName(), fixture.bundle_name("app"));
    verify_layout_and_capture("ready");
    fixture.ready();
    dialog.close();
}
TEST_F(ApplicationUpdateTest, UpdateDialogCoalescesProgressAndPreservesReleaseNotes) {
    const auto previous_version = QCoreApplication::applicationVersion();
    const auto restore_version = qScopeGuard(
        [previous_version] { QCoreApplication::setApplicationVersion(previous_version); });
    QCoreApplication::setApplicationVersion("0.3.1");
    apply_workbench_style(*qApp);
    DownloadFixture fixture(temp.path());
    auto release = QJsonDocument::fromJson(fixture.release()).object();
    QString release_notes;
    for (int i = 0; i < 80; ++i)
        release_notes += QStringLiteral("Update note %1: Download progress stays stable.\n").arg(i);
    release["body"] = release_notes;
    Network network;
    network.responses = {{200, QJsonDocument(release).toJson(), {}},
                         {200, fixture.manifest, {}},
                         {200, {}, {}, true}};
    QMainWindow window;
    QMenuBar menus;
    ApplicationUpdateUi dialog(settings(), &menus, &window, &network, fixture.current);
    auto *check = menus.findChild<QAction *>("checkApplicationUpdates");
    auto *download = dialog.findChild<QPushButton *>("downloadApplicationUpdate");
    auto *progress = dialog.findChild<QProgressBar *>();
    auto *notes = dialog.findChild<QTextBrowser *>();
    auto *heading = dialog.findChild<QLabel *>("applicationUpdateHeading");
    ASSERT_NE(check, nullptr);
    ASSERT_NE(download, nullptr);
    ASSERT_NE(progress, nullptr);
    ASSERT_NE(notes, nullptr);
    ASSERT_NE(heading, nullptr);
    check->trigger();
    ASSERT_TRUE(wait_for_ui([&] { return download->isVisible(); }));
    download->click();
    ASSERT_TRUE(wait_for_ui([&] { return network.requests.size() == 3; }));
    ASSERT_NE(network.last_reply, nullptr);
    EXPECT_EQ(progress->maximum(), 1000);
    EXPECT_EQ(progress->value(), 0);
    EXPECT_EQ(heading->text(), QString::fromUtf8("正在下载程序…"));
    QCoreApplication::processEvents();
    auto cursor = notes->textCursor();
    cursor.setPosition(100);
    cursor.setPosition(120, QTextCursor::KeepAnchor);
    notes->setTextCursor(cursor);
    auto *scroll = notes->verticalScrollBar();
    ASSERT_GT(scroll->maximum(), 0);
    scroll->setValue(scroll->maximum() / 2);
    const auto scroll_position = scroll->value();
    const auto selection = notes->textCursor().selectedText();
    const auto progress_bounds = progress->geometry();
    int document_changes = 0;
    QList<int> values;
    QObject observer;
    QObject::connect(notes->document(), &QTextDocument::contentsChanged, &observer,
                     [&] { ++document_changes; });
    QObject::connect(progress, &QProgressBar::valueChanged, &observer,
                     [&](int value) { values.append(value); });
    const auto application = fixture.archives["app"];
    const auto half = application.size() / 2;
    for (qsizetype offset = 0; offset < half; offset += 17)
        network.last_reply->deliver(
            application.mid(offset, std::min(qsizetype{17}, half - offset)));
    // A burst of network chunks must not repaint the dialog once per chunk.
    EXPECT_TRUE(values.isEmpty());
    const auto halfway_value = static_cast<int>(1000 * half / application.size());
    ASSERT_TRUE(wait_for_ui([&] { return progress->value() == halfway_value; }));
    EXPECT_EQ(values, QList<int>{halfway_value});
    EXPECT_EQ(document_changes, 0);
    EXPECT_EQ(scroll->value(), scroll_position);
    EXPECT_EQ(notes->textCursor().selectedText(), selection);
    EXPECT_EQ(progress->geometry(), progress_bounds);
    EXPECT_EQ(progress->maximum(), 1000);
    const auto screenshots = qEnvironmentVariable("ORBELLE_UPDATE_SCREENSHOTS");
    if (!screenshots.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(screenshots));
        EXPECT_TRUE(dialog.grab().save(QDir(screenshots).filePath("downloading.png")));
    }
    // Repeated notifications with unchanged progress must not reset the bar or document.
    for (int i = 0; i < 20; ++i)
        network.last_reply->deliver({});
    bool settled = false;
    QTimer::singleShot(100, &observer, [&] { settled = true; });
    ASSERT_TRUE(wait_for_ui([&] { return settled; }));
    EXPECT_EQ(values, QList<int>{halfway_value});
    EXPECT_EQ(document_changes, 0);
    EXPECT_EQ(scroll->value(), scroll_position);
    EXPECT_EQ(notes->textCursor().selectedText(), selection);
    network.last_reply->deliver(application.mid(half));
    network.last_reply->complete();
    EXPECT_EQ(heading->text(), QString::fromUtf8("正在校验和准备更新…"));
    EXPECT_EQ(progress->maximum(), 0);
    ASSERT_TRUE(wait_for_ui([&] {
        return download->isVisible() && download->text() == QString::fromUtf8("重启并更新");
    }));
    EXPECT_FALSE(progress->isVisible());
    EXPECT_EQ(document_changes, 0);
    fixture.ready();
    dialog.close();
}
TEST_F(ApplicationUpdateTest, PendingProgressCannotOverwriteCancellationOrFailure) {
    const auto previous_version = QCoreApplication::applicationVersion();
    const auto restore_version = qScopeGuard(
        [previous_version] { QCoreApplication::setApplicationVersion(previous_version); });
    QCoreApplication::setApplicationVersion("0.3.1");
    for (const bool cancel : {true, false}) {
        SCOPED_TRACE(cancel ? "cancel" : "failure");
        const auto directory = temp.path() + (cancel ? "/cancel" : "/failure");
        DownloadFixture fixture(directory);
        Network network;
        network.responses = {
            {200, fixture.release(), {}}, {200, fixture.manifest, {}}, {200, {}, {}, true}};
        QMainWindow window;
        QMenuBar menus;
        ApplicationUpdateUi dialog(directory + "/preferences.ini", &menus, &window, &network,
                                   fixture.current);
        auto *check = menus.findChild<QAction *>("checkApplicationUpdates");
        auto *download = dialog.findChild<QPushButton *>("downloadApplicationUpdate");
        auto *progress = dialog.findChild<QProgressBar *>();
        auto *heading = dialog.findChild<QLabel *>("applicationUpdateHeading");
        auto *notes = dialog.findChild<QTextBrowser *>();
        ASSERT_NE(check, nullptr);
        ASSERT_NE(download, nullptr);
        ASSERT_NE(progress, nullptr);
        ASSERT_NE(heading, nullptr);
        ASSERT_NE(notes, nullptr);
        check->trigger();
        ASSERT_TRUE(wait_for_ui([&] { return download->isVisible(); }));
        download->click();
        ASSERT_TRUE(wait_for_ui([&] { return network.requests.size() == 3; }));
        ASSERT_NE(network.last_reply, nullptr);
        network.last_reply->deliver(fixture.archives["app"].first(10));
        if (cancel)
            dialog.close();
        else
            network.last_reply->complete();
        const auto expected_heading = QString::fromUtf8(cancel ? "发现新版本 0.3.2" : "更新未完成");
        EXPECT_EQ(heading->text(), expected_heading);
        EXPECT_TRUE(progress->isHidden());
        if (!cancel)
            EXPECT_TRUE(notes->toPlainText().contains("verification failed"));
        bool settled = false;
        QObject observer;
        QTimer::singleShot(100, &observer, [&] { settled = true; });
        ASSERT_TRUE(wait_for_ui([&] { return settled; }));
        EXPECT_EQ(heading->text(), expected_heading);
        EXPECT_TRUE(progress->isHidden());
        fixture.unchanged();
        // Reopen after cancellation, or retry the failed download, with fresh counters.
        if (cancel) {
            network.responses = {{200, fixture.release(), {}}};
            check->trigger();
            ASSERT_TRUE(wait_for_ui([&] { return download->isVisible(); }));
        }
        network.responses = {{200, fixture.manifest, {}}, {200, fixture.archives["app"], {}}};
        download->click();
        EXPECT_EQ(progress->maximum(), 1000);
        EXPECT_EQ(progress->value(), 0);
        ASSERT_TRUE(wait_for_ui([&] {
            return download->isVisible() && download->text() == QString::fromUtf8("重启并更新");
        }));
        fixture.ready();
        dialog.close();
    }
}
TEST_F(ApplicationUpdateTest, UpdateDialogFitsNarrowWindowsWithoutAutomaticNetworkTraffic) {
    apply_workbench_style(*qApp);
    QMainWindow window;
    QMenuBar menus;
    ApplicationUpdateUi dialog(settings(), &menus, &window);
    dialog.resize(420, 380);
    dialog.show();
    QCoreApplication::processEvents();
    EXPECT_FALSE(dialog.isModal());
    EXPECT_EQ(dialog.windowTitle(), QString::fromUtf8("软件更新"));
    for (auto *button : dialog.findChildren<QPushButton *>()) {
        if (!button->isVisible())
            continue;
        const QRect bounds(button->mapTo(&dialog, QPoint{}), button->size());
        EXPECT_TRUE(dialog.rect().contains(bounds)) << button->text().toStdString();
    }
    EXPECT_EQ(dialog.findChild<QCheckBox *>(), nullptr);
    for (auto *action : menus.findChildren<QAction *>())
        EXPECT_FALSE(action->isCheckable());
    dialog.close();
}
} // namespace
} // namespace orbelle::desktop
