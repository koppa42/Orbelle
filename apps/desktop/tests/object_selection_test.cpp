#include "../src/viewer/object_selection.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "window_show_observer.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QScrollBar>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <gtest/gtest.h>

namespace orbitforge::desktop {
namespace {
class ObjectSelectionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        static int argc = 1;
        static char executable[] = "object_selection_test";
        static char *argv[] = {executable, nullptr};
        static QApplication app(argc, argv);
        apply_workbench_style(app);
    }
    static application::Project project() {
        application::Project p("Mission", core::Epoch{0}, core::Epoch{60'000'000'000});
        application::ProjectSatellite satellite;
        satellite.orbit.object_id = "aurora";
        satellite.display_name = "晨星 / Aurora";
        p.satellites.push_back(satellite);
        satellite.orbit.object_id = "voyager";
        satellite.display_name = "远航 / Voyager";
        p.satellites.push_back(satellite);
        p.sensors.push_back({"camera-east", "观测仪", "aurora"});
        p.sensors.push_back({"camera-west", "观测仪", "voyager"});
        p.ground_stations.push_back({"station-east", "东部测控站"});
        p.ground_stations.push_back({"station-west", "西部测控站"});
        return p;
    }
    static QTreeWidget *tree(ObjectSelectionDialog &d) {
        return d.findChild<QTreeWidget *>("objectSelectionTree");
    }
    static QTreeWidgetItem *item(ObjectSelectionDialog &d, const char *id) {
        for (QTreeWidgetItemIterator it(tree(d)); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toString() == QString::fromUtf8(id))
                return *it;
        return nullptr;
    }
    static QPushButton *ok(ObjectSelectionDialog &d) {
        return d.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
    }
    static void key(QWidget &widget, int code) {
        QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
        QApplication::sendEvent(&widget, &press);
        QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier);
        QApplication::sendEvent(&widget, &release);
    }
    static void capture(QWidget &widget, const QString &name) {
        const auto directory = qEnvironmentVariable("ORBITFORGE_OBJECT_UI_CAPTURE");
        if (directory.isEmpty())
            return;
        ASSERT_TRUE(QDir{}.mkpath(directory));
        QApplication::processEvents();
        EXPECT_TRUE(widget.grab().save(directory + "/" + name + ".png"));
    }
};

TEST_F(ObjectSelectionTest, FilterKeepsHierarchyAndResolvesDuplicateNamesById) {
    ObjectSelectionDialog d(project_object_choices(project()),
                            {QStringLiteral("选择可见性分析的卫星或传感器"), false, {}},
                            {{"camera-east"}, false});
    d.show();
    QApplication::processEvents();
    ASSERT_TRUE(item(d, "camera-east"));
    ASSERT_EQ(item(d, "camera-east")->parent(), item(d, "aurora"));
    auto *search = d.findChild<QLineEdit *>("objectSelectionSearch");
    search->setText("VOYAGER");
    EXPECT_TRUE(item(d, "aurora")->isHidden());
    EXPECT_FALSE(item(d, "voyager")->isHidden());
    EXPECT_FALSE(item(d, "camera-west")->isHidden());
    // Filtering never silently rebinds a selected object hidden by that filter.
    EXPECT_EQ(d.selection().ids, std::vector<std::string>{"camera-east"});
    search->setText("camera-west");
    tree(d)->setCurrentItem(item(d, "camera-west"));
    EXPECT_EQ(d.selection().ids, std::vector<std::string>{"camera-west"});
    EXPECT_TRUE(d.findChild<QLabel *>("objectSelectionDetails")->toolTip().contains("Voyager"));
    capture(d, "search-by-id");
    search->clear();
    auto *kind = d.findChild<QComboBox *>("objectSelectionKind");
    kind->setCurrentIndex(kind->findData(static_cast<int>(ObjectKind::ground_station)));
    EXPECT_TRUE(item(d, "aurora")->isHidden());
    EXPECT_FALSE(item(d, "station-east")->isHidden());
    kind->setCurrentIndex(0);
    key(d, Qt::Key_Return);
    EXPECT_EQ(d.result(), QDialog::Accepted);

    ObjectSelectionDialog sensors(project_object_choices(project(), {ObjectKind::sensor}), {}, {});
    EXPECT_FALSE(item(sensors, "aurora")->flags().testFlag(Qt::ItemIsSelectable));
    EXPECT_EQ(item(sensors, "camera-east")->parent(), item(sensors, "aurora"));
    EXPECT_FALSE(item(sensors, "station-east"));
}

TEST_F(ObjectSelectionTest, MultiSelectionSurvivesFilteringAndAllIsExplicit) {
    ObjectSelectionDialog d(
        project_object_choices(project()),
        {QStringLiteral("选择报告适用对象"), true, QStringLiteral("全部适用对象（含以后新增）")},
        {{"aurora", "station-east"}, false});
    auto *search = d.findChild<QLineEdit *>("objectSelectionSearch");
    search->setText("west");
    item(d, "station-west")->setCheckState(0, Qt::Checked);
    EXPECT_EQ(d.selection().ids,
              (std::vector<std::string>{"aurora", "station-east", "station-west"}));
    auto *all = d.findChild<QCheckBox *>("objectSelectionAll");
    all->setChecked(true);
    EXPECT_TRUE(d.selection().all);
    EXPECT_TRUE(d.selection().ids.empty());
    EXPECT_TRUE(ok(d)->isEnabled());
    all->setChecked(false);
    EXPECT_EQ(d.selection().ids.size(), 3U);
    search->clear();
    d.show();
    capture(d, "multiple-objects");
    d.findChild<QPushButton *>("objectSelectionClear")->click();
    EXPECT_TRUE(d.selection().ids.empty());
    EXPECT_FALSE(d.selection().all);
    EXPECT_FALSE(ok(d)->isEnabled());
    key(d, Qt::Key_Return);
    EXPECT_TRUE(d.isVisible());
    EXPECT_EQ(d.result(), QDialog::Rejected);
    key(d, Qt::Key_Escape);
    EXPECT_FALSE(d.isVisible());
}

TEST_F(ObjectSelectionTest, FieldCommitsOnlyAcceptedDraftAndRefreshesOpenDialog) {
    ObjectSelectionField field;
    field.set_options({QStringLiteral("选择目标地面站"), false, {}});
    auto choices = project_object_choices(project(), {ObjectKind::ground_station});
    field.set_objects(choices);
    field.set_id("station-east");
    int changes = 0;
    QObject::connect(&field, &ObjectSelectionField::selectionChanged, &field, [&] { ++changes; });
    QTimer::singleShot(0, [&] {
        auto *d = qobject_cast<ObjectSelectionDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(d);
        tree(*d)->setCurrentItem(item(*d, "station-west"));
        key(*d, Qt::Key_Escape);
    });
    field.click();
    EXPECT_EQ(field.id(), "station-east");
    EXPECT_EQ(changes, 0);
    QTimer::singleShot(0, [&] {
        auto *d = qobject_cast<ObjectSelectionDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(d);
        // An object removed while the dialog is open invalidates its draft, without rebinding.
        choices.erase(choices.begin());
        field.set_objects(choices);
        EXPECT_FALSE(ok(*d)->isEnabled());
        EXPECT_EQ(d->selection().ids, std::vector<std::string>{"station-east"});
        tree(*d)->setCurrentItem(item(*d, "station-west"));
        ok(*d)->click();
    });
    field.click();
    EXPECT_EQ(field.id(), "station-west");
    EXPECT_EQ(changes, 1);
    // Renaming changes the label, never the selected ID or the binding notification.
    choices.front().name = QStringLiteral("新站名");
    choices.front().path = choices.front().name;
    field.set_objects(choices);
    EXPECT_EQ(field.text(), QStringLiteral("新站名"));
    EXPECT_EQ(changes, 1);
}

TEST_F(ObjectSelectionTest, MissingBindingsAndEmptyMatchesCannotBeSubmitted) {
    WindowShowObserver observer;
    ObjectSelectionDialog d({}, {QStringLiteral("选择目标地面站"), false, {}},
                            {{"removed"}, false});
    EXPECT_TRUE(observer.shown.empty());
    EXPECT_FALSE(ok(d)->isEnabled());
    EXPECT_EQ(d.selection().ids, std::vector<std::string>{"removed"});
    d.show();
    capture(d, "missing-target");
    d.set_objects(project_object_choices(project(), {ObjectKind::ground_station}));
    EXPECT_FALSE(ok(d)->isEnabled());
    d.findChild<QLineEdit *>("objectSelectionSearch")->setText("not-found");
    EXPECT_TRUE(d.findChild<QLabel *>("objectSelectionEmpty")->isVisible());
    capture(d, "no-match");

    ObjectSelectionDialog multi(project_object_choices(project()), {{}, true, {}},
                                {{"removed", "aurora"}, false});
    EXPECT_FALSE(ok(multi)->isEnabled());
    item(multi, "removed")->setCheckState(0, Qt::Unchecked);
    EXPECT_TRUE(ok(multi)->isEnabled());
    EXPECT_EQ(multi.selection().ids, std::vector<std::string>{"aurora"});
}

TEST_F(ObjectSelectionTest, TenThousandObjectsStayBoundedAndSearchable) {
    std::vector<ObjectChoice> choices;
    for (int i = 0; i < 10000; ++i)
        choices.push_back(
            {"sat-" + std::to_string(i), QStringLiteral("晨星 %1").arg(i), ObjectKind::satellite});
    ObjectSelectionDialog d(std::move(choices), {QStringLiteral("选择曲线显示对象"), false, {}},
                            {});
    d.show();
    QApplication::processEvents();
    const auto size = d.size();
    EXPECT_LE(size.height(), 540);
    EXPECT_GT(tree(d)->verticalScrollBar()->maximum(), 0);
    EXPECT_FALSE(tree(d)->horizontalScrollBar()->isVisible());
    d.findChild<QLineEdit *>("objectSelectionSearch")->setText("sat-9999");
    QApplication::processEvents();
    EXPECT_EQ(d.size(), size);
    EXPECT_FALSE(item(d, "sat-9999")->isHidden());
    EXPECT_TRUE(item(d, "sat-9998")->isHidden());
    tree(d)->setCurrentItem(item(d, "sat-9999"));
    ok(d)->click();
    EXPECT_EQ(d.result(), QDialog::Accepted);
    EXPECT_EQ(d.selection().ids, std::vector<std::string>{"sat-9999"});
}

TEST_F(ObjectSelectionTest, NarrowLayoutsElideLongNamesWithoutExpandingWindow) {
    auto p = project();
    p.satellites[0].display_name = std::string(150, 'A') + " / 晨星";
    ObjectSelectionDialog d(
        project_object_choices(p),
        {QStringLiteral("选择报告适用对象"), true, QStringLiteral("全部适用对象（含以后新增）")},
        {{"camera-east"}, false});
    d.show();
    for (const int width : {360, 600}) {
        d.resize(width, 540);
        QApplication::processEvents();
        EXPECT_EQ(d.width(), width);
        EXPECT_FALSE(tree(d)->horizontalScrollBar()->isVisible());
        const QRect button_rect(ok(d)->mapTo(&d, QPoint{}), ok(d)->size());
        EXPECT_TRUE(d.rect().contains(button_rect));
        capture(d, QStringLiteral("selector-%1").arg(width));
    }
    QWidget host;
    auto *layout = new QVBoxLayout(&host);
    auto *field = new ObjectSelectionField(&host);
    layout->addWidget(new QLabel(QStringLiteral("跟踪目标"), &host));
    layout->addWidget(field);
    field->set_objects(project_object_choices(p));
    field->set_id("camera-east");
    host.resize(240, 100);
    host.show();
    QApplication::processEvents();
    EXPECT_EQ(host.width(), 240);
    EXPECT_TRUE(field->toolTip().contains(QString(150, 'A')));
    capture(host, "compact-field");
}
} // namespace
} // namespace orbitforge::desktop
