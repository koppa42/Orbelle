#include "../src/viewer/application_update.hpp"
#include "../src/viewer/application_update_ui.hpp"
#include "../src/viewer/workbench_style.hpp"
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
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
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

  protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        requests.append(request);
        const auto response = responses.isEmpty() ? Response{503, {}, {}} : responses.takeFirst();
        return new Reply(request, response, this);
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
    update.set_startup_enabled(false);
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
    auto *checkbox = dialog.findChild<QCheckBox *>();
    ASSERT_NE(checkbox, nullptr);
    checkbox->setChecked(false);
    QSettings saved(settings(), QSettings::IniFormat);
    EXPECT_FALSE(saved.value("updates/startup", true).toBool());
    dialog.close();
}
} // namespace
} // namespace orbelle::desktop
