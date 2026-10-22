#include "../src/viewer/eop_catalog.hpp"
#include "../src/viewer/epoch_editor.hpp"
#include "../src/viewer/form_kit.hpp"
#include "../src/viewer/ground_station_editor.hpp"
#include "../src/viewer/hpop_covariance_editor.hpp"
#include "../src/viewer/hpop_settings.hpp"
#include "../src/viewer/mission_editor.hpp"
#include "../src/viewer/object_selection.hpp"
#include "../src/viewer/orbit_editor.hpp"
#include "../src/viewer/project_update.hpp"
#include "../src/viewer/quantity_editor.hpp"
#include "../src/viewer/report_names.hpp"
#include "../src/viewer/sensor_editor.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "window_show_observer.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <numbers>
#include <orbelle/runtime/ephemeris.hpp>
#include <orbelle/runtime/j2.hpp>
#include <orbelle/runtime/j4.hpp>
#include <orbelle/runtime/time_frames.hpp>
#include <orbelle/runtime/two_body.hpp>

#include "support/hpop_test_support.hpp"
#include "support/property_editor_fixture.hpp"

namespace orbelle::desktop {
TEST_F(QuantityTest, HpopMissingGravityStartsOwnedDownloadAndCancelCloseCanRetryWithoutCommitting) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    auto options = new_hpop_options();
    const auto original = options;
    for (int attempt = 0; attempt < 2; ++attempt) {
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            auto *model = dialog->findChild<QComboBox *>("hpopGravityModel");
            EXPECT_EQ(model->currentData().toString(), "egm2008");
            EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityDegree")->value(), 21);
            // Trigger the same request before processing network events: this test is offline.
            dialog->findChild<QPushButton *>("hpopDownloadGravity")->click();
            EXPECT_TRUE(catalog.downloading());
            EXPECT_EQ(catalog.active_product(), DataProduct::egm2008);
            EXPECT_TRUE(dialog->findChild<QProgressBar *>("hpopGravityProgress")->isVisible());
            EXPECT_FALSE(
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
            if (attempt == 1)
                dialog->findChild<QPushButton *>("hpopCancelGravityDownload")->click();
            dialog->findChild<QSpinBox *>("hpopGravityDegree")->setValue(30);
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
        EXPECT_EQ(options, original);
        settle_hpop_catalog(catalog);
        EXPECT_EQ(catalog.update_status(DataProduct::egm2008).state, DataUpdateState::cancelled);
        EXPECT_TRUE(catalog.data().gravity_models().empty());
    }
}

TEST_F(QuantityTest, HpopGravityFailureIsVisibleAndNeverRetriesOrFallsBackAutomatically) {
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    QTcpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost));
    int requests = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            socket->readAll();
            ++requests;
            socket->write(
                "HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\ninvalid");
            socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });
    catalog.start_download(
        QUrl(QStringLiteral("http://127.0.0.1:%1/gravity").arg(server.serverPort())),
        DataProduct::egm2008);
    auto options = new_hpop_options();
    const auto original = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        // It observes the shared download without owning or cancelling it.
        EXPECT_TRUE(dialog->findChild<QPushButton *>("hpopCancelGravityDownload")->isHidden());
        settle_hpop_catalog(catalog);
        EXPECT_EQ(catalog.update_status(DataProduct::egm2008).state, DataUpdateState::failed);
        EXPECT_EQ(requests, 1);
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopGravityModel")->currentData().toString(),
                  "egm2008");
        EXPECT_TRUE(dialog->findChild<QLabel *>("hpopGravityStatus")
                        ->text()
                        .contains(QStringLiteral("重试")));
        EXPECT_TRUE(dialog->findChild<QPushButton *>("hpopDownloadGravity")->isVisible());
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(options, original);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_EQ(options, original);
    EXPECT_EQ(requests, 1);
}

TEST_F(QuantityTest, HpopQueuedGravityCanBeCancelledWithoutCancellingAnUnrelatedDownload) {
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    QTcpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost));
    catalog.start_download(
        QUrl(QStringLiteral("http://127.0.0.1:%1/pending").arg(server.serverPort())));
    auto options = new_hpop_options();
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QPushButton *>("hpopDownloadGravity")->click();
        EXPECT_TRUE(dialog->findChild<QLabel *>("hpopGravityStatus")
                        ->text()
                        .contains(QStringLiteral("等待")));
        dialog->findChild<QPushButton *>("hpopCancelGravityDownload")->click();
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_TRUE(catalog.downloading());
    EXPECT_EQ(catalog.active_product(), DataProduct::finals2000a);
    catalog.cancel_download();
    settle_hpop_catalog(catalog);
}

TEST_F(QuantityTest,
       HpopBackgroundGravityPublicationRefreshesLimitsAndCachedReopenDoesNotDownload) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    EopCatalog catalog(root / "data", {}, {}, false);
    QFile file(temporary.path() + "/local.grv");
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("stk.v.6.0\nModel Local\nDegree 21\nOrder 12\nGm 3.95e14\nRefDistance 6378137\n"
               "Normalized Yes\nIncludesPermTide No\nBEGIN Coefficients\n2 0 -0.000484 0\nEND "
               "Coefficients\n");
    file.close();
    catalog.import_raw(root / "local.grv", DataProduct::custom_gravity);
    core::HpopOptions options;
    options.gravity_model = "custom";
    const auto original = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        settle_hpop_catalog(catalog);
        auto *degree = dialog->findChild<QSpinBox *>("hpopGravityDegree");
        EXPECT_EQ(degree->maximum(), 21);
        degree->setValue(21);
        dialog->findChild<QSpinBox *>("hpopGravityOrder")->setValue(12);
        EXPECT_TRUE(dialog->findChild<QLabel *>("hpopGravityStatus")->text().contains("21"));
        EXPECT_EQ(options, original);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_EQ(options, original);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_FALSE(catalog.downloading());
        EXPECT_TRUE(dialog->findChild<QPushButton *>("hpopDownloadGravity")->isHidden());
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopGravityModel")->currentText(),
                  QStringLiteral("Local · 外部文件"));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_EQ(options.gravity_mu, 3.95e14);
    EXPECT_FALSE(catalog.downloading());
}

TEST_F(QuantityTest, HpopOceanIndependentTruncationKeyboardResizeReopenAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    EopCatalog catalog(root / "data", {}, {}, false);
    QFile file(temporary.path() + "/test.pot");
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(
        "Coefficients to compute variations in normalized Stokes coefficients (unit = 10^-11)\n"
        "Ocean tide model: Test up to (4,2)\n(test)\nDoodson Darw l m DelC+ DelS+ DelC- DelS-\n");
    for (int n = 2; n <= 4; ++n)
        for (int m = 0; m <= 2; ++m)
            file.write(QString("55.555 Test %1 %2 1 2 3 4\n").arg(n).arg(m).toUtf8());
    file.close();
    catalog.data().install_ocean_tides(root / "test.pot", "custom-ocean");
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.ocean_tides = {false, "custom-ocean", 4, 2};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *use = dialog->findChild<QCheckBox *>("hpopUseOceanTides");
        auto *degree = dialog->findChild<QSpinBox *>("hpopOceanTideDegree");
        auto *order = dialog->findChild<QSpinBox *>("hpopOceanTideOrder");
        ASSERT_TRUE(use);
        ASSERT_TRUE(degree);
        ASSERT_TRUE(order);
        EXPECT_FALSE(degree->isEnabled());
        reveal(use);
        use->setFocus();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(use, &press);
        QApplication::sendEvent(use, &release);
        EXPECT_TRUE(use->isChecked());
        EXPECT_TRUE(degree->isEnabled());
        auto *accept = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        degree->setValue(5);
        accept->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_FALSE(options.ocean_tides.enabled);
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("errorText")->text().contains(QStringLiteral("可用范围")));
        degree->setValue(4);
        const auto capture = qEnvironmentVariable("ORBELLE_HPOP_UI_CAPTURE");
        std::cout << "HPOP ocean tide device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            auto *scroll = dialog->findChild<QScrollArea *>("hpopGravityPage");
            scroll->ensureWidgetVisible(order);
            QApplication::processEvents();
            EXPECT_EQ(degree, dialog->findChild<QSpinBox *>("hpopOceanTideDegree"));
            EXPECT_EQ(degree->value(), 4);
            EXPECT_EQ(order->value(), 2);
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_FALSE(dialog->findChild<QLabel *>("hpopOceanTideStatus")
                             ->text()
                             .contains(QStringLiteral("已安装")));
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-ocean-%1.png").arg(width)));
            }
        }
        // An unrelated official-model download must not block this available custom model.
        catalog.start_download(QUrl("http://127.0.0.1:1"), DataProduct::fes2014bv1);
        use->setChecked(false);
        EXPECT_FALSE(degree->isEnabled());
        EXPECT_EQ(order->value(), 2);
        use->setChecked(true);
        EXPECT_TRUE(accept->isEnabled());
        EXPECT_TRUE(dialog->findChild<QProgressBar *>("hpopOceanTideProgress")->isHidden());
        accept->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_TRUE(catalog.downloading());
    catalog.cancel_download();
    settle_hpop_catalog(catalog);
    EXPECT_EQ(options.gravity_degree, 0);
    EXPECT_EQ(options.ocean_tides, (core::OceanTideOptions{true, "custom-ocean", 4, 2}));
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopUseOceanTides")->isChecked());
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopOceanTideDegree")->value(), 4);
        dialog->findChild<QSpinBox *>("hpopOceanTideOrder")->setValue(0);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopMissingOceanDownloadsAreOwnedCancellableAndRetryOnReopen) {
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    core::HpopOptions options;
    options.ocean_tides.enabled = true;
    const auto saved = options;
    for (int attempt = 0; attempt < 2; ++attempt) {
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QPushButton *>("hpopDownloadOceanTides")->click();
            EXPECT_TRUE(catalog.downloading());
            EXPECT_EQ(catalog.active_product(), DataProduct::fes2014bv1);
            EXPECT_FALSE(
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
            if (attempt)
                dialog->findChild<QPushButton *>("hpopCancelOceanDownload")->click();
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
        settle_hpop_catalog(catalog);
        EXPECT_EQ(options, saved);
        EXPECT_EQ(catalog.update_status(DataProduct::fes2014bv1).state, DataUpdateState::cancelled);
    }
}

TEST_F(QuantityTest, HpopQueuedOceanCancellationPreservesUnrelatedDownloadAndDisabledDraft) {
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    catalog.start_download(QUrl("http://127.0.0.1:1"), DataProduct::space_weather);
    core::HpopOptions options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *use = dialog->findChild<QCheckBox *>("hpopUseOceanTides");
        use->setChecked(true);
        EXPECT_TRUE(dialog->findChild<QLabel *>("hpopOceanTideStatus")
                        ->text()
                        .contains(QStringLiteral("等待")));
        dialog->findChild<QPushButton *>("hpopCancelOceanDownload")->click();
        EXPECT_TRUE(catalog.downloading());
        EXPECT_EQ(catalog.active_product(), DataProduct::space_weather);
        use->setChecked(false);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_FALSE(options.ocean_tides.enabled);
    EXPECT_EQ(options.ocean_tides.degree, 4);
    catalog.cancel_download();
    settle_hpop_catalog(catalog);
}
} // namespace orbelle::desktop
