#include "../src/viewer/eop_catalog.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <orbitforge/application/project.hpp>

namespace orbitforge::desktop {
namespace {
QByteArray payload() {
    QFile file(QString::fromStdString(
        (std::filesystem::path(ORBITFORGE_TEST_FINALS).parent_path() / "finals2000A.data")
            .string()));
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing finals fixture");
    return file.readAll();
}
class Server : public QTcpServer {
  public:
    QByteArray bytes, etag;
    QList<QByteArray> requests;
    explicit Server(QByteArray body, int code = 200, bool truncate = false)
        : bytes(std::move(body)) {
        if (!listen(QHostAddress::LocalHost))
            throw std::runtime_error("Cannot open loopback test server");
        QObject::connect(this, &QTcpServer::newConnection, this, [this, code, truncate] {
            auto *socket = nextPendingConnection();
            QObject::connect(
                socket, &QTcpSocket::readyRead, socket, [this, socket, code, truncate] {
                    const auto request = socket->readAll();
                    if (socket->property("answered").toBool())
                        return;
                    socket->setProperty("answered", true);
                    requests.push_back(request);
                    if (!etag.isEmpty() && request.contains("If-None-Match: " + etag)) {
                        socket->write("HTTP/1.1 304 Not Modified\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    socket->write("HTTP/1.1 " + QByteArray::number(code) +
                                  " Test\r\nContent-Length: " +
                                  QByteArray::number(bytes.size() + (truncate ? 30 : 0)) +
                                  (etag.isEmpty() ? QByteArray{} : "\r\nETag: " + etag) +
                                  "\r\nConnection: close\r\n\r\n" + bytes);
                    socket->disconnectFromHost();
                });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
    QUrl url() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/finals").arg(serverPort()));
    }
};
class DataCenterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        static int argc = 1;
        static char name[] = "data_center_test";
        static char *argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
        ASSERT_TRUE(temp.isValid());
    }
    std::filesystem::path root() const { return std::filesystem::path(temp.path().toStdWString()); }
    static void complete(EopCatalog &catalog) {
        QElapsedTimer timer;
        timer.start();
        const int limit = qEnvironmentVariableIsEmpty("ORBITFORGE_LIVE_IERS_URL") ? 15000 : 120000;
        while (catalog.downloading() && timer.elapsed() < limit)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        ASSERT_FALSE(catalog.downloading()) << catalog.status().toStdString();
    }
    QTemporaryDir temp;
};
TEST_F(DataCenterTest, DownloadInstallsOnceWithoutSwitchingPinnedScenesOrDefault) {
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {}, false);
    auto original = catalog.providers().front();
    const auto lock = application::eop_data_lock(original->data_info());
    Server server(payload());
    const auto live = qEnvironmentVariable("ORBITFORGE_LIVE_IERS_URL");
    const auto endpoint = live.isEmpty() ? server.url() : QUrl(live);
    catalog.start_download(endpoint);
    complete(catalog);
    ASSERT_TRUE(catalog.last_installed().has_value()) << catalog.status().toStdString();
    EXPECT_EQ(catalog.providers().size(), 3);
    const auto &installed = catalog.providers().at(*catalog.last_installed())->data_info();
    if (live.isEmpty()) {
        EXPECT_EQ(
            installed.sha256,
            QCryptographicHash::hash(payload(), QCryptographicHash::Sha256).toHex().toStdString());
        EXPECT_EQ(installed.first_epoch, original->data_info().first_epoch);
        EXPECT_EQ(installed.last_epoch, original->data_info().last_epoch);
    } else {
        const auto time = runtime::make_builtin_time_provider();
        EXPECT_EQ(time->to_utc(installed.first_epoch).year, 1973);
    }
    EXPECT_EQ(catalog.preferred, 0);
    EXPECT_EQ(application::eop_data_lock(original->data_info()), lock);
    EXPECT_EQ(catalog.data().resolve_eop(lock)->data_info().id, original->data_info().id);
    catalog.start_download(endpoint);
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 3);
    EXPECT_TRUE(std::filesystem::is_empty(root() / "data/staging"));
}
TEST_F(DataCenterTest, CancelHttpFailureTruncationAndInvalidDataNeverInstall) {
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_C04}, {}, false);
    Server valid(payload());
    catalog.start_download(valid.url());
    catalog.cancel_download();
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 1);
    EXPECT_TRUE(catalog.status().contains(QStringLiteral("取消")));
    for (int kind = 0; kind < 3; ++kind) {
        Server failure(kind == 0 ? QByteArray("<html>invalid</html>") : payload(),
                       kind == 1 ? 404 : 200, kind == 2);
        catalog.start_download(failure.url());
        complete(catalog);
        EXPECT_EQ(catalog.providers().size(), 1);
        EXPECT_FALSE(catalog.last_installed().has_value());
        EXPECT_TRUE(std::filesystem::is_empty(root() / "data/staging"));
    }
}
TEST_F(DataCenterTest, OversizedResponseIsRejectedAndCanRetry) {
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_C04}, {}, false);
    Server large(QByteArray(16 * 1024 * 1024 + 1, 'x'));
    catalog.start_download(large.url());
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 1);
    EXPECT_TRUE(catalog.status().contains("16 MiB"));
    Server good(payload());
    catalog.start_download(good.url());
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 2);
}
TEST_F(DataCenterTest, LegacyMigrationAndPreferencesSurviveRestart) {
    const auto old = runtime::import_eop_snapshot(ORBITFORGE_TEST_C04, root() / "eop");
    auto provider = runtime::load_eop_snapshot(old);
    const auto lock = application::eop_data_lock(provider->data_info());
    {
        EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_FINALS}, root() / "eop", false);
        ASSERT_EQ(catalog.providers().size(), 2);
        EXPECT_EQ(application::eop_data_lock(catalog.providers()[1]->data_info()), lock);
        catalog.set_preferred(1);
        catalog.set_automatic_updates(false);
    }
    EopCatalog reopened(root() / "data", {ORBITFORGE_TEST_FINALS}, root() / "eop", false);
    EXPECT_EQ(reopened.preferred, 1);
    EXPECT_FALSE(reopened.automatic_updates());
    EXPECT_EQ(reopened.providers().size(), 2);
    EXPECT_TRUE(std::filesystem::exists(old));
}
TEST_F(DataCenterTest, UpdatesStartAfterFirstDownloadAndRespectDailyIntervalAndOptOut) {
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_FINALS}, {}, false);
    Server server(payload());
    const auto now = QDateTime::currentSecsSinceEpoch();
    EXPECT_TRUE(catalog.automatic_updates());
    catalog.check_updates_if_due(now, server.url());
    EXPECT_FALSE(catalog.downloading());
    catalog.start_download(server.url());
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 2);
    EXPECT_TRUE(catalog.has_full_download());
    catalog.check_updates_if_due(now, server.url());
    EXPECT_FALSE(catalog.downloading());
    catalog.set_automatic_updates(false);
    catalog.data().set_setting("finals-last-attempt", "0");
    catalog.check_updates_if_due(now, server.url());
    EXPECT_FALSE(catalog.downloading());
    catalog.set_automatic_updates(true);
    catalog.check_updates_if_due(now, server.url());
    EXPECT_TRUE(catalog.downloading());
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 2);
    EXPECT_EQ(catalog.preferred, 0);
}
TEST_F(DataCenterTest, ConditionalUpdatesSkipUnchangedBytesAndPreserveOldVersionsAcrossRestart) {
    Server server(payload());
    server.etag = "\"revision-1\"";
    const auto store = root() / "data";
    std::string first_key;
    {
        EopCatalog catalog(store, {ORBITFORGE_TEST_C04}, {}, false);
        catalog.start_download(server.url());
        complete(catalog);
        ASSERT_TRUE(catalog.last_installed());
        first_key = application::eop_data_lock(catalog.providers().back()->data_info()).sha256();
        EXPECT_FALSE(server.requests.back().contains("If-None-Match"));
    }
    EopCatalog catalog(store, {ORBITFORGE_TEST_C04}, {}, false);
    catalog.start_download(server.url());
    complete(catalog);
    EXPECT_TRUE(server.requests.back().contains("If-None-Match: \"revision-1\""));
    EXPECT_EQ(catalog.received_bytes(), 0);
    EXPECT_EQ(catalog.providers().size(), 2);
    EXPECT_TRUE(catalog.status().contains(QStringLiteral("未重复下载")));
    server.etag = "\"revision-2\"";
    // A new source revision, even with unchanged useful values, has its own full-file identity.
    server.bytes += '\n';
    catalog.data().set_setting("finals-last-attempt", "0");
    catalog.check_updates_if_due(QDateTime::currentSecsSinceEpoch(), server.url());
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 3);
    EXPECT_EQ(catalog.preferred, 0);
    EXPECT_NO_THROW((void)catalog.data().load_eop(first_key));
    EXPECT_NE(application::eop_data_lock(catalog.providers().back()->data_info()).sha256(),
              first_key);
    // A failed newer response must not replace the validator for the installed revision.
    server.etag = "\"broken-revision\"";
    server.bytes = "<html>error</html>";
    catalog.start_download(server.url());
    complete(catalog);
    EXPECT_EQ(catalog.data().setting("finals-etag"), "\"revision-2\"");
    EXPECT_EQ(catalog.providers().size(), 3);
    EXPECT_TRUE(std::filesystem::is_empty(store / "staging"));
}
} // namespace
} // namespace orbitforge::desktop
