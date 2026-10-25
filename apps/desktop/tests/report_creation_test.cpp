#include "../src/viewer/report_creation.hpp"
#include "../src/viewer/report_dialog.hpp"
#include "support/report_test_support.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <functional>
#include <gtest/gtest.h>
#include <orbelle/runtime/time_frames.hpp>

namespace orbelle::desktop {
namespace {
using namespace application;
class ReportCreation : public ::testing::Test {
  protected:
    void SetUp() override { (void)report_test::test_app(); }
    static Project project() {
        Project project("Mission", core::Epoch{0}, core::Epoch{120'000'000'000});
        ProjectSatellite satellite;
        satellite.orbit.object_id = "alpha";
        satellite.display_name = "晨星";
        project.satellites.push_back(satellite);
        satellite.orbit.object_id = "beta";
        satellite.display_name = "远航";
        project.satellites.push_back(satellite);
        project.ground_stations.push_back({"station", "东站"});
        project.sensors.push_back({"sensor", "观测仪", "alpha"});
        project.regions.push_back({"region", "研究区"});
        return project;
    }
    static QPushButton *next(ReportCreationDialog &dialog) {
        return dialog.findChild<QPushButton *>("reportCreationNext");
    }
    static QListWidgetItem *choice(ReportCreationDialog &dialog, const QString &key) {
        auto *list = dialog.findChild<QListWidget *>("reportSelectionList");
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->data(Qt::UserRole).toString() == key)
                return list->item(row);
        return nullptr;
    }
    static void select(ReportCreationDialog &dialog, ReportKind kind) {
        const auto key = QStringLiteral("builtin:%1").arg(static_cast<int>(kind));
        auto *item = choice(dialog, key);
        ASSERT_NE(item, nullptr);
        dialog.findChild<QListWidget *>("reportSelectionList")->setCurrentItem(item);
    }
    static QWidget *page(ReportCreationDialog &dialog) {
        return dialog.findChild<QStackedWidget *>("reportCreationPages")->currentWidget();
    }
    static QTreeWidgetItem *object(QWidget &dialog, const char *id) {
        auto *tree = dialog.findChild<QTreeWidget *>("objectSelectionTree");
        if (!tree)
            return nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toString() == QString::fromUtf8(id))
                return *it;
        return nullptr;
    }
    static ObjectSelectionField *field(ReportCreationDialog &dialog, int role = 0) {
        return page(dialog)->findChild<ObjectSelectionField *>(
            QStringLiteral("reportCreationObject%1").arg(role));
    }
    static void picker(ReportCreationDialog &dialog, int role,
                       const std::function<void(ObjectSelectionDialog &)> &inspect,
                       bool accept = false) {
        auto *button = field(dialog, role);
        ASSERT_NE(button, nullptr);
        bool visited = false;
        QTimer::singleShot(0, &dialog, [&] {
            auto *modal = qobject_cast<ObjectSelectionDialog *>(QApplication::activeModalWidget());
            EXPECT_NE(modal, nullptr);
            if (modal) {
                visited = true;
                inspect(*modal);
                if (accept)
                    modal->accept();
                if (modal->result() != QDialog::Accepted)
                    modal->reject();
            }
        });
        button->click();
        EXPECT_TRUE(visited);
    }
    static void check_object(QWidget &dialog, const char *id, bool multiple = false) {
        auto *item = object(dialog, id);
        ASSERT_NE(item, nullptr);
        dialog.findChild<QTreeWidget *>("objectSelectionTree")->setCurrentItem(item);
        if (multiple)
            item->setCheckState(0, Qt::Checked);
    }
    static void pick(ReportCreationDialog &dialog, const char *id, bool multiple = false,
                     int role = 0) {
        picker(dialog, role, [&](auto &modal) { check_object(modal, id, multiple); }, true);
    }
};

TEST_F(ReportCreation, TypeFirstExplicitObjectsAndBackKeepDraft) {
    ReportCreationDialog dialog(project());
    dialog.show();
    EXPECT_FALSE(next(dialog)->isEnabled());
    select(dialog, ReportKind::cartesian_state);
    next(dialog)->click();
    EXPECT_FALSE(next(dialog)->isEnabled());
    EXPECT_EQ(page(dialog)->findChild<QTreeWidget *>(), nullptr);
    picker(
        dialog, 0,
        [&](auto &modal) {
            EXPECT_FALSE(modal.template findChild<QCheckBox *>("objectSelectionAll")->isChecked());
            auto *types = modal.template findChild<QComboBox *>("objectSelectionKind");
            EXPECT_EQ(types->count(), 3); // All, satellite, ground station; never sensors.
            EXPECT_EQ(types->findData(static_cast<int>(ObjectKind::sensor)), -1);
            EXPECT_EQ(object(modal, "sensor"), nullptr);
            check_object(modal, "alpha", true);
            check_object(modal, "station", true);
            modal.template findChild<QLineEdit *>("objectSelectionSearch")->setText("no match");
        },
        true);
    EXPECT_TRUE(next(dialog)->isEnabled()); // Filtering preserves checked objects.
    dialog.findChild<QPushButton *>("reportCreationBack")->click();
    next(dialog)->click();
    picker(dialog, 0, [&](auto &modal) {
        EXPECT_EQ(object(modal, "alpha")->checkState(0), Qt::Checked);
        EXPECT_EQ(object(modal, "station")->checkState(0), Qt::Checked);
        check_object(modal, "beta", true); // Cancelling this picker preserves the two original IDs.
    });
    next(dialog)->click();
    ASSERT_EQ(dialog.result(), QDialog::Accepted);
    EXPECT_EQ(dialog.definition().query.kind, ReportKind::cartesian_state);
    EXPECT_EQ(dialog.definition().query.scope, ReportScope::objects);
    EXPECT_EQ(dialog.definition().query.object_ids, (std::vector<std::string>{"alpha", "station"}));
}

TEST_F(ReportCreation, AllObjectsMustBeExplicitlyChosenInPopup) {
    ReportCreationDialog dialog(project());
    select(dialog, ReportKind::cartesian_state);
    dialog.accept();
    EXPECT_FALSE(next(dialog)->isEnabled());
    picker(
        dialog, 0,
        [&](auto &modal) {
            modal.template findChild<QCheckBox *>("objectSelectionAll")->setChecked(true);
        },
        true);
    EXPECT_TRUE(next(dialog)->isEnabled());
    dialog.accept();
    EXPECT_EQ(dialog.definition().query.scope, ReportScope::all_compatible);
    EXPECT_TRUE(dialog.definition().query.object_ids.empty());
}

TEST_F(ReportCreation, OpenPickerTracksObjectChangesWithoutCommittingCancelledChoice) {
    auto input = project();
    ReportCreationDialog dialog(input, "sensor");
    select(dialog, ReportKind::aer);
    dialog.accept();
    pick(dialog, "station");
    picker(dialog, 0, [&](auto &modal) {
        input.ground_stations.front().display_name = "新名称";
        dialog.update_project(input);
        EXPECT_EQ(object(modal, "station")->text(0), QStringLiteral("新名称"));
        input.ground_stations.clear();
        input.ground_stations.push_back({"replacement", "新名称"});
        dialog.update_project(input);
        modal.accept();
        EXPECT_NE(modal.result(), QDialog::Accepted);
        check_object(modal, "replacement");
        // The outer picker helper cancels, even though the replacement is a valid draft.
    });
    EXPECT_EQ(field(dialog)->id(), "station");
    EXPECT_FALSE(next(dialog)->isEnabled());
    pick(dialog, "replacement");
    EXPECT_TRUE(next(dialog)->isEnabled());
}

TEST_F(ReportCreation, ContextFiltersTypesAndOnlyAsksForMissingPartner) {
    ReportCreationDialog dialog(project(), "station");
    dialog.show();
    EXPECT_EQ(dialog.findChild<QListWidget *>("reportSelectionList")->count(), 6);
    EXPECT_EQ(choice(dialog, "builtin:0"), nullptr);
    EXPECT_EQ(choice(dialog, "builtin:4"), nullptr);
    EXPECT_EQ(choice(dialog, "builtin:13"), nullptr);
    EXPECT_EQ(choice(dialog, "builtin:15"), nullptr);
    select(dialog, ReportKind::access_intervals);
    next(dialog)->click();
    EXPECT_FALSE(next(dialog)->isEnabled());
    ASSERT_NE(field(dialog, 0), nullptr);
    EXPECT_EQ(field(dialog, 0)->id(), "station");
    EXPECT_TRUE(field(dialog, 0)->isEnabled());
    pick(dialog, "sensor", false, 1);
    next(dialog)->click();
    ASSERT_EQ(dialog.result(), QDialog::Accepted);
    EXPECT_EQ(dialog.definition().query.object_ids,
              (std::vector<std::string>{"station", "sensor"}));

    ReportCreationDialog sensor(project(), "sensor");
    sensor.show();
    EXPECT_EQ(sensor.findChild<QListWidget *>("reportSelectionList")->count(), 4);
    select(sensor, ReportKind::aer);
    next(sensor)->click();
    EXPECT_EQ(field(sensor, 1)->id(), "sensor");
    pick(sensor, "station");
    next(sensor)->click();
    EXPECT_EQ(sensor.definition().query.object_ids,
              (std::vector<std::string>{"station", "sensor"}));
    EXPECT_EQ(sensor.definition().query.step_nanoseconds, 1'000'000'000);
}

TEST_F(ReportCreation, ScenarioSkipsObjectsAndContextShowsPrefilledSummary) {
    ReportCreationDialog general(project());
    select(general, ReportKind::scenario_summary);
    general.accept();
    EXPECT_EQ(general.result(), QDialog::Accepted);
    EXPECT_EQ(general.definition().query.scope, ReportScope::all_compatible);
    EXPECT_TRUE(general.definition().query.object_ids.empty());
    ReportCreationDialog station(project(), "station");
    select(station, ReportKind::ground_stations);
    station.accept();
    EXPECT_NE(station.result(), QDialog::Accepted);
    EXPECT_TRUE(next(station)->isEnabled());
    EXPECT_EQ(field(station)->id(), "station");
    EXPECT_TRUE(field(station)->isEnabled());
    station.accept();
    EXPECT_EQ(station.result(), QDialog::Accepted);
    EXPECT_EQ(station.definition().query.object_ids, (std::vector<std::string>{"station"}));
}

TEST_F(ReportCreation, OrderedPairRequiresDistinctExplicitSatellites) {
    ReportCreationDialog dialog(project());
    dialog.show();
    select(dialog, ReportKind::relative_motion);
    next(dialog)->click();
    EXPECT_FALSE(next(dialog)->isEnabled());
    ASSERT_NE(field(dialog, 0), nullptr);
    ASSERT_NE(field(dialog, 1), nullptr);
    // Both roles are on the same page; users can choose the target first.
    pick(dialog, "alpha", false, 1);
    EXPECT_FALSE(field(dialog, 0)->contains("alpha"));
    pick(dialog, "beta");
    EXPECT_FALSE(field(dialog, 1)->contains("beta"));
    EXPECT_TRUE(next(dialog)->isEnabled());
    next(dialog)->click();
    EXPECT_EQ(dialog.definition().query.object_ids, (std::vector<std::string>{"beta", "alpha"}));
    ReportCreationDialog contextual(project(), "alpha");
    select(contextual, ReportKind::intersatellite_access);
    contextual.accept();
    EXPECT_FALSE(field(contextual, 1)->contains("alpha"));
    pick(contextual, "beta", false, 1);
    contextual.accept();
    EXPECT_EQ(contextual.definition().query.object_ids,
              (std::vector<std::string>{"alpha", "beta"}));
}

TEST_F(ReportCreation, DeletedBindingsAndRenamesNeverSubstituteAnotherObject) {
    auto input = project();
    ReportCreationDialog dialog(input, "sensor");
    dialog.show();
    select(dialog, ReportKind::access_intervals);
    next(dialog)->click();
    pick(dialog, "station");
    input.ground_stations.front().display_name = "重命名测控站";
    dialog.update_project(input);
    EXPECT_TRUE(field(dialog)->text().contains(QStringLiteral("重命名测控站")));
    EXPECT_TRUE(next(dialog)->isEnabled());
    input.ground_stations.clear();
    input.ground_stations.push_back({"replacement", "重命名测控站"});
    dialog.update_project(input);
    EXPECT_FALSE(next(dialog)->isEnabled());
    dialog.accept();
    EXPECT_EQ(dialog.result(), QDialog::Rejected);
    pick(dialog, "replacement");
    EXPECT_TRUE(next(dialog)->isEnabled());
    input.sensors.clear();
    dialog.update_project(input);
    EXPECT_FALSE(next(dialog)->isEnabled());
    pick(dialog, "alpha", false, 1);
    EXPECT_TRUE(next(dialog)->isEnabled());
    dialog.accept();
    EXPECT_EQ(dialog.definition().query.object_ids,
              (std::vector<std::string>{"replacement", "alpha"}));
}

TEST_F(ReportCreation, PrefilledContextCanBeReplacedAndCancelledPickerKeepsOldValue) {
    ReportCreationDialog dialog(project(), "alpha");
    select(dialog, ReportKind::aer);
    dialog.accept();
    EXPECT_EQ(field(dialog, 1)->id(), "alpha");
    picker(dialog, 1, [&](auto &modal) { check_object(modal, "beta"); });
    EXPECT_EQ(field(dialog, 1)->id(), "alpha");
    pick(dialog, "sensor", false, 1);
    pick(dialog, "station");
    dialog.accept();
    EXPECT_EQ(dialog.definition().query.object_ids,
              (std::vector<std::string>{"station", "sensor"}));

    ReportCreationDialog multiple(project(), "station");
    select(multiple, ReportKind::cartesian_state);
    multiple.accept();
    picker(
        multiple, 0,
        [&](auto &modal) {
            object(modal, "station")->setCheckState(0, Qt::Unchecked);
            check_object(modal, "alpha", true);
        },
        true);
    multiple.accept();
    EXPECT_EQ(multiple.definition().query.object_ids, (std::vector<std::string>{"alpha"}));
}

TEST_F(ReportCreation, PagesRestoreUserSizesAndSelections) {
    ReportCreationDialog dialog(project());
    dialog.show();
    dialog.resize(760, 560);
    select(dialog, ReportKind::cartesian_state);
    dialog.accept();
    EXPECT_LT(dialog.width(), 760);
    dialog.resize(600, 520);
    pick(dialog, "alpha", true);
    dialog.findChild<QPushButton *>("reportCreationBack")->click();
    EXPECT_EQ(dialog.size(), QSize(760, 560));
    dialog.accept();
    EXPECT_EQ(dialog.size(), QSize(600, 520));
    EXPECT_EQ(field(dialog)->id(), "alpha");
}

TEST_F(ReportCreation, MultiSelectionHasBoundedRowsAndCanRemoveOrReplaceObjects) {
    auto input = project();
    std::vector<std::string> ids{"alpha", "station"};
    for (int i = 0; i < 8; ++i) {
        auto satellite = input.satellites.front();
        satellite.orbit.object_id = "extra-" + std::to_string(i);
        satellite.display_name = "长名称卫星 / 通信与对地观测星座 / " + std::to_string(i);
        ids.push_back(satellite.orbit.object_id);
        input.satellites.push_back(satellite);
    }
    ReportCreationDialog dialog(input);
    dialog.show();
    select(dialog, ReportKind::cartesian_state);
    dialog.accept();
    field(dialog)->set_selection({ids, false});
    QCoreApplication::processEvents();
    auto *list = page(dialog)->findChild<QListWidget *>("reportCreationSelectedObjects");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->count(), 10);
    EXPECT_TRUE(list->isVisible());
    EXPECT_TRUE(field(dialog)->isHidden());
    EXPECT_LE(list->height(), 4 * std::max(36, list->fontMetrics().height() + 16) + 2);
    EXPECT_TRUE(next(dialog)->isEnabled());
    const auto directory = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
    if (!directory.isEmpty()) {
        QDir().mkpath(directory);
        EXPECT_TRUE(dialog.grab().save(directory + "/report-create-multiple-default.png"));
    }
    dialog.resize(360, 400);
    QCoreApplication::processEvents();
    EXPECT_EQ(dialog.width(), 360);
    EXPECT_EQ(dialog.height(), 400);
    if (!directory.isEmpty())
        EXPECT_TRUE(dialog.grab().save(directory + "/report-create-multiple-narrow.png"));

    const auto row = list->visualItemRect(list->item(0));
    const QPoint local(row.right() - 15, row.center().y());
    for (const auto event : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent mouse(event, local, list->viewport()->mapToGlobal(local), Qt::LeftButton,
                          event == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(list->viewport(), &mouse);
    }
    EXPECT_EQ(field(dialog)->selection().ids.size(), 9U);
    EXPECT_EQ(field(dialog)->selection().ids.front(), "station");
    list->setCurrentRow(0);
    list->findChild<QAction *>("reportCreationRemoveObject")->trigger();
    EXPECT_EQ(field(dialog)->selection().ids.size(), 8U);
    picker(dialog, 0, [&](auto &modal) {
        EXPECT_EQ(object(modal, "extra-0")->checkState(0), Qt::Checked);
        check_object(modal, "beta", true);
    });
    EXPECT_EQ(field(dialog)->selection().ids.size(), 8U);
    field(dialog)->set_selection({{"beta"}, false});
    EXPECT_TRUE(list->isHidden());
    EXPECT_TRUE(field(dialog)->isVisible());
    dialog.accept();
    EXPECT_EQ(dialog.definition().query.object_ids, (std::vector<std::string>{"beta"}));
}

TEST_F(ReportCreation, TemplatesRespectContextAndSurviveBackWithoutSaving) {
    auto input = project();
    ReportDefinition definition;
    definition.id = "template";
    definition.name = "双星相对运动";
    definition.query.kind = ReportKind::relative_motion;
    definition.query.object_ids = {"beta", "alpha"};
    definition.query.step_nanoseconds = 30'000'000'000;
    input.reports.push_back(definition);
    ReportCreationDialog unrelated(input, "station");
    EXPECT_EQ(choice(unrelated, "template:template"), nullptr);
    ReportCreationDialog dialog(input, "alpha");
    dialog.show();
    dialog.findChild<QListWidget *>("reportSelectionList")
        ->setCurrentItem(choice(dialog, "template:template"));
    next(dialog)->click();
    EXPECT_EQ(field(dialog)->id(), "beta");
    next(dialog)->click();
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
    EXPECT_EQ(dialog.definition().query.object_ids, definition.query.object_ids);
    EXPECT_EQ(dialog.definition().query.step_nanoseconds, 30'000'000'000);

    ReportCreationDialog cancelled(input);
    select(cancelled, ReportKind::cartesian_state);
    cancelled.accept();
    pick(cancelled, "beta", true);
    cancelled.reject();
    EXPECT_EQ(cancelled.result(), QDialog::Rejected);
    EXPECT_EQ(input.reports.size(), 1U);
    EXPECT_EQ(input.reports.front().query, definition.query);
}

TEST_F(ReportCreation, MissingObjectsAndCovarianceEligibilityAreVisible) {
    auto input = project();
    ReportCreationDialog ordinary(input, "alpha");
    EXPECT_EQ(choice(ordinary, "builtin:9"), nullptr);
    input.satellites.front().orbit.propagator = OrbitPropagator::hpop;
    input.satellites.front().orbit.covariance.enabled = true;
    ReportCreationDialog covariance(input);
    select(covariance, ReportKind::state_covariance);
    covariance.accept();
    picker(covariance, 0, [&](auto &modal) {
        EXPECT_NE(object(modal, "alpha"), nullptr);
        EXPECT_EQ(object(modal, "beta"), nullptr);
        EXPECT_TRUE(modal.template findChild<QComboBox *>("objectSelectionKind")->isHidden());
    });
    Project empty("Empty", core::Epoch{0}, core::Epoch{1});
    ReportCreationDialog missing(empty);
    select(missing, ReportKind::access_intervals);
    EXPECT_FALSE(next(missing)->isEnabled());
    select(missing, ReportKind::scenario_summary);
    EXPECT_TRUE(next(missing)->isEnabled());
}

TEST_F(ReportCreation, ReportTypeDeterminesSelectableObjectTypes) {
    for (const auto kind : {ReportKind::classical_elements, ReportKind::lighting_state,
                            ReportKind::ground_stations, ReportKind::coverage_grid}) {
        ReportCreationDialog dialog(project());
        select(dialog, kind);
        dialog.accept();
        picker(dialog, kind == ReportKind::coverage_grid ? 1 : 0, [&](auto &modal) {
            auto *types = modal.template findChild<QComboBox *>("objectSelectionKind");
            ASSERT_NE(types, nullptr);
            const bool station = kind == ReportKind::ground_stations;
            EXPECT_EQ(object(modal, "station") != nullptr, station);
            EXPECT_EQ(object(modal, "alpha") != nullptr, !station);
            EXPECT_EQ(object(modal, "sensor") != nullptr, kind == ReportKind::coverage_grid);
            EXPECT_EQ(types->count(), kind == ReportKind::coverage_grid ? 3 : 2);
            EXPECT_EQ(types->isHidden(), kind != ReportKind::coverage_grid);
        });
    }
}

TEST_F(ReportCreation, ChangedTemplateMustBeRechosenAndCancellationPreservesReportSettings) {
    auto input = project();
    ReportDefinition definition;
    definition.id = "saved";
    definition.name = "位置报告";
    definition.query.object_ids = {"alpha"};
    input.reports.push_back(definition);
    ReportCreationDialog chooser(input);
    chooser.findChild<QListWidget *>("reportSelectionList")
        ->setCurrentItem(choice(chooser, "template:saved"));
    chooser.accept();
    input.reports.front().query.step_nanoseconds = 30'000'000'000;
    chooser.update_project(input);
    EXPECT_FALSE(next(chooser)->isEnabled());
    chooser.findChild<QPushButton *>("reportCreationBack")->click();
    chooser.accept();
    chooser.accept();
    EXPECT_EQ(chooser.result(), QDialog::Accepted);
    EXPECT_EQ(chooser.definition().query.step_nanoseconds, 30'000'000'000);

    ReportDialog report(
        input, runtime::make_builtin_time_provider(),
        [](const ReportQuery &, ReportDialog::Ready) {}, [](auto) {}, [](auto) {});
    ASSERT_TRUE(report.configure_report(chooser.definition()));
    auto *scope = report.findChild<QLabel *>("reportScopeLabel");
    const auto before = scope->text();
    ReportCreationDialog cancelled(input, "station");
    select(cancelled, ReportKind::aer);
    cancelled.accept();
    cancelled.reject();
    EXPECT_EQ(scope->text(), before);
    input.reports.clear();
    chooser.update_project(input);
    EXPECT_FALSE(next(chooser)->isEnabled());
}

TEST_F(ReportCreation, NarrowLayoutKeepsNavigationReachable) {
    ReportCreationDialog dialog(project(), "alpha");
    dialog.show();
    select(dialog, ReportKind::access_intervals);
    const auto directory = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
    for (const int width : {840, 360}) {
        dialog.resize(width, 540);
        QCoreApplication::processEvents();
        EXPECT_TRUE(dialog.rect().contains(
            next(dialog)->mapTo(&dialog, next(dialog)->rect().bottomRight())));
        if (!directory.isEmpty()) {
            QDir().mkpath(directory);
            EXPECT_TRUE(
                dialog.grab().save(directory + QString("/report-create-types-%1.png").arg(width)));
        }
    }
    next(dialog)->click();
    QCoreApplication::processEvents();
    if (!directory.isEmpty())
        EXPECT_TRUE(dialog.grab().save(directory + "/report-create-context-objects-default.png"));
    for (const int width : {840, 360}) {
        dialog.resize(width, 540);
        QCoreApplication::processEvents();
        EXPECT_TRUE(dialog.rect().contains(
            next(dialog)->mapTo(&dialog, next(dialog)->rect().bottomRight())));
        EXPECT_TRUE(field(dialog)->isVisible());
        if (!directory.isEmpty())
            EXPECT_TRUE(dialog.grab().save(
                directory + QString("/report-create-context-objects-%1.png").arg(width)));
    }
    dialog.reject();
    ReportCreationDialog general(project());
    general.show();
    select(general, ReportKind::aer);
    general.accept();
    QCoreApplication::processEvents();
    if (!directory.isEmpty())
        EXPECT_TRUE(general.grab().save(directory + "/report-create-objects-default.png"));
    for (const int width : {840, 360}) {
        general.resize(width, 540);
        QCoreApplication::processEvents();
        EXPECT_TRUE(general.rect().contains(
            next(general)->mapTo(&general, next(general)->rect().bottomRight())));
        EXPECT_TRUE(field(general, 0)->isVisible());
        EXPECT_TRUE(field(general, 1)->isVisible());
        if (!directory.isEmpty())
            EXPECT_TRUE(general.grab().save(directory +
                                            QString("/report-create-objects-%1.png").arg(width)));
    }
    picker(general, 1, [&](auto &modal) {
        if (!directory.isEmpty())
            EXPECT_TRUE(modal.grab().save(directory + "/report-create-object-picker.png"));
    });
    general.reject();
}
TEST_F(ReportCreation, RegionContextRequiresExplicitAssetsAndPreservesMissingBinding) {
    auto input = project();
    ReportCreationDialog dialog(input, "region", ReportKind::coverage_grid);
    EXPECT_TRUE(next(dialog)->isEnabled());
    dialog.accept();
    ASSERT_TRUE(field(dialog, 0) && field(dialog, 1));
    EXPECT_EQ(field(dialog, 0)->id(), "region");
    EXPECT_FALSE(next(dialog)->isEnabled());
    EXPECT_TRUE(field(dialog, 1)->selection().ids.empty());
    field(dialog, 1)->set_selection({{"alpha", "sensor"}, false});
    EXPECT_TRUE(next(dialog)->isEnabled());
    input.regions.front().display_name = "改名区域";
    dialog.update_project(input);
    EXPECT_EQ(field(dialog, 0)->id(), "region");
    EXPECT_TRUE(next(dialog)->isEnabled());
    input.regions.clear();
    dialog.update_project(input);
    EXPECT_FALSE(next(dialog)->isEnabled());
    EXPECT_EQ(field(dialog, 0)->id(), "region");
    input = project();
    dialog.update_project(input);
    dialog.accept();
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
    EXPECT_EQ(dialog.definition().query.region_id, "region");
    EXPECT_EQ(dialog.definition().query.object_ids, (std::vector<std::string>{"alpha", "sensor"}));
    for (auto &satellite : input.satellites) {
        satellite.orbit.first = input.first;
        satellite.orbit.last = input.last;
        satellite.orbit.initial.epoch = input.first;
    }
    ReportDialog report(
        input, runtime::make_builtin_time_provider(),
        [](const ReportQuery &, ReportDialog::Ready) {}, [](auto) {}, [](auto) {});
    ASSERT_TRUE(report.configure_report(dialog.definition()));
    EXPECT_TRUE(report.windowTitle().contains(QStringLiteral("研究区")));
    input.regions.front().display_name = "重新命名的区域";
    report.update_project(input, runtime::make_builtin_time_provider());
    EXPECT_TRUE(report.windowTitle().contains(QStringLiteral("重新命名的区域")));
    ReportCreationDialog from_asset(input, "alpha", ReportKind::coverage_time);
    from_asset.accept();
    EXPECT_EQ(field(from_asset, 1)->selection().ids, (std::vector<std::string>{"alpha"}));
    EXPECT_TRUE(field(from_asset, 0)->id().empty());
    EXPECT_FALSE(next(from_asset)->isEnabled());
}
} // namespace
} // namespace orbelle::desktop
