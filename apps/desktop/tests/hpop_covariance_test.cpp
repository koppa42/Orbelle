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
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
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

#include "support/property_editor_fixture.hpp"

namespace orbelle::desktop {
TEST_F(QuantityTest, HpopCovarianceEditorPreservesDraftsAndMirrorsLowerTriangle) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    application::HpopCovarianceSettings s;
    HpopCovarianceEditor editor(s, true);
    editor.resize(700, 900);
    editor.show();
    auto *enabled = editor.findChild<QCheckBox *>("hpopComputeCovariance");
    auto *matrix = editor.findChild<QTableWidget *>("hpopCovarianceMatrix");
    EXPECT_FALSE(matrix->isEnabled());
    enabled->setChecked(true);
    for (int i = 0; i < 6; ++i)
        matrix->item(i, i)->setText(i < 3 ? "10000" : "1");
    matrix->item(2, 0)->setText("1.2345678901234567");
    EXPECT_EQ(matrix->item(0, 2)->text(), matrix->item(2, 0)->text());
    EXPECT_FALSE(matrix->item(0, 2)->flags() & Qt::ItemIsEditable);
    auto *consider = editor.findChild<QCheckBox *>("hpopConsiderAnalysis");
    consider->setChecked(true);
    auto *parameters = editor.findChild<QTableWidget *>("hpopConsiderTable");
    parameters->item(0, 0)->setCheckState(Qt::Checked);
    parameters->item(1, 0)->setCheckState(Qt::Checked);
    parameters->item(0, 2)->setText("1e-4");
    parameters->item(1, 2)->setText("2e-4");
    editor.findChild<QCheckBox *>("hpopConsiderCrossCorrelation")->setChecked(true);
    editor.findChild<QTableWidget *>("hpopConsiderCrossTable")->item(0, 2)->setText("5e-5");
    auto value = editor.value();
    EXPECT_NO_THROW(application::validate_hpop_covariance_settings(value, {}));
    const auto capture = qEnvironmentVariable("ORBELLE_HPOP_UI_CAPTURE");
    const auto screenshot = [&](const QString &name) {
        QApplication::processEvents();
        if (!capture.isEmpty()) {
            QDir{}.mkpath(capture);
            EXPECT_TRUE(editor.grab().save(capture + "/" + name + ".png"));
        }
    };
    screenshot("covariance-editor-wide");
    editor.resize(360, 900);
    screenshot("covariance-editor-narrow");
    EXPECT_EQ(editor.value(), value);
    parameters->item(1, 0)->setCheckState(Qt::Unchecked);
    EXPECT_FALSE(parameters->item(1, 2)->flags() & Qt::ItemIsEditable);
    EXPECT_FALSE(editor.findChild<QTableWidget *>("hpopConsiderCrossTable")->isEnabled());
    screenshot("covariance-editor-inactive-srp");
    parameters->item(1, 0)->setCheckState(Qt::Checked);
    EXPECT_EQ(editor.value(), value);
    enabled->setChecked(false);
    EXPECT_FALSE(matrix->isEnabled());
    value.enabled = false;
    EXPECT_EQ(editor.value(), value);
    matrix->item(1, 0)->setText("not a number");
    EXPECT_EQ(editor.value(), value);
    enabled->setChecked(true);
    EXPECT_THROW((void)editor.value(), std::invalid_argument);
}

TEST_F(QuantityTest, HpopCovarianceInactiveInvalidDraftsRetainValidValues) {
    application::HpopCovarianceSettings initial;
    for (int i = 0; i < 6; ++i)
        initial.state[i][i] = 1;
    initial.enabled = true;
    initial.drag.variance = .1;
    HpopCovarianceEditor editor(initial, true);
    auto *parameters = editor.findChild<QTableWidget *>("hpopConsiderTable");
    auto *consider = editor.findChild<QCheckBox *>("hpopConsiderAnalysis");
    consider->setChecked(true);
    parameters->item(0, 0)->setCheckState(Qt::Checked);
    parameters->item(0, 2)->setText("-1");
    EXPECT_THROW((void)editor.value(), std::invalid_argument);
    parameters->item(0, 0)->setCheckState(Qt::Unchecked);
    EXPECT_FALSE(parameters->item(0, 2)->flags() & Qt::ItemIsEditable);
    parameters->item(0, 3)->setText("0.02");
    auto value = editor.value();
    EXPECT_EQ(value.drag.variance, .1);
    EXPECT_EQ(value.drag.state_cross_covariance[0], .02);
    EXPECT_NO_THROW(application::validate_hpop_covariance_settings(value, {}));
    auto *matrix = editor.findChild<QTableWidget *>("hpopCovarianceMatrix");
    matrix->item(0, 0)->setText("-2");
    matrix->item(1, 0)->setText("unfinished");
    editor.findChild<QLineEdit *>("hpopCovarianceTolerance")->setText("0");
    editor.findChild<QSpinBox *>("hpopCovarianceDegree")->setValue(1);
    editor.findChild<QCheckBox *>("hpopComputeCovariance")->setChecked(false);
    value = editor.value();
    EXPECT_EQ(value.state, initial.state);
    EXPECT_EQ(value.absolute_tolerance, initial.absolute_tolerance);
    EXPECT_EQ(value.gravity, initial.gravity);
    EXPECT_NO_THROW(application::validate_hpop_covariance_settings(value, {}));
}

TEST_F(QuantityTest, HpopCovarianceRejectsGravityBeyondSelectedFileBeforeAccept) {
    core::HpopOptions force;
    force.gravity_degree = force.gravity_order = 0;
    application::HpopCovarianceSettings settings;
    settings.enabled = settings.independent_gravity = true;
    for (int i = 0; i < 6; ++i)
        settings.state[i][i] = 1;
    const auto before = settings;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *degree = dialog->findChild<QSpinBox *>("hpopCovarianceDegree");
        degree->setValue(core::egm96_max_degree + 1);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(settings, before);
        EXPECT_EQ(dialog->findChild<QTabWidget *>("hpopSettingsTabs")->currentIndex(), 2);
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->text().contains("重力文件"));
        degree->setValue(4);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        if (dialog->isVisible())
            dialog->reject();
    });
    ASSERT_TRUE(edit_hpop_settings(force, nullptr, {}, {}, {}, nullptr, &settings));
    EXPECT_EQ(settings.gravity.degree, 4);
}

TEST_F(QuantityTest, HpopCovarianceDialogRejectsInvalidJointInputAndCancelsAtomically) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions force;
    application::HpopCovarianceSettings settings;
    const auto before = settings;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QCheckBox *>("hpopComputeCovariance")->setChecked(true);
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        buttons->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(dialog->findChild<QTabWidget *>("hpopSettingsTabs")->currentIndex(), 2);
        EXPECT_EQ(settings, before);
        auto *matrix = dialog->findChild<QTableWidget *>("hpopCovarianceMatrix");
        for (int i = 0; i < 6; ++i)
            matrix->item(i, i)->setText(i < 3 ? "10000" : "1");
        dialog->findChild<QLabel *>("errorText")->hide();
        auto *scroll = dialog->findChild<QScrollArea *>("hpopCovariancePage");
        const auto directory = qEnvironmentVariable("ORBELLE_HPOP_UI_CAPTURE");
        for (const int width : {780, 380}) {
            dialog->resize(width, 680);
            // Navigation and nested forms post separate layout requests after a resize.
            for (int pass = 0; pass < 3; ++pass)
                QApplication::processEvents();
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            for (const bool bottom : {false, true}) {
                scroll->verticalScrollBar()->setValue(
                    bottom ? scroll->verticalScrollBar()->maximum() : 0);
                QApplication::processEvents();
                if (!directory.isEmpty()) {
                    ASSERT_TRUE(QDir{}.mkpath(directory));
                    EXPECT_TRUE(
                        dialog->grab().save(directory + QString("/covariance-dialog-%1-%2.png")
                                                            .arg(width)
                                                            .arg(bottom ? "bottom" : "top")));
                }
                EXPECT_TRUE(buttons->button(QDialogButtonBox::Ok)->isVisible());
            }
        }
        EXPECT_EQ(matrix->item(0, 0)->text(), "10000");
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    ASSERT_TRUE(edit_hpop_settings(force, nullptr, {}, {}, {}, nullptr, &settings));
    EXPECT_TRUE(settings.enabled);
    EXPECT_EQ(settings.state[0][0], 10000);
    const auto saved = settings;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QTableWidget *>("hpopCovarianceMatrix")->item(0, 0)->setText("2");
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(force, nullptr, {}, {}, {}, nullptr, &settings));
    EXPECT_EQ(settings, saved);
}
} // namespace orbelle::desktop
