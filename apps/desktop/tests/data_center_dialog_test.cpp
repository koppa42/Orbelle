#include "../src/viewer/eop_catalog.hpp"
#include "../src/viewer/workbench_style.hpp"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <gtest/gtest.h>

namespace orbitforge::desktop {
namespace {
class DownloadServer : public QTcpServer {
  public:
    QByteArray body;
    bool hold = true, fail = false;
    QPointer<QTcpSocket> pending;
    explicit DownloadServer(QByteArray bytes) : body(std::move(bytes)) {
        if (!listen(QHostAddress::LocalHost))
            throw std::runtime_error("Cannot open UI loopback server");
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                const auto request = socket->readAll();
                if (socket->property("answered").toBool())
                    return;
                socket->setProperty("answered", true);
                if (fail) {
                    socket->write("HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: "
                                  "close\r\n\r\n");
                    socket->disconnectFromHost();
                } else if (!hold && request.contains("If-None-Match: \"ui-file\"")) {
                    socket->write("HTTP/1.1 304 Not Modified\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                } else {
                    socket->write("HTTP/1.1 200 OK\r\nETag: \"ui-file\"\r\nContent-Length: " +
                                  QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body.left(256));
                    pending = socket;
                    if (!hold)
                        release();
                }
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
    void release() {
        if (pending) {
            pending->write(body.mid(256));
            pending->disconnectFromHost();
            pending.clear();
        }
    }
    QUrl url() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/finals").arg(serverPort()));
    }
};
TEST(DataCenterDialog, DownloadProgressResultsAndProductSwitchingStayConsistent) {
    static int argc = 1;
    static char name[] = "data_center_ui_test";
    static char *argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    apply_workbench_style(app);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    QFile file(QString::fromStdWString(
        (std::filesystem::path(ORBITFORGE_TEST_FINALS).parent_path() / "finals2000A.data")
            .wstring()));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    DownloadServer server(file.readAll());
    EopCatalog catalog(root / "data", {ORBITFORGE_TEST_FINALS}, {}, false);
    const auto output = qEnvironmentVariable("ORBITFORGE_DATA_UI_CAPTURE");
    if (!output.isEmpty())
        ASSERT_TRUE(QDir().mkpath(output));
    const auto capture = [&](QDialog *dialog, const QString &state) {
        if (!output.isEmpty()) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            EXPECT_TRUE(dialog->grab().save(output + "/" + state + ".png"));
        }
    };
    int stage = 0;
    std::optional<qint64> downloaded_at;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        if (elapsed.elapsed() > 10000) {
            ADD_FAILURE() << "Data center UI timeout at stage " << stage
                          << ", received=" << catalog.received_bytes()
                          << ", total=" << catalog.total_bytes();
            timer.stop();
            dialog->reject();
            return;
        }
        auto *list = dialog->findChild<QListWidget *>("dataInventory");
        auto *checked = dialog->findChild<QLabel *>("dataLastChecked");
        auto *badge = dialog->findChild<QLabel *>("dataStatus");
        auto *progress = dialog->findChild<QProgressBar *>("dataDownloadProgress");
        auto *cancel = dialog->findChild<QPushButton *>("cancelEopDownload");
        auto *download = dialog->findChild<QPushButton *>("downloadEop");
        auto *status = dialog->findChild<QLabel *>("dataCenterStatus");
        ASSERT_TRUE(list && checked && badge && progress && cancel && download && status);
        switch (stage) {
        case 0:
            EXPECT_TRUE(badge->text().contains(QStringLiteral("示例")));
            EXPECT_TRUE(checked->text().contains(QStringLiteral("尚未检查")));
            list->setCurrentRow(1);
            EXPECT_TRUE(badge->text().contains(QStringLiteral("未导入")));
            EXPECT_TRUE(dialog->findChild<QPushButton *>("importEopSnapshot")->isVisible());
            EXPECT_FALSE(download->isVisible());
            capture(dialog, "missing-c04");
            list->setCurrentRow(0);
            capture(dialog, "sample");
            catalog.start_download(server.url());
            ++stage;
            break;
        case 1:
            if (!progress->isVisible() || catalog.received_bytes() == 0 || progress->maximum() == 0)
                break;
            EXPECT_EQ(catalog.update_status().state, DataUpdateState::downloading);
            EXPECT_FALSE(download->isEnabled());
            EXPECT_TRUE(cancel->isVisible());
            EXPECT_GT(progress->value(), 0);
            EXPECT_LT(progress->value(), progress->maximum());
            capture(dialog, "downloading");
            list->setCurrentRow(2);
            EXPECT_FALSE(progress->isVisible());
            EXPECT_FALSE(cancel->isVisible());
            EXPECT_EQ(checked->text(), QStringLiteral("随程序更新"));
            list->setCurrentRow(0);
            EXPECT_TRUE(progress->isVisible());
            server.release();
            ++stage;
            break;
        case 2:
            if (catalog.downloading() || !badge->text().contains(QStringLiteral("完整包")))
                break;
            EXPECT_FALSE(progress->isVisible());
            EXPECT_FALSE(cancel->isVisible());
            EXPECT_TRUE(checked->text().contains(QStringLiteral("检查成功")));
            EXPECT_TRUE(download->isEnabled());
            downloaded_at = catalog.inventory()[0].acquired_at;
            EXPECT_TRUE(downloaded_at);
            capture(dialog, "downloaded");
            server.hold = false;
            catalog.start_download(server.url());
            ++stage;
            break;
        case 3:
            if (catalog.downloading() || !checked->text().contains(QStringLiteral("无新内容")))
                break;
            EXPECT_EQ(catalog.received_bytes(), 0);
            EXPECT_EQ(catalog.inventory()[0].acquired_at, downloaded_at);
            capture(dialog, "unchanged");
            server.fail = true;
            catalog.start_download(server.url());
            ++stage;
            break;
        case 4:
            if (catalog.downloading() || !download->text().contains(QStringLiteral("重试")))
                break;
            EXPECT_TRUE(checked->text().contains(QStringLiteral("失败")));
            EXPECT_TRUE(status->isVisible());
            EXPECT_EQ(catalog.inventory()[0].acquired_at, downloaded_at);
            capture(dialog, "failed");
            list->setCurrentRow(2);
            EXPECT_FALSE(status->isVisible());
            EXPECT_FALSE(download->isVisible());
            EXPECT_EQ(checked->text(), QStringLiteral("随程序更新"));
            capture(dialog, "utc");
            list->setCurrentRow(0);
            server.fail = false;
            server.hold = true;
            catalog.start_download(server.url());
            ++stage;
            break;
        case 5:
            if (!cancel->isVisible())
                break;
            cancel->click();
            ++stage;
            break;
        case 6:
            if (catalog.downloading() || !checked->text().contains(QStringLiteral("取消")))
                break;
            EXPECT_EQ(catalog.update_status().state, DataUpdateState::cancelled);
            EXPECT_EQ(catalog.inventory()[0].acquired_at, downloaded_at);
            EXPECT_FALSE(cancel->isVisible());
            EXPECT_TRUE(download->isEnabled());
            capture(dialog, "cancelled");
            ++stage;
            timer.stop();
            dialog->reject();
            break;
        }
    });
    timer.start(25);
    show_data_center(nullptr, catalog);
    EXPECT_EQ(stage, 7);
}
} // namespace
} // namespace orbitforge::desktop
