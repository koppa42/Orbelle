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
#include <cmath>
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
TEST_F(DataCenterTest, DownloadSelectsCurrentDataWithoutChangingActiveProviders) {
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {}, false);
    auto original = catalog.providers().front();
    const auto lock = application::eop_data_lock(original->data_info());
    for (const auto &entry : catalog.data().snapshots()) {
        EXPECT_EQ(entry.acquisition, runtime::DataAcquisition::bundled);
        EXPECT_FALSE(entry.acquired_at_unix_seconds);
    }
    const auto before = QDateTime::currentSecsSinceEpoch();
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
    EXPECT_EQ(catalog.preferred_index(), catalog.providers().size() - 1);
    const auto receipt = catalog.data().snapshots().back();
    EXPECT_EQ(receipt.acquisition, runtime::DataAcquisition::downloaded);
    ASSERT_TRUE(receipt.acquired_at_unix_seconds);
    EXPECT_GE(*receipt.acquired_at_unix_seconds, before);
    EXPECT_LE(*receipt.acquired_at_unix_seconds, QDateTime::currentSecsSinceEpoch());
    EXPECT_EQ(catalog.inventory()[0].installation, DataInstallation::downloaded);
    EXPECT_EQ(catalog.update_status().state,
              live.isEmpty() ? DataUpdateState::unchanged : DataUpdateState::updated);
    EXPECT_TRUE(catalog.update_status().checked_at);
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
    EXPECT_EQ(catalog.update_status().state, DataUpdateState::cancelled);
    EXPECT_FALSE(catalog.update_status().checked_at);
    for (int kind = 0; kind < 3; ++kind) {
        Server failure(kind == 0 ? QByteArray("<html>invalid</html>") : payload(),
                       kind == 1 ? 404 : 200, kind == 2);
        catalog.start_download(failure.url());
        complete(catalog);
        EXPECT_EQ(catalog.providers().size(), 1);
        EXPECT_FALSE(catalog.last_installed().has_value());
        EXPECT_EQ(catalog.update_status().state, DataUpdateState::failed);
        EXPECT_FALSE(catalog.update_status().checked_at);
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
        catalog.set_automatic_updates(false);
    }
    EopCatalog reopened(root() / "data", {ORBITFORGE_TEST_FINALS}, root() / "eop", false);
    EXPECT_EQ(reopened.current_indices().size(), 2);
    EXPECT_FALSE(reopened.automatic_updates());
    EXPECT_EQ(reopened.providers().size(), 2);
    EXPECT_TRUE(std::filesystem::exists(old));
    const auto migrated = reopened.data().snapshots().back();
    EXPECT_EQ(migrated.acquisition, runtime::DataAcquisition::unknown);
    EXPECT_FALSE(migrated.acquired_at_unix_seconds);
}
TEST_F(DataCenterTest, ReimportingExistingContentSelectsItAndPersistsAcrossRestart) {
    const auto store = root() / "data";
    std::string selected_key;
    {
        EopCatalog catalog(store, {ORBITFORGE_TEST_C04}, {}, false);
        const auto source = root() / "finals.all";
        QFile file(QString::fromStdWString(source.wstring()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(payload());
        file.close();
        catalog.import_raw(source);
        selected_key = application::eop_data_lock(catalog.providers().back()->data_info()).sha256();
        ASSERT_TRUE(file.open(QIODevice::Append));
        file.write("\n");
        file.close();
        catalog.import_raw(source);
        EXPECT_EQ(catalog.preferred_index(), 2);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(payload());
        file.close();
        catalog.import_raw(source);
        EXPECT_EQ(catalog.preferred_index(), 1);
        EXPECT_EQ(catalog.current_indices().size(), 2);
        EXPECT_EQ(catalog.providers().size(), 3);
    }
    EopCatalog reopened(store, {ORBITFORGE_TEST_C04}, {}, false);
    EXPECT_EQ(
        application::eop_data_lock(reopened.providers()[reopened.preferred_index()]->data_info())
            .sha256(),
        selected_key);
    EXPECT_EQ(reopened.current_indices().size(), 2);
}
TEST_F(DataCenterTest, AutomaticUpdatesAcquireFirstFullFileAndRespectIntervalAndOptOut) {
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_FINALS}, {}, false);
    Server server(payload());
    const auto now = QDateTime::currentSecsSinceEpoch();
    EXPECT_TRUE(catalog.automatic_updates());
    catalog.check_updates_if_due(now, server.url());
    EXPECT_TRUE(catalog.downloading());
    EXPECT_EQ(catalog.update_status().state, DataUpdateState::checking);
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 2);
    EXPECT_TRUE(catalog.has_full_download());
    catalog.check_updates_if_due(now, server.url());
    EXPECT_FALSE(catalog.downloading());
    catalog.set_update_interval_hours(6);
    catalog.data().set_setting("finals-last-attempt", std::to_string(now - 5 * 3600));
    catalog.check_updates_if_due(now, server.url());
    EXPECT_FALSE(catalog.downloading());
    catalog.data().set_setting("finals-last-attempt", std::to_string(now - 6 * 3600));
    catalog.check_updates_if_due(now, server.url());
    EXPECT_TRUE(catalog.downloading());
    complete(catalog);
    catalog.set_automatic_updates(false);
    catalog.data().set_setting("finals-last-attempt", "0");
    catalog.check_updates_if_due(now, server.url());
    EXPECT_FALSE(catalog.downloading());
    catalog.set_automatic_updates(true);
    catalog.check_updates_if_due(now, server.url());
    EXPECT_TRUE(catalog.downloading());
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 2);
    EXPECT_EQ(catalog.preferred_index(), catalog.providers().size() - 1);
    EXPECT_THROW(catalog.set_update_interval_hours(0), std::invalid_argument);
    EXPECT_THROW(catalog.set_update_interval_hours(169), std::invalid_argument);
}
TEST_F(DataCenterTest, ConditionalUpdatesSkipUnchangedBytesAndPreserveOldVersionsAcrossRestart) {
    Server server(payload());
    server.etag = "\"revision-1\"";
    const auto store = root() / "data";
    std::string first_key;
    std::optional<std::int64_t> downloaded_at;
    {
        EopCatalog catalog(store, {ORBITFORGE_TEST_C04}, {}, false);
        catalog.start_download(server.url());
        complete(catalog);
        ASSERT_TRUE(catalog.last_installed());
        first_key = application::eop_data_lock(catalog.providers().back()->data_info()).sha256();
        downloaded_at = catalog.data().snapshots().back().acquired_at_unix_seconds;
        ASSERT_TRUE(downloaded_at);
        EXPECT_FALSE(server.requests.back().contains("If-None-Match"));
    }
    EopCatalog catalog(store, {ORBITFORGE_TEST_C04}, {}, false);
    const auto revision = catalog.revision();
    catalog.start_download(server.url());
    complete(catalog);
    EXPECT_TRUE(server.requests.back().contains("If-None-Match: \"revision-1\""));
    EXPECT_EQ(catalog.received_bytes(), 0);
    EXPECT_EQ(catalog.update_status().state, DataUpdateState::unchanged);
    const auto checked_at = catalog.update_status().checked_at;
    EXPECT_TRUE(checked_at);
    EXPECT_EQ(catalog.providers().size(), 2);
    EXPECT_TRUE(catalog.status().contains(QStringLiteral("未重复下载")));
    EXPECT_EQ(catalog.data().snapshots().back().acquired_at_unix_seconds, downloaded_at);
    EXPECT_EQ(catalog.revision(), revision);
    server.etag = "\"revision-2\"";
    // A new source revision, even with unchanged useful values, has its own full-file identity.
    server.bytes += '\n';
    catalog.data().set_setting("finals-last-attempt", "0");
    catalog.check_updates_if_due(QDateTime::currentSecsSinceEpoch(), server.url());
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 3);
    EXPECT_EQ(catalog.preferred_index(), catalog.providers().size() - 1);
    EXPECT_NO_THROW((void)catalog.data().load_eop(first_key));
    EXPECT_NE(application::eop_data_lock(catalog.providers().back()->data_info()).sha256(),
              first_key);
    // A failed newer response must not replace the validator for the installed revision.
    server.etag = "\"broken-revision\"";
    server.bytes = "<html>error</html>";
    const auto current_revision = catalog.revision();
    const auto current_receipt = catalog.data().snapshots().back();
    catalog.start_download(server.url());
    complete(catalog);
    EXPECT_EQ(catalog.data().setting("finals-etag"), "\"revision-2\"");
    EXPECT_EQ(catalog.providers().size(), 3);
    EXPECT_EQ(catalog.current_indices().size(), 2);
    EXPECT_EQ(catalog.revision(), current_revision);
    EXPECT_EQ(catalog.data().snapshots().back().acquired_at_unix_seconds,
              current_receipt.acquired_at_unix_seconds);
    EXPECT_TRUE(std::filesystem::is_empty(store / "staging"));
}
TEST_F(DataCenterTest, InventorySeparatesInstallationCoverageAndUpdateCapabilities) {
    EopCatalog empty(root() / "empty", {}, {}, false);
    const auto missing = empty.inventory();
    ASSERT_EQ(missing.size(), 3);
    EXPECT_EQ(missing[0].installation, DataInstallation::missing);
    EXPECT_FALSE(missing[0].last.isValid());
    EXPECT_TRUE(missing[0].online_updates);
    EXPECT_TRUE(missing[1].manifest_import);
    EXPECT_FALSE(missing[1].online_updates);
    EXPECT_EQ(missing[2].product, DataProduct::utc);
    EXPECT_EQ(missing[2].installation, DataInstallation::builtin);
    EXPECT_TRUE(missing[2].exclusive_end);
    EXPECT_FALSE(missing[2].online_updates);
    EXPECT_FALSE(missing[2].extrapolation);
    EXPECT_FALSE(missing[2].acquired_at);

    EopCatalog samples(root() / "samples", {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {},
                       false);
    const auto installed = samples.inventory();
    EXPECT_EQ(installed[0].installation, DataInstallation::bundled_sample);
    EXPECT_EQ(installed[1].installation, DataInstallation::bundled_sample);
    EXPECT_FALSE(installed[0].acquired_at);
    EXPECT_TRUE(installed[0].file_size);
    EXPECT_TRUE(std::filesystem::exists(installed[0].file));
    EXPECT_EQ(installed[1].last.date(), QDate(2000, 1, 2));
    EXPECT_EQ(samples.update_status().state, DataUpdateState::never_checked);
    samples.import_snapshot(ORBITFORGE_TEST_C04);
    const auto imported = samples.inventory();
    EXPECT_EQ(imported[1].installation, DataInstallation::local_file);
    EXPECT_TRUE(imported[1].acquired_at);
    EXPECT_EQ(imported[1].last, installed[1].last);
}
TEST_F(DataCenterTest, LocalReplacementMustNotReuseAnOlderFilesOnlineVerification) {
    Server server(payload());
    server.etag = "\"server-file\"";
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_FINALS}, {}, false);
    catalog.start_download(server.url());
    complete(catalog);
    const auto verified = catalog.update_status().checked_sha256;
    QFile imported(temp.filePath("different.all"));
    ASSERT_TRUE(imported.open(QIODevice::WriteOnly));
    imported.write(payload() + "\n");
    imported.close();
    catalog.import_raw(std::filesystem::path(imported.fileName().toStdWString()));
    EXPECT_EQ(catalog.update_status().state, DataUpdateState::local_changed);
    EXPECT_EQ(catalog.inventory()[0].installation, DataInstallation::local_file);
    catalog.start_download(server.url());
    complete(catalog);
    ASSERT_EQ(server.requests.size(), 2);
    EXPECT_FALSE(server.requests.back().contains("If-None-Match"));
    EXPECT_EQ(catalog.update_status().state, DataUpdateState::updated);
    EXPECT_EQ(catalog.update_status().checked_sha256, verified);
    EXPECT_EQ(catalog.inventory()[0].sha256, verified);
    catalog.import_snapshot(ORBITFORGE_TEST_FINALS);
    EXPECT_FALSE(catalog.has_full_download());
}
TEST_F(DataCenterTest, UpdateResultsAndPreferencesSurviveFailureCancellationAndRestart) {
    const auto store = root() / "data";
    std::optional<qint64> checked_at, downloaded_at;
    QString verified;
    {
        EopCatalog catalog(store, {}, {}, false);
        Server server(payload());
        catalog.start_download(server.url());
        complete(catalog);
        checked_at = catalog.update_status().checked_at;
        verified = catalog.update_status().checked_sha256;
        downloaded_at = catalog.inventory()[0].acquired_at;
        Server bad("invalid", 500);
        catalog.start_download(bad.url());
        complete(catalog);
        EXPECT_EQ(catalog.update_status().state, DataUpdateState::failed);
        EXPECT_EQ(catalog.update_status().checked_at, checked_at);
        EXPECT_EQ(catalog.inventory()[0].acquired_at, downloaded_at);
        catalog.set_update_interval_hours(168);
        catalog.set_automatic_updates(false);
    }
    {
        EopCatalog catalog(store, {}, {}, false);
        EXPECT_EQ(catalog.update_status().state, DataUpdateState::failed);
        EXPECT_EQ(catalog.update_status().checked_at, checked_at);
        EXPECT_EQ(catalog.update_status().checked_sha256, verified);
        EXPECT_EQ(catalog.update_interval_hours(), 168);
        EXPECT_FALSE(catalog.automatic_updates());
        Server server(payload());
        catalog.start_download(server.url());
        catalog.cancel_download();
        complete(catalog);
        EXPECT_EQ(catalog.update_status().state, DataUpdateState::cancelled);
        EXPECT_EQ(catalog.update_status().checked_at, checked_at);
        EXPECT_EQ(catalog.inventory()[0].acquired_at, downloaded_at);
    }
    EopCatalog reopened(store, {}, {}, false);
    EXPECT_EQ(reopened.update_status().state, DataUpdateState::cancelled);
    EXPECT_EQ(reopened.update_status().checked_at, checked_at);
}
TEST_F(DataCenterTest, InterruptedAndLegacyChecksNeverClaimThatCurrentDataIsLatest) {
    const auto store = root() / "data";
    {
        runtime::DataCatalog data(store);
        data.set_setting("finals-last-attempt", "1234567890");
        data.set_setting("finals-status", "数据已是最新");
    }
    {
        EopCatalog legacy(store, {}, {}, false);
        EXPECT_EQ(legacy.update_status().state, DataUpdateState::never_checked);
        EXPECT_FALSE(legacy.update_status().checked_at);
        legacy.data().set_setting("finals-update-record",
                                  R"({"schema":1,"state":1,"checked_at":"1234567800"})");
    }
    EopCatalog interrupted(store, {}, {}, false);
    EXPECT_EQ(interrupted.update_status().state, DataUpdateState::interrupted);
    EXPECT_EQ(interrupted.update_status().attempted_at, 1234567890);
    EXPECT_EQ(interrupted.update_status().checked_at, 1234567800);
}
TEST_F(DataCenterTest, ExtrapolationPoliciesAreIndependentPersistentAndNeverChangeActiveProviders) {
    const auto store = root() / "data";
    EopCatalog::Provider active;
    core::Epoch begin{0}, end{0};
    {
        EopCatalog catalog(store, {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {}, false);
        EXPECT_EQ(catalog.extrapolation_days(0), 0);
        EXPECT_EQ(catalog.extrapolation_days(1), 0);
        const auto &info = catalog.providers()[0]->data_info();
        begin = core::Epoch{info.last_epoch.tai_nanoseconds() + 1'000'000'000};
        end = core::Epoch{begin.tai_nanoseconds() + 60'000'000'000};
        EXPECT_THROW((void)catalog.for_interval(begin, end), core::EopCoverageError);
        catalog.set_extrapolation_days(0, 2);
        active = catalog.for_interval(begin, end);
        EXPECT_EQ(active->data_info().extrapolation_days, 2);
        EXPECT_EQ(active->data_info().last_epoch, info.last_epoch);
        EXPECT_EQ(catalog.extrapolation_days(1), 0);
        EXPECT_EQ(
            catalog.for_interval(info.first_epoch, info.last_epoch)->data_info().extrapolation_days,
            0);
        catalog.set_extrapolation_days(0, 0);
        EXPECT_THROW((void)catalog.for_interval(begin, end), core::EopCoverageError);
        EXPECT_NO_THROW((void)active->at(end));
        catalog.set_extrapolation_days(0, 3);
        EXPECT_THROW(catalog.set_extrapolation_days(0, 31), std::invalid_argument);
        EXPECT_THROW(catalog.set_extrapolation_days(1, -1), std::invalid_argument);
    }
    EopCatalog reopened(store, {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {}, false);
    EXPECT_EQ(reopened.extrapolation_days(0), 3);
    EXPECT_EQ(reopened.extrapolation_days(1), 0);
    EXPECT_EQ(active->data_info().extrapolation_days, 2);
    EXPECT_EQ(reopened.for_interval(begin, end)->data_info().extrapolation_days, 3);
    reopened.data().set_setting("extrapolation-finals2000a-days", "invalid");
    EXPECT_EQ(reopened.extrapolation_days(0), 0);
}
TEST_F(DataCenterTest, LimitedExtrapolationIsContinuousDifferentiableAndBoundedAtBothEnds) {
    constexpr std::int64_t day = 86'400'000'000'000;
    for (const auto *manifest : {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}) {
        const auto strict = runtime::load_eop_snapshot(manifest);
        const auto extended = runtime::load_eop_snapshot(manifest, 2);
        const auto &info = strict->data_info();
        EXPECT_EQ(extended->data_info().sha256, info.sha256);
        EXPECT_EQ(extended->data_info().first_epoch, info.first_epoch);
        EXPECT_EQ(extended->data_info().last_epoch, info.last_epoch);
        for (const auto direction : {-1, 1}) {
            const auto anchor = direction < 0 ? info.first_epoch : info.last_epoch;
            const auto limit = core::Epoch{anchor.tai_nanoseconds() + direction * 2 * day};
            EXPECT_NO_THROW((void)extended->at(limit));
            EXPECT_THROW((void)extended->at(core::Epoch{limit.tai_nanoseconds() + direction}),
                         core::EopCoverageError);
            const auto inside = extended->at(anchor);
            EXPECT_EQ(inside.sample.ut1_minus_tai_seconds,
                      strict->at(anchor).sample.ut1_minus_tai_seconds);
            const auto adjacent = extended->at(core::Epoch{anchor.tai_nanoseconds() + direction});
            EXPECT_NEAR(adjacent.sample.ut1_minus_tai_seconds, inside.sample.ut1_minus_tai_seconds,
                        1e-12);
            const auto earlier =
                extended->at(core::Epoch{anchor.tai_nanoseconds() - 1'000'000'000});
            const auto later = extended->at(core::Epoch{anchor.tai_nanoseconds() + 1'000'000'000});
            EXPECT_NEAR(
                (later.sample.ut1_minus_tai_seconds - earlier.sample.ut1_minus_tai_seconds) / 2,
                inside.rates.ut1_minus_tai_seconds_per_second, 1e-11);
            EXPECT_NEAR((later.sample.polar_motion_x_rad - earlier.sample.polar_motion_x_rad) / 2,
                        inside.rates.polar_motion_x_rad_per_second, 1e-15);
            EXPECT_NEAR((later.sample.polar_motion_y_rad - earlier.sample.polar_motion_y_rad) / 2,
                        inside.rates.polar_motion_y_rad_per_second, 1e-15);
            EXPECT_NEAR(
                (later.sample.celestial_pole_dx_rad - earlier.sample.celestial_pole_dx_rad) / 2,
                inside.rates.celestial_pole_dx_rad_per_second, 1e-15);
            EXPECT_NEAR(
                (later.sample.celestial_pole_dy_rad - earlier.sample.celestial_pole_dy_rad) / 2,
                inside.rates.celestial_pole_dy_rad_per_second, 1e-15);
            EXPECT_NE(
                extended->at(limit).sample.source_id.find("extrapolation=endpoint-linear-tai-v1"),
                std::string::npos);
            EXPECT_THROW((void)strict->at(limit), core::EopCoverageError);
        }
    }
}
TEST_F(DataCenterTest, ExtrapolationNeverExtendsLeapSecondValidity) {
    class EndOfTimeData final : public core::IEarthOrientationProvider {
      public:
        core::EopDataInfo info;
        explicit EndOfTimeData(const core::ITimeProvider &clock)
            : info{"test",
                   std::string(64, 'a'),
                   "test",
                   "ITRS",
                   "test",
                   clock.from_utc({2027, 6, 26, 0, 0, 0, 0}),
                   clock.from_utc({2027, 6, 27, 0, 0, 0, 0}),
                   clock.data_info()} {}
        const core::EopDataInfo &data_info() const noexcept override { return info; }
        core::EarthOrientationState at(core::Epoch t) const override {
            return {{t, -37, 0, 0, 0, 0, "test"}, {0, 0, 0, 0, 0}};
        }
    };
    const auto time = runtime::make_builtin_time_provider();
    auto source = std::make_shared<EndOfTimeData>(*time);
    const auto extended = runtime::with_eop_extrapolation(source, 30);
    const auto expiry = core::Epoch{source->info.last_epoch.tai_nanoseconds() + 86'400'000'000'000};
    EXPECT_NO_THROW((void)extended->at(core::Epoch{expiry.tai_nanoseconds() - 1}));
    EXPECT_THROW((void)extended->at(expiry), core::TimeDataRangeError);
}
} // namespace
} // namespace orbitforge::desktop
