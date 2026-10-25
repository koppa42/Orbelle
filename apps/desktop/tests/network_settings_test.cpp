#include "../src/viewer/network_settings.hpp"
#include "../src/viewer/preferences_dialog.hpp"
#include "../src/viewer/workbench_style.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <gtest/gtest.h>

namespace orbelle::desktop {
namespace {
QApplication &test_app() {
    static int argc = 1;
    static char name[] = "network_settings_test";
    static char *argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    apply_workbench_style(app);
    return app;
}
class NetworkSettingsTest : public testing::Test {
  protected:
    void SetUp() override {
        (void)test_app();
        ASSERT_TRUE(directory.isValid());
        system = QNetworkProxyFactory::usesSystemConfiguration();
        proxy = QNetworkProxy::applicationProxy();
        preferences = directory.filePath("desktop-preferences.ini");
    }
    void TearDown() override {
        QNetworkProxy::setApplicationProxy(proxy);
        if (system)
            QNetworkProxyFactory::setUseSystemConfiguration(true);
    }
    QTemporaryDir directory;
    QString preferences;
    bool system = false;
    QNetworkProxy proxy;
};

TEST_F(NetworkSettingsTest, DefaultsToSystemAndPersistsModesWithoutChangingOtherPreferences) {
    QSettings other(preferences, QSettings::IniFormat);
    other.setValue("updates/startup", false);
    other.sync();
    NetworkSettings settings(preferences);
    EXPECT_EQ(settings.value().mode, ProxyMode::system);
    EXPECT_TRUE(QNetworkProxyFactory::usesSystemConfiguration());
    QString error;
    ASSERT_TRUE(settings.save({ProxyMode::manual, " 127.0.0.1:7890 "}, error));
    EXPECT_EQ(settings.value().address, "http://127.0.0.1:7890");
    EXPECT_EQ(QNetworkProxy::applicationProxy().type(), QNetworkProxy::HttpProxy);
    EXPECT_FALSE(QNetworkProxyFactory::usesSystemConfiguration());
    NetworkSettings reloaded(preferences);
    EXPECT_EQ(reloaded.value(), settings.value());
    ASSERT_TRUE(settings.save({ProxyMode::direct, {}}, error));
    EXPECT_EQ(QNetworkProxy::applicationProxy().type(), QNetworkProxy::NoProxy);
    NetworkSettings direct(preferences);
    EXPECT_EQ(direct.value().mode, ProxyMode::direct);
    EXPECT_EQ(direct.value().address, "http://127.0.0.1:7890");
    ASSERT_TRUE(settings.save({ProxyMode::system, {}}, error));
    EXPECT_TRUE(QNetworkProxyFactory::usesSystemConfiguration());
    NetworkSettings restored(preferences);
    EXPECT_EQ(restored.value().mode, ProxyMode::system);
    other.sync();
    EXPECT_FALSE(other.value("updates/startup", true).toBool());
}

TEST_F(NetworkSettingsTest, AcceptsHttpSocksAndIpv6AndRejectsAmbiguousEndpoints) {
    for (const auto &address :
         {"http://proxy.example:8080", "proxy.example:80", "HTTP://127.0.0.1:7890/",
          "socks5://127.0.0.1:1080", "http://[::1]:7890", "[2001:db8::1]:65535"}) {
        SCOPED_TRACE(address);
        EXPECT_TRUE(NetworkSettings::parse_address(address));
    }
    const auto socks = NetworkSettings::parse_address("socks5://[::1]:1080");
    ASSERT_TRUE(socks);
    EXPECT_EQ(socks->type(), QNetworkProxy::Socks5Proxy);
    EXPECT_EQ(socks->hostName(), "::1");
    EXPECT_EQ(socks->port(), 1080);
    for (const auto &address :
         {"", "localhost", "http://:1234", "http://localhost:0", "http://localhost:-1",
          "http://localhost:65536", "http://localhost:port", "https://localhost:443",
          "socks4://localhost:1080", "http://user:secret@localhost:80", "http://@localhost:80",
          "http://localhost:80/path", "http://localhost:80?query", "http://localhost:80#fragment",
          "http://local host:80", "http://localhost:80\nheader"}) {
        SCOPED_TRACE(address);
        EXPECT_FALSE(NetworkSettings::parse_address(address));
    }
}

TEST_F(NetworkSettingsTest, InvalidInputAndWriteFailurePreserveTheLivePolicy) {
    NetworkSettings settings(preferences);
    QString error;
    ASSERT_TRUE(settings.save({ProxyMode::manual, "socks5://localhost:1080"}, error));
    QFile file(preferences);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto saved = file.readAll();
    file.close();
    ASSERT_FALSE(settings.save({ProxyMode::manual, "https://localhost:443"}, error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(QNetworkProxy::applicationProxy().type(), QNetworkProxy::Socks5Proxy);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), saved);
    file.close();
    NetworkSettings unwritable(directory.path()); // A directory cannot be replaced by an INI.
    ASSERT_FALSE(unwritable.save({ProxyMode::direct, {}}, error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(unwritable.value().mode, ProxyMode::system);
    EXPECT_TRUE(QNetworkProxyFactory::usesSystemConfiguration());
}

TEST_F(NetworkSettingsTest, InvalidSavedManualSettingsReportFallbackToSystem) {
    QSettings file(preferences, QSettings::IniFormat);
    file.setValue("network/proxy-mode", "manual");
    file.setValue("network/proxy-address", "https://localhost:443");
    file.sync();
    NetworkSettings settings(preferences);
    EXPECT_EQ(settings.value().mode, ProxyMode::system);
    EXPECT_TRUE(QNetworkProxyFactory::usesSystemConfiguration());
    EXPECT_FALSE(settings.load_error().isEmpty());
    QString error;
    ASSERT_TRUE(settings.save({ProxyMode::system, {}}, error));
    EXPECT_TRUE(settings.load_error().isEmpty());
}

class HttpProxy final : public QTcpServer {
  public:
    explicit HttpProxy(bool socks = false) : socks_(socks) {
        setProxy(QNetworkProxy::NoProxy);
        if (!listen(QHostAddress::LocalHost))
            throw std::runtime_error("Cannot open proxy fixture");
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                auto bytes = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", bytes);
                if (socks_) {
                    int stage = socket->property("socksStage").toInt();
                    if (stage == 0) {
                        if (bytes.size() < 2 || bytes.size() < 2 + static_cast<quint8>(bytes[1]))
                            return;
                        bytes.remove(0, 2 + static_cast<quint8>(bytes[1]));
                        socket->write(QByteArray::fromHex("0500"));
                        socket->setProperty("socksStage", ++stage);
                        socket->setProperty("request", bytes);
                    }
                    if (stage == 1) {
                        // The fixture expects remote DNS: domain name, not a local resolution.
                        if (bytes.size() < 5)
                            return;
                        EXPECT_EQ(bytes.left(4), QByteArray::fromHex("05010003"));
                        const int size = 7 + static_cast<quint8>(bytes[4]);
                        if (bytes.size() < size)
                            return;
                        targets.append(bytes.mid(5, static_cast<quint8>(bytes[4])));
                        bytes.remove(0, size);
                        socket->write(QByteArray::fromHex("05000001000000000000"));
                        socket->setProperty("socksStage", 2);
                        socket->setProperty("request", bytes);
                    }
                }
                if (!bytes.contains("\r\n\r\n") || socket->property("answered").toBool())
                    return;
                socket->setProperty("answered", true);
                requests.append(bytes);
                pending = socket;
                if (!hold)
                    finish();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
    void finish() {
        if (!pending)
            return;
        pending->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
        pending->disconnectFromHost();
        pending.clear();
    }
    QString address() const {
        return QStringLiteral("%1://127.0.0.1:%2")
            .arg(socks_ ? "socks5" : "http")
            .arg(serverPort());
    }
    QList<QByteArray> requests;
    QList<QByteArray> targets;
    bool hold = false;
    QPointer<QTcpSocket> pending;

  private:
    bool socks_;
};

TEST_F(NetworkSettingsTest, ExistingManagersUseNewPolicyWhileActiveReplyCompletes) {
    QNetworkAccessManager data, updates;
    NetworkSettings settings(preferences);
    HttpProxy first, second;
    first.hold = true;
    QString error;
    ASSERT_TRUE(settings.save({ProxyMode::manual, first.address()}, error));
    auto *active = data.get(QNetworkRequest(QUrl("http://download.invalid/first")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return first.requests.size() == 1; }, 3000))
        << active->errorString().toStdString() << " manager proxy " << data.proxy().type();
    EXPECT_FALSE(active->isFinished());
    ASSERT_TRUE(settings.save({ProxyMode::manual, second.address()}, error));
    auto *next = updates.get(QNetworkRequest(QUrl("http://download.invalid/update")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return next->isFinished(); }, 3000));
    EXPECT_EQ(next->error(), QNetworkReply::NoError);
    ASSERT_EQ(second.requests.size(), 1);
    EXPECT_TRUE(second.requests.front().startsWith("GET http://download.invalid/update "));
    first.finish();
    ASSERT_TRUE(QTest::qWaitFor([&] { return active->isFinished(); }, 3000));
    EXPECT_EQ(active->error(), QNetworkReply::NoError);
    EXPECT_EQ(active->readAll(), "ok");
    auto *same_manager = data.get(QNetworkRequest(QUrl("http://download.invalid/next")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return same_manager->isFinished(); }, 3000));
    EXPECT_EQ(same_manager->error(), QNetworkReply::NoError);
    EXPECT_EQ(first.requests.size(), 1);
    EXPECT_EQ(second.requests.size(), 2);
    ASSERT_TRUE(settings.save({ProxyMode::direct, {}}, error));
    auto *direct = updates.get(QNetworkRequest(QUrl(second.address() + "/direct")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return direct->isFinished(); }, 3000));
    EXPECT_EQ(direct->error(), QNetworkReply::NoError);
    ASSERT_EQ(second.requests.size(), 3);
    EXPECT_TRUE(second.requests.back().startsWith("GET /direct "));
}

TEST_F(NetworkSettingsTest, UnavailableManualProxyDoesNotFallBackToDirect) {
    NetworkSettings settings(preferences);
    HttpProxy unavailable;
    const auto address = unavailable.address();
    unavailable.close();
    QString error;
    ASSERT_TRUE(settings.save({ProxyMode::manual, address}, error));
    const auto proxies = QNetworkProxyFactory::proxyForQuery(
        QNetworkProxyQuery(QUrl("https://download.invalid/package")));
    ASSERT_EQ(proxies.size(), 1);
    EXPECT_EQ(proxies.front().type(), QNetworkProxy::HttpProxy);
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl("http://download.invalid/package"));
    request.setTransferTimeout(1000);
    auto *reply = manager.get(request);
    ASSERT_TRUE(QTest::qWaitFor([&] { return reply->isFinished(); }, 3000));
    EXPECT_NE(reply->error(), QNetworkReply::NoError);
}

TEST_F(NetworkSettingsTest, HttpProxyReceivesHttpsConnectAndSocksResolvesRemoteNames) {
    NetworkSettings settings(preferences);
    HttpProxy http, socks(true);
    http.hold = true;
    QString error;
    ASSERT_TRUE(settings.save({ProxyMode::manual, http.address()}, error));
    QNetworkAccessManager manager;
    auto *secure = manager.get(QNetworkRequest(QUrl("https://updates.invalid/package")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !http.requests.isEmpty(); }, 3000));
    EXPECT_TRUE(http.requests.front().startsWith("CONNECT updates.invalid:443 "));
    secure->abort();
    ASSERT_TRUE(settings.save({ProxyMode::manual, socks.address()}, error));
    auto *reply = manager.get(QNetworkRequest(QUrl("http://download.invalid/data")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return reply->isFinished(); }, 3000));
    EXPECT_EQ(reply->error(), QNetworkReply::NoError) << reply->errorString().toStdString();
    ASSERT_EQ(socks.targets.size(), 1);
    EXPECT_EQ(socks.targets.front(), "download.invalid");
    ASSERT_EQ(socks.requests.size(), 1);
    EXPECT_TRUE(socks.requests.front().startsWith("GET /data "));
}

TEST_F(NetworkSettingsTest, DialogCancelValidationApplyAndReopenPreserveDraftBoundaries) {
    NetworkSettings settings(preferences);
    {
        PreferencesDialog dialog(settings);
        dialog.show();
        auto *mode = dialog.findChild<QComboBox *>("networkProxyMode");
        auto *address = dialog.findChild<QLineEdit *>("networkProxyAddress");
        ASSERT_NE(mode, nullptr);
        ASSERT_NE(address, nullptr);
        EXPECT_EQ(mode->currentText(), QString::fromUtf8("使用系统代理（默认）"));
        EXPECT_FALSE(address->isEnabled());
        mode->setCurrentIndex(2);
        EXPECT_TRUE(address->isEnabled());
        address->setText("http://localhost:7890");
        dialog.findChild<QPushButton *>("cancelPreferences")->click();
        EXPECT_EQ(dialog.result(), QDialog::Rejected);
        EXPECT_EQ(settings.value().mode, ProxyMode::system);
        EXPECT_FALSE(QFile::exists(preferences));
    }
    {
        PreferencesDialog dialog(settings);
        dialog.show();
        auto *mode = dialog.findChild<QComboBox *>("networkProxyMode");
        auto *address = dialog.findChild<QLineEdit *>("networkProxyAddress");
        mode->setCurrentIndex(2);
        address->setText("https://localhost:7890");
        dialog.findChild<QPushButton *>("savePreferences")->click();
        EXPECT_TRUE(dialog.isVisible());
        EXPECT_TRUE(dialog.findChild<QLabel *>("inputError")->isVisible());
        EXPECT_EQ(settings.value().mode, ProxyMode::system);
        address->setText("localhost:7890");
        dialog.findChild<QPushButton *>("applyPreferences")->click();
        EXPECT_TRUE(dialog.isVisible());
        EXPECT_FALSE(dialog.findChild<QLabel *>("inputError")->isVisible());
        EXPECT_EQ(settings.value().address, "http://localhost:7890");
        mode->setCurrentIndex(1);
        dialog.reject();
    }
    PreferencesDialog reopened(settings);
    EXPECT_EQ(reopened.findChild<QComboBox *>("networkProxyMode")->currentIndex(), 2);
    EXPECT_EQ(reopened.findChild<QLineEdit *>("networkProxyAddress")->text(),
              "http://localhost:7890");
}

TEST_F(NetworkSettingsTest, NarrowLayoutKeepsFieldsAndActionsReachable) {
    NetworkSettings settings(preferences);
    PreferencesDialog dialog(settings);
    dialog.show();
    auto *mode = dialog.findChild<QComboBox *>("networkProxyMode");
    auto *address = dialog.findChild<QLineEdit *>("networkProxyAddress");
    mode->setCurrentIndex(2);
    address->setText("socks5://127.0.0.1:1080");
    const auto output = qEnvironmentVariable("ORBELLE_NETWORK_UI_CAPTURE");
    if (!output.isEmpty())
        ASSERT_TRUE(QDir().mkpath(output));
    for (const QSize size : {QSize(940, 640), QSize(360, 420), QSize(360, 320), QSize(940, 640)}) {
        dialog.resize(size);
        QTest::qWait(30);
        EXPECT_EQ(address->text(), "socks5://127.0.0.1:1080");
        auto *scroll = dialog.findChild<QScrollArea *>("networkSettingsScroll");
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        scroll->ensureWidgetVisible(address);
        EXPECT_TRUE(address->isVisible());
        for (const auto *name : {"savePreferences", "cancelPreferences", "applyPreferences"}) {
            auto *button = dialog.findChild<QPushButton *>(name);
            EXPECT_TRUE(button->isVisible());
            EXPECT_TRUE(
                dialog.rect().contains(QRect(button->mapTo(&dialog, QPoint()), button->size())));
        }
        if (!output.isEmpty())
            EXPECT_TRUE(dialog.grab().save(
                output +
                QStringLiteral("/network-%1x%2.png").arg(size.width()).arg(size.height())));
    }
    address->setFocus();
    QTest::keyClick(address, Qt::Key_Escape);
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(settings.value().mode, ProxyMode::system);
}
void select_data(PreferencesDialog &dialog, DataProduct product) {
    auto *navigation = dialog.findChild<QTreeWidget *>("propertyNavigation");
    for (QTreeWidgetItemIterator item(navigation); *item; ++item)
        if ((*item)->data(0, Qt::UserRole).isValid() &&
            (*item)->data(0, Qt::UserRole).toInt() == static_cast<int>(product)) {
            navigation->setCurrentItem(*item);
            return;
        }
    FAIL() << "Missing data navigation item";
}
TEST_F(NetworkSettingsTest, PreferencesKeepAllPagesAsDraftsAndApplyTogether) {
    EopCatalog catalog(std::filesystem::path(directory.filePath("data").toStdWString()), {}, {},
                       false);
    auto original = catalog.preferences();
    original.updates.at(DataProduct::c04).hours = 37;
    catalog.set_preferences(original);
    NetworkSettings settings(preferences);
    PreferencesDialog dialog(settings, &catalog);
    dialog.show();
    EXPECT_EQ(dialog.windowTitle(), QString::fromUtf8("首选项"));
    auto *navigation = dialog.findChild<QTreeWidget *>("propertyNavigation");
    auto *pages = dialog.findChild<QStackedWidget *>("preferencesPages");
    auto *apply = dialog.findChild<QPushButton *>("applyPreferences");
    ASSERT_EQ(navigation->topLevelItemCount(), 4);
    EXPECT_FALSE(apply->isEnabled());
    auto *startup = dialog.findChild<QCheckBox *>("preferencesStartupUpdates");
    auto *method = dialog.findChild<QComboBox *>("preferencesInsertMethod0");
    select_data(dialog, DataProduct::c04);
    auto *hours = dialog.findChild<QComboBox *>("dataUpdateFrequency");
    EXPECT_EQ(hours->currentData().toInt(), 37);
    startup->setChecked(false);
    method->setCurrentIndex(method->findData("orbit-wizard"));
    hours->setCurrentIndex(hours->findData(24));
    select_data(dialog, DataProduct::finals2000a);
    dialog.findChild<QCheckBox *>("allowEopExtrapolation")->setChecked(true);
    dialog.findChild<QSpinBox *>("eopExtrapolationDays")->setValue(4);
    auto *mode = dialog.findChild<QComboBox *>("networkProxyMode");
    auto *address = dialog.findChild<QLineEdit *>("networkProxyAddress");
    mode->setCurrentIndex(2);
    address->setText("https://localhost:7890");
    navigation->setCurrentItem(navigation->topLevelItem(3));
    EXPECT_EQ(pages->currentIndex(), 3);
    EXPECT_EQ(catalog.preferences(), original);
    EXPECT_FALSE(QFile::exists(preferences));
    apply->click();
    EXPECT_EQ(pages->currentIndex(), 0);
    EXPECT_EQ(catalog.preferences(), original);
    EXPECT_FALSE(QFile::exists(preferences));
    address->setText("localhost:7890");
    apply->click();
    EXPECT_FALSE(apply->isEnabled());
    EXPECT_EQ(catalog.preferences().updates.at(DataProduct::c04).hours, 24);
    EXPECT_EQ(catalog.preferences().finals_extrapolation_days, 4);
    EXPECT_EQ(settings.value().mode, ProxyMode::manual);
    QSettings saved(preferences, QSettings::IniFormat);
    EXPECT_FALSE(saved.value("updates/startup", true).toBool());
    EXPECT_EQ(saved.value("insertion/default-method/satellite").toString(), "orbit-wizard");
    startup->setChecked(true);
    select_data(dialog, DataProduct::c04);
    hours->setCurrentIndex(hours->findData(6));
    dialog.reject();
    PreferencesDialog reopened(settings, &catalog);
    EXPECT_FALSE(reopened.findChild<QCheckBox *>("preferencesStartupUpdates")->isChecked());
    select_data(reopened, DataProduct::c04);
    EXPECT_EQ(reopened.findChild<QComboBox *>("dataUpdateFrequency")->currentData().toInt(), 24);
    EXPECT_EQ(reopened.findChild<QComboBox *>("preferencesInsertMethod0")->currentData().toString(),
              "orbit-wizard");
}

TEST_F(NetworkSettingsTest, PreferenceFailureRollsBackDataAndIniDrafts) {
    EopCatalog catalog(std::filesystem::path(directory.filePath("data").toStdWString()), {}, {},
                       false);
    const auto original = catalog.preferences();
    auto invalid = original;
    invalid.preferred = DataProduct::finals2000a;
    invalid.updates.at(DataProduct::utc).hours = 0;
    EXPECT_THROW(catalog.set_preferences(invalid), std::invalid_argument);
    EXPECT_EQ(catalog.preferences(), original);
    NetworkSettings unwritable(directory.path());
    PreferencesDialog dialog(unwritable, &catalog);
    dialog.show();
    select_data(dialog, DataProduct::c04);
    auto *frequency = dialog.findChild<QComboBox *>("dataUpdateFrequency");
    frequency->setCurrentIndex(frequency->findData(24));
    dialog.findChild<QCheckBox *>("preferencesStartupUpdates")->setChecked(false);
    dialog.findChild<QComboBox *>("networkProxyMode")->setCurrentIndex(1);
    dialog.findChild<QPushButton *>("savePreferences")->click();
    EXPECT_TRUE(dialog.isVisible());
    EXPECT_TRUE(dialog.findChild<QWidget *>("preferencesFooter")
                    ->findChild<QLabel *>("inputError")
                    ->isVisible());
    EXPECT_EQ(catalog.preferences(), original);
    EXPECT_EQ(unwritable.value().mode, ProxyMode::system);
    QSettings saved(directory.path(), QSettings::IniFormat);
    EXPECT_FALSE(saved.contains("updates/startup"));
    EXPECT_FALSE(saved.contains("insertion/keep-open"));
    dialog.reject();
}

TEST_F(NetworkSettingsTest, EveryPreferencePageKeepsDraftAndNavigationAcrossWidths) {
    EopCatalog catalog(std::filesystem::path(directory.filePath("data").toStdWString()), {}, {},
                       false);
    NetworkSettings settings(preferences);
    PreferencesDialog dialog(settings, &catalog);
    dialog.show();
    auto *navigation = dialog.findChild<QTreeWidget *>("propertyNavigation");
    auto *selector = dialog.findChild<QComboBox *>("preferencesPageSelector");
    auto *pages = dialog.findChild<QStackedWidget *>("preferencesPages");
    auto *method = dialog.findChild<QComboBox *>("preferencesInsertMethod0");
    method->setCurrentIndex(method->findData("tle-file"));
    const auto output = qEnvironmentVariable("ORBELLE_NETWORK_UI_CAPTURE");
    if (!output.isEmpty())
        ASSERT_TRUE(QDir().mkpath(output));
    for (QSize size : {QSize(940, 640), QSize(420, 540), QSize(360, 400), QSize(940, 640)}) {
        dialog.resize(size);
        QTest::qWait(40);
        for (int i = 0; i < 4; ++i) {
            if (navigation->isVisible())
                navigation->setCurrentItem(navigation->topLevelItem(i));
            else
                selector->setCurrentIndex(
                    navigation->topLevelItem(i)->data(0, Qt::UserRole + 21).toInt());
            QTest::qWait(30);
            EXPECT_EQ(pages->currentIndex(), i);
            EXPECT_EQ(navigation->currentItem()->data(0, Qt::UserRole + 20).toInt(), i);
            EXPECT_EQ(selector->currentIndex(),
                      navigation->currentItem()->data(0, Qt::UserRole + 21).toInt());
            for (auto *scroll : pages->currentWidget()->findChildren<QScrollArea *>())
                if (scroll->isVisible())
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            if (auto *scroll = qobject_cast<QScrollArea *>(pages->currentWidget()))
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_EQ(method->currentData().toString(), "tle-file");
            auto *ok = dialog.findChild<QPushButton *>("savePreferences");
            EXPECT_TRUE(dialog.rect().contains(QRect(ok->mapTo(&dialog, QPoint{}), ok->size())));
            if (!output.isEmpty())
                EXPECT_TRUE(dialog.grab().save(
                    output + QStringLiteral("/preferences-%1-%2.png").arg(i).arg(size.width())));
        }
    }
    dialog.reject();
    EXPECT_FALSE(QFile::exists(preferences));
}
TEST_F(NetworkSettingsTest, IntegratedDataKeepsDownloadsAndDiscardsOnlyUnappliedPreferences) {
    NetworkSettings settings(preferences);
    QString error;
    ASSERT_TRUE(settings.save({ProxyMode::direct, {}}, error));
    EopCatalog catalog(std::filesystem::path(directory.filePath("data").toStdWString()), {}, {},
                       false);
    const auto original = catalog.preferences();
    HttpProxy server;
    server.hold = true;
    catalog.start_download(QUrl(server.address()), DataProduct::finals2000a);
    QTRY_VERIFY_WITH_TIMEOUT(!server.requests.isEmpty(), 2000);
    {
        PreferencesDialog dialog(settings, &catalog, nullptr, PreferencesPage::data,
                                 DataProduct::finals2000a);
        dialog.show();
        QTest::qWait(180);
        auto *tree = dialog.findChild<QTreeWidget *>("propertyNavigation");
        EXPECT_EQ(tree->currentItem()->data(0, Qt::UserRole).toInt(),
                  static_cast<int>(DataProduct::finals2000a));
        EXPECT_EQ(dialog.findChildren<QTreeWidget *>().size(), 1);
        auto *tabs = dialog.findChild<QTabWidget *>("dataPages");
        tabs->setCurrentIndex(1);
        dialog.findChild<QCheckBox *>("dataAutoUpdate")->setChecked(false);
        dialog.findChild<QCheckBox *>("allowEopExtrapolation")->setChecked(true);
        dialog.findChild<QSpinBox *>("eopExtrapolationDays")->setValue(5);
        tree->setCurrentItem(tree->topLevelItem(0));
        QTest::qWait(180);
        select_data(dialog, DataProduct::finals2000a);
        tabs->setCurrentIndex(1);
        EXPECT_FALSE(dialog.findChild<QCheckBox *>("dataAutoUpdate")->isChecked());
        EXPECT_EQ(dialog.findChild<QSpinBox *>("eopExtrapolationDays")->value(), 5);
        EXPECT_EQ(catalog.preferences(), original);
        EXPECT_TRUE(catalog.downloading());
        dialog.reject();
    }
    QTest::qWait(180);
    EXPECT_TRUE(catalog.downloading());
    EXPECT_EQ(catalog.preferences(), original);
    PreferencesDialog reopened(settings, &catalog, nullptr, PreferencesPage::data,
                               DataProduct::de440s);
    reopened.show();
    EXPECT_EQ(reopened.findChild<QTreeWidget *>("propertyNavigation")
                  ->currentItem()
                  ->data(0, Qt::UserRole)
                  .toInt(),
              static_cast<int>(DataProduct::de440s));
    select_data(reopened, DataProduct::finals2000a);
    auto *cancel = reopened.findChild<QPushButton *>("cancelEopDownload");
    QTRY_VERIFY_WITH_TIMEOUT(cancel->isVisible(), 1000);
    cancel->click();
    QTRY_VERIFY_WITH_TIMEOUT(!catalog.downloading(), 1000);
    EXPECT_EQ(catalog.update_status().state, DataUpdateState::cancelled);
    EXPECT_TRUE(reopened.findChild<QCheckBox *>("dataAutoUpdate")->isChecked());
    EXPECT_FALSE(reopened.findChild<QCheckBox *>("allowEopExtrapolation")->isChecked());
    reopened.reject();
}
TEST_F(NetworkSettingsTest, IdleDataNavigationDoesNotRepublishUnchangedRows) {
    NetworkSettings settings(preferences);
    EopCatalog catalog(std::filesystem::path(directory.filePath("data").toStdWString()), {}, {},
                       false);
    PreferencesDialog dialog(settings, &catalog, nullptr, PreferencesPage::data);
    dialog.show();
    auto *tree = dialog.findChild<QTreeWidget *>("propertyNavigation");
    ASSERT_NE(tree, nullptr);
    QTest::qWait(200);
    QSignalSpy changes(tree->model(), &QAbstractItemModel::dataChanged);
    QTest::qWait(500); // Several refresh ticks must not generate layout/accessibility work.
    EXPECT_EQ(changes.count(), 0);
    select_data(dialog, DataProduct::finals2000a);
    QTest::qWait(200);
    EXPECT_EQ(tree->currentItem()->text(0), "finals2000A");
    EXPECT_TRUE(tree->currentItem()->toolTip(0).contains('\n'));
    changes.clear();
    QTest::qWait(500);
    EXPECT_EQ(changes.count(), 0);
    dialog.reject();
}
TEST_F(NetworkSettingsTest, NestedDataEntryUsesTheSharedPreferencesRoute) {
    EopCatalog catalog(std::filesystem::path(directory.filePath("data").toStdWString()), {}, {},
                       false);
    QWidget parent;
    int opened = 0;
    catalog.open_data_center = [&](QWidget *owner, std::optional<DataProduct> product) {
        ++opened;
        EXPECT_EQ(owner, &parent);
        EXPECT_EQ(product, DataProduct::space_weather);
    };
    show_data_center(&parent, catalog, DataProduct::space_weather);
    EXPECT_EQ(opened, 1);
}
TEST_F(NetworkSettingsTest, DataGroupsKeepCompactHeadingsAndScrollableBody) {
    NetworkSettings settings(preferences);
    EopCatalog catalog(std::filesystem::path(directory.filePath("data").toStdWString()), {}, {},
                       false);
    PreferencesDialog dialog(settings, &catalog, nullptr, PreferencesPage::data);
    dialog.show();
    auto *tree = dialog.findChild<QTreeWidget *>("propertyNavigation");
    auto *panel = dialog.findChild<QWidget *>("dataDetailPanel");
    auto *heading = dialog.findChild<QLabel *>("dataName");
    auto *description = dialog.findChild<QLabel *>("dataDescription");
    auto *scroll = dialog.findChild<QScrollArea *>("dataGroupScroll");
    ASSERT_TRUE(tree && panel && heading && description && scroll);
    const auto output = qEnvironmentVariable("ORBELLE_NETWORK_UI_CAPTURE");
    if (!output.isEmpty())
        ASSERT_TRUE(QDir().mkpath(output));
    auto *root = tree->topLevelItem(static_cast<int>(PreferencesPage::data));
    for (const QSize size :
         {QSize(1040, 720), QSize(1040, 900), QSize(360, 480), QSize(360, 320)}) {
        dialog.resize(size);
        QTest::qWait(30);
        for (int row = 0; row < root->childCount(); ++row) {
            auto *item = root->child(row);
            if (item->data(0, Qt::UserRole).isValid())
                continue;
            tree->setCurrentItem(item);
            QTest::qWait(30);
            SCOPED_TRACE(item->text(0).toStdString());
            EXPECT_LE(heading->mapTo(panel, QPoint{}).y(), 24);
            const auto description_top = description->mapTo(panel, QPoint{}).y();
            const auto heading_bottom = heading->mapTo(panel, QPoint(0, heading->height())).y();
            EXPECT_GE(description_top, heading_bottom);
            EXPECT_LE(description_top - heading_bottom, 16);
            if (scroll->isVisible()) {
                auto *summary = scroll->findChild<QLabel *>("hint");
                ASSERT_NE(summary, nullptr);
                EXPECT_LE(summary->mapTo(scroll->widget(), QPoint{}).y(), 16);
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            }
            auto *ok = dialog.findChild<QPushButton *>("savePreferences");
            EXPECT_TRUE(dialog.rect().contains(QRect(ok->mapTo(&dialog, QPoint{}), ok->size())));
            if (!output.isEmpty())
                EXPECT_TRUE(dialog.grab().save(output + QStringLiteral("/group-%1-%2x%3.png")
                                                            .arg(row)
                                                            .arg(size.width())
                                                            .arg(size.height())));
        }
    }
    dialog.reject();
}
TEST_F(NetworkSettingsTest, IntegratedDataUsesOneNavigationAndFitsEveryProductAtNarrowWidths) {
    NetworkSettings settings(preferences);
    EopCatalog catalog(std::filesystem::path(directory.filePath("data").toStdWString()), {}, {},
                       false);
    PreferencesDialog dialog(settings, &catalog, nullptr, PreferencesPage::data);
    dialog.show();
    const auto output = qEnvironmentVariable("ORBELLE_NETWORK_UI_CAPTURE");
    if (!output.isEmpty())
        ASSERT_TRUE(QDir().mkpath(output));
    for (int width : {1040, 360, 420, 1040}) {
        dialog.resize(width, width == 1040 ? 720 : 480);
        QTest::qWait(35);
        for (auto product : all_data_products) {
            SCOPED_TRACE(::testing::Message()
                         << "width=" << width << " product=" << static_cast<int>(product));
            select_data(dialog, product);
            auto *tabs = dialog.findChild<QTabWidget *>("dataPages");
            EXPECT_EQ(tabs->isTabVisible(1),
                      product == DataProduct::finals2000a || product == DataProduct::c04 ||
                          product == DataProduct::utc || product == DataProduct::space_weather);
            for (int tab = 0; tab < tabs->count(); ++tab) {
                if (!tabs->isTabVisible(tab))
                    continue;
                SCOPED_TRACE(::testing::Message() << "tab=" << tab);
                tabs->setCurrentIndex(tab);
                QTest::qWait(5);
                auto *scroll = qobject_cast<QScrollArea *>(tabs->currentWidget());
                ASSERT_NE(scroll, nullptr);
                EXPECT_TRUE(QTest::qWaitFor(
                    [scroll] { return scroll->horizontalScrollBar()->maximum() == 0; }, 1000));
                auto *ok = dialog.findChild<QPushButton *>("savePreferences");
                EXPECT_TRUE(
                    dialog.rect().contains(QRect(ok->mapTo(&dialog, QPoint{}), ok->size())));
                if (!output.isEmpty())
                    EXPECT_TRUE(dialog.grab().save(output + QStringLiteral("/data-%1-%2-%3.png")
                                                                .arg(static_cast<int>(product))
                                                                .arg(tab)
                                                                .arg(width)));
            }
        }
    }
    dialog.reject();
}
} // namespace
} // namespace orbelle::desktop
