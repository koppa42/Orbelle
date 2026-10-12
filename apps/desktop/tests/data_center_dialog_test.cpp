#include "../src/viewer/eop_catalog.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "window_show_observer.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
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
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <gtest/gtest.h>

namespace orbitforge::desktop {
namespace {
QApplication &test_app() {
    static int argc = 1;
    static char name[] = "data_center_ui_test";
    static char *argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    apply_workbench_style(app);
    return app;
}
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
TEST(DataCenterDialog, LoadingWarningsStayInsideTheDialog) {
    (void)test_app();
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ASSERT_TRUE(QDir().mkpath(temporary.path() + "/legacy/" + QString(64, 'a')));
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    // An incomplete legacy snapshot should produce a visible, inline warning.
    EopCatalog catalog(root / "data", {ORBITFORGE_TEST_FINALS}, root / "legacy", false);
    ASSERT_FALSE(catalog.warnings().isEmpty());
    WindowShowObserver windows;
    bool inspected = false;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        bool found = false;
        for (auto *label : dialog->findChildren<QLabel *>("inputError"))
            if (label->text() == catalog.warnings().join('\n')) {
                EXPECT_TRUE(label->isVisible());
                EXPECT_FALSE(label->isWindow());
                found = true;
            }
        EXPECT_TRUE(found);
        inspected = true;
        dialog->reject();
    });
    show_data_center(nullptr, catalog);
    EXPECT_TRUE(inspected);
    EXPECT_EQ(windows.shown.join(", ").toStdString(), "QDialog:eopDataDialog");
}
TEST(DataCenterDialog, GroupedSourcesKeepDownloadStateAndPriorityIndependent) {
    (void)test_app();
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
            for (int pass = 0; pass < 3; ++pass)
                QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            EXPECT_LE(dialog->height(), dialog->screen()->availableGeometry().height());
            EXPECT_LE(dialog->width(), dialog->screen()->availableGeometry().width());
            for (auto *scroll : dialog->findChildren<QScrollArea *>())
                if (scroll->isVisible())
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0) << state.toStdString();
            EXPECT_TRUE(dialog->grab().save(output + "/" + state + ".png"));
        }
    };
    int stage = 0;
    std::optional<qint64> downloaded_at;
    QElapsedTimer elapsed;
    elapsed.start();
    // This case exercises the side-by-side layout even on Qt's 800px offscreen display.
    QTimer::singleShot(0, [] {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            dialog->resize(1020, dialog->height());
    });
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
        auto *list = dialog->findChild<QTreeWidget *>("dataInventory");
        auto *priority = dialog->findChild<QListWidget *>("eopSourcePriority");
        auto *pages = dialog->findChild<QTabWidget *>("dataPages");
        auto *automatic = dialog->findChild<QCheckBox *>("dataAutoUpdate");
        auto *checked = dialog->findChild<QLabel *>("dataLastChecked");
        auto *badge = dialog->findChild<QLabel *>("dataStatus");
        auto *progress = dialog->findChild<QProgressBar *>("dataDownloadProgress");
        auto *cancel = dialog->findChild<QPushButton *>("cancelEopDownload");
        auto *download = dialog->findChild<QPushButton *>("downloadEop");
        auto *status = dialog->findChild<QLabel *>("dataCenterStatus");
        ASSERT_TRUE(list && priority && checked && badge && progress && cancel && download &&
                    status);
        ASSERT_EQ(list->topLevelItemCount(), 2);
        auto *group = list->topLevelItem(0);
        ASSERT_EQ(group->childCount(), 2);
        auto *finals = group->child(0);
        auto *c04 = group->child(1);
        auto *utc = list->topLevelItem(1);
        switch (stage) {
        case 0:
            EXPECT_EQ(list->currentItem(), group);
            EXPECT_TRUE(group->isExpanded());
            EXPECT_TRUE(priority->isVisible());
            EXPECT_EQ(priority->count(), 2);
            EXPECT_EQ(priority->dragDropMode(), QAbstractItemView::InternalMove);
            EXPECT_FALSE(dialog->findChild<QComboBox *>("eopDataSource"));
            EXPECT_FALSE(download->isVisible());
            EXPECT_FALSE(checked->isVisible());
            EXPECT_EQ(priority->item(0)->data(Qt::UserRole).toInt(),
                      static_cast<int>(DataProduct::c04));
            EXPECT_TRUE(priority->model()->moveRow({}, 1, {}, 0));
            EXPECT_EQ(catalog.preferred_product(), DataProduct::finals2000a);
            EXPECT_TRUE(priority->model()->moveRow({}, 1, {}, 0));
            EXPECT_EQ(catalog.preferred_product(), DataProduct::c04);
            capture(dialog, "priority-c04");
            EXPECT_TRUE(priority->model()->moveRow({}, 1, {}, 0));
            EXPECT_EQ(catalog.preferred_product(), DataProduct::finals2000a);
            priority->setCurrentRow(1);
            dialog->findChild<QToolButton *>("eopPriorityUp")->click();
            EXPECT_EQ(catalog.preferred_product(), DataProduct::c04);
            dialog->findChild<QToolButton *>("eopPriorityDown")->click();
            EXPECT_EQ(catalog.preferred_product(), DataProduct::finals2000a);
            capture(dialog, "priority");
            list->setCurrentItem(finals);
            EXPECT_FALSE(priority->isVisible());
            EXPECT_TRUE(badge->text().contains(QStringLiteral("示例")));
            EXPECT_TRUE(checked->text().contains(QStringLiteral("尚未检查")));
            list->setCurrentItem(c04);
            EXPECT_EQ(catalog.preferred_product(), DataProduct::finals2000a);
            EXPECT_TRUE(badge->text().contains(QStringLiteral("未下载")));
            EXPECT_FALSE(dialog->findChild<QPushButton *>("importEopSnapshot")->isVisible());
            EXPECT_TRUE(download->isVisible());
            ASSERT_TRUE(pages);
            EXPECT_EQ(pages->currentIndex(), 0);
            EXPECT_FALSE(automatic->isVisible());
            capture(dialog, "missing-c04");
            pages->setCurrentIndex(1);
            EXPECT_TRUE(automatic->isVisible());
            automatic->setChecked(false);
            EXPECT_FALSE(catalog.automatic_updates(DataProduct::c04));
            EXPECT_TRUE(catalog.automatic_updates());
            capture(dialog, "c04-settings");
            pages->setCurrentIndex(2);
            EXPECT_TRUE(dialog->findChild<QPushButton *>("importEopSnapshot")->isVisible());
            list->setCurrentItem(finals);
            EXPECT_EQ(pages->currentIndex(), 0);
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
            list->setCurrentItem(c04);
            EXPECT_FALSE(progress->isVisible());
            EXPECT_FALSE(cancel->isVisible());
            EXPECT_TRUE(finals->text(0).contains(QStringLiteral("正在下载")));
            EXPECT_TRUE(checked->text().contains(QStringLiteral("尚未检查")));
            list->setCurrentItem(utc);
            EXPECT_FALSE(priority->isVisible());
            EXPECT_FALSE(progress->isVisible());
            EXPECT_FALSE(cancel->isVisible());
            EXPECT_TRUE(checked->text().contains(QStringLiteral("尚未检查")));
            list->setCurrentItem(group);
            EXPECT_TRUE(priority->isVisible());
            EXPECT_FALSE(download->isVisible());
            EXPECT_FALSE(progress->isVisible());
            list->setCurrentItem(finals);
            EXPECT_TRUE(progress->isVisible());
            list->setCurrentItem(c04);
            server.release();
            ++stage;
            break;
        case 2: {
            if (catalog.downloading())
                break;
            EXPECT_EQ(list->currentItem(), c04);
            EXPECT_TRUE(badge->text().contains(QStringLiteral("未下载")));
            list->setCurrentItem(finals);
            EXPECT_TRUE(badge->text().contains(QStringLiteral("完整包")));
            EXPECT_FALSE(progress->isVisible());
            EXPECT_FALSE(cancel->isVisible());
            EXPECT_TRUE(checked->text().contains(QStringLiteral("检查成功")));
            EXPECT_TRUE(download->isEnabled());
            downloaded_at = catalog.inventory()[0].acquired_at;
            EXPECT_TRUE(downloaded_at);
            ASSERT_TRUE(pages);
            auto *overview = qobject_cast<QScrollArea *>(pages->widget(0));
            ASSERT_TRUE(overview);
            EXPECT_EQ(overview->horizontalScrollBar()->maximum(), 0);
            overview->ensureWidgetVisible(download);
            EXPECT_TRUE(overview->viewport()->rect().intersects(
                QRect(download->mapTo(overview->viewport(), QPoint()), download->size())));
            capture(dialog, "downloaded");
            pages->setCurrentIndex(2);
            capture(dialog, "finals-file");
            pages->setCurrentIndex(0);
            server.hold = false;
            catalog.start_download(server.url());
            ++stage;
            break;
        }
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
            list->setCurrentItem(c04);
            EXPECT_FALSE(status->isVisible());
            EXPECT_TRUE(finals->text(0).contains(QStringLiteral("检查失败")));
            list->setCurrentItem(utc);
            EXPECT_FALSE(status->isVisible());
            EXPECT_TRUE(download->isVisible());
            EXPECT_TRUE(checked->text().contains(QStringLiteral("尚未检查")));
            capture(dialog, "utc");
            pages->setCurrentIndex(2);
            capture(dialog, "utc-file");
            list->setCurrentItem(finals);
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
TEST(DataCenterDialog, ResponsiveNavigationPreservesSelectionSettingsAndDownload) {
    (void)test_app();
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    QFile file(QString::fromStdWString(
        (std::filesystem::path(ORBITFORGE_TEST_FINALS).parent_path() / "finals2000A.data")
            .wstring()));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    DownloadServer server(file.readAll());
    EopCatalog catalog(root / "data", {ORBITFORGE_TEST_FINALS}, {}, false);
    QTimer timeout;
    QObject::connect(&timeout, &QTimer::timeout, [] {
        ADD_FAILURE() << "Responsive data center test timed out";
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    timeout.start(5000);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        const auto settle = [] {
            for (int i = 0; i < 12; ++i)
                QCoreApplication::processEvents();
        };
        const auto capture = [&](const QString &name) {
            settle();
            for (auto *scroll : dialog->findChildren<QScrollArea *>())
                if (scroll->isVisible())
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0) << name.toStdString();
            const auto output = qEnvironmentVariable("ORBITFORGE_RESPONSIVE_UI_CAPTURE");
            if (!output.isEmpty()) {
                ASSERT_TRUE(QDir().mkpath(output));
                EXPECT_TRUE(dialog->grab().save(output + "/data-" + name + ".png"));
            }
        };
        auto *list = dialog->findChild<QTreeWidget *>("dataInventory");
        auto *panel = dialog->findChild<QWidget *>("dataDetailPanel");
        auto *pages = dialog->findChild<QTabWidget *>("dataPages");
        auto *back = dialog->findChild<QPushButton *>("dataBackToList");
        ASSERT_TRUE(list && panel && pages && back);
        auto *finals = list->topLevelItem(0)->child(0);
        list->setCurrentItem(finals);
        pages->setCurrentIndex(1);
        auto *automatic = dialog->findChild<QCheckBox *>("dataAutoUpdate");
        auto *days = dialog->findChild<QSpinBox *>("eopExtrapolationDays");
        automatic->setChecked(false);
        dialog->findChild<QCheckBox *>("allowEopExtrapolation")->setChecked(true);
        days->setValue(5);
        dialog->resize(360, 620);
        settle();
        EXPECT_EQ(dialog->width(), 360);
        EXPECT_TRUE(list->isVisible());
        EXPECT_FALSE(panel->isVisible());
        capture("narrow-list");
        // Keyboard activation must offer the same navigation as clicking a row.
        list->itemActivated(finals, 0);
        settle();
        EXPECT_TRUE(panel->isVisible());
        EXPECT_FALSE(list->isVisible());
        EXPECT_TRUE(back->isVisible());
        EXPECT_EQ(pages->currentIndex(), 1);
        EXPECT_FALSE(automatic->isChecked());
        EXPECT_EQ(days->value(), 5);
        capture("narrow-settings");
        pages->setCurrentIndex(2);
        capture("narrow-file");
        back->click();
        EXPECT_TRUE(list->isVisible());
        EXPECT_EQ(list->currentItem(), finals);
        list->itemClicked(finals, 0);
        EXPECT_EQ(pages->currentIndex(), 2);
        for (int width : {1020, 360, 780, 1020}) {
            dialog->resize(width, 620);
            settle();
            EXPECT_EQ(dialog->width(), width);
            EXPECT_EQ(list->currentItem(), finals);
            EXPECT_EQ(pages->currentIndex(), 2);
            EXPECT_TRUE(panel->isVisible());
            EXPECT_EQ(list->isVisible(), width >= 800);
        }
        capture("wide-file");
        pages->setCurrentIndex(0);
        catalog.start_download(server.url());
        QElapsedTimer elapsed;
        elapsed.start();
        while (catalog.received_bytes() == 0 && elapsed.elapsed() < 1500)
            QCoreApplication::processEvents();
        EXPECT_GT(catalog.received_bytes(), 0);
        dialog->resize(360, 620);
        settle();
        // Refresh the download UI through its normal timer before checking the actions.
        auto *cancel = dialog->findChild<QPushButton *>("cancelEopDownload");
        while (!cancel->isVisible() && elapsed.elapsed() < 2500)
            QCoreApplication::processEvents();
        EXPECT_TRUE(catalog.downloading());
        EXPECT_TRUE(cancel->isVisible());
        auto *overview = qobject_cast<QScrollArea *>(pages->widget(0));
        settle();
        overview->ensureWidgetVisible(cancel);
        capture("narrow-download");
        EXPECT_TRUE(overview->viewport()->rect().contains(
            cancel->mapTo(overview->viewport(), cancel->rect().bottomRight())));
        cancel->click();
        while (catalog.downloading() && elapsed.elapsed() < 3000)
            QCoreApplication::processEvents();
        EXPECT_FALSE(catalog.downloading());
        EXPECT_EQ(catalog.update_status().state, DataUpdateState::cancelled);
        dialog->reject();
    });
    show_data_center(nullptr, catalog);
}
} // namespace
} // namespace orbitforge::desktop
