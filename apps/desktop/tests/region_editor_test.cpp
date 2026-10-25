#include "../src/viewer/region_editor.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "support/property_editor_fixture.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabBar>
#include <QTableWidget>
#include <QTreeWidget>
#include <numbers>

namespace orbelle::desktop {
TEST_F(QuantityTest, RegionDraftUnitsCloseReopenAndApply) {
    RegionEditor editor;
    application::ProjectRegion region;
    region.display_name = "研究区";
    editor.set_region(region, false);
    EXPECT_FALSE(editor.has_unapplied_changes());
    auto *south = editor.findChild<QuantityEditor *>("regionSouth");
    ASSERT_TRUE(south);
    choose(*south, Unit::radian);
    EXPECT_FALSE(editor.has_unapplied_changes());
    south->set_value(.1, Unit::radian);
    auto *spacing = editor.findChild<QuantityEditor *>("regionGridSpacing");
    ASSERT_TRUE(spacing);
    choose(*spacing, Unit::radian);
    spacing->set_value(.125, Unit::radian);
    editor.findChild<QLineEdit *>("regionName")->setText("新区域");
    int applied = 0;
    editor.apply = [&](auto value) {
        region = value;
        ++applied;
        editor.accept_region(value);
    };
    editor.show();
    editor.close();
    editor.show();
    EXPECT_EQ(applied, 0);
    EXPECT_TRUE(editor.has_unapplied_changes());
    editor.findChild<QPushButton *>("applyRegion")->click();
    EXPECT_EQ(applied, 1);
    EXPECT_EQ(region.name(), "新区域");
    EXPECT_DOUBLE_EQ(region.boundary.south_rad, .1);
    EXPECT_DOUBLE_EQ(region.grid_resolution_rad, .125);
    EXPECT_FALSE(editor.has_unapplied_changes());
    south->set_value(89, Unit::degree);
    editor.findChild<QPushButton *>("applyRegion")->click();
    EXPECT_EQ(applied, 1);
    EXPECT_FALSE(editor.findChild<QLabel *>("regionResult")->text().isEmpty());
    editor.findChild<QPushButton *>("resetRegion")->click();
    EXPECT_FALSE(editor.has_unapplied_changes());
    spacing->set_value(0, Unit::degree);
    editor.findChild<QPushButton *>("applyRegion")->click();
    EXPECT_EQ(applied, 1);
    EXPECT_DOUBLE_EQ(region.grid_resolution_rad, .125);
}

TEST_F(QuantityTest, RegionPolygonValidationAndNarrowLayoutRetainVertices) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    RegionEditor editor;
    application::ProjectRegion region;
    region.display_name = "跨日期变更线的多边形";
    region.boundary.vertices = {{3.0, -.1}, {-3.0, -.1}, {3.1, .2}};
    editor.set_region(region, false);
    auto *table = editor.findChild<QTableWidget *>("regionVertices");
    ASSERT_EQ(table->rowCount(), 3);
    int applied = 0;
    editor.apply = [&](auto) { ++applied; };
    for (const auto size : {QSize(900, 700), QSize(360, 600), QSize(320, 360), QSize(900, 700)}) {
        editor.resize(size);
        editor.show();
        for (int i = 0; i < 12; ++i)
            QApplication::processEvents();
        EXPECT_EQ(editor.region().boundary, region.boundary);
        auto *scroll = editor.findChild<QScrollArea *>("regionBoundaryScroll");
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        EXPECT_TRUE(editor.findChild<QPushButton *>("applyRegion")->isVisible());
        const auto directory = qEnvironmentVariable("ORBELLE_REGION_UI_CAPTURE");
        if (!directory.isEmpty()) {
            QDir().mkpath(directory);
            EXPECT_TRUE(editor.grab().save(directory +
                                           QStringLiteral("/polygon-%1.png").arg(size.width())));
        }
    }
    static_cast<QuantityEditor *>(table->cellWidget(1, 0))->set_value(3.0, Unit::radian);
    editor.findChild<QPushButton *>("applyRegion")->click();
    EXPECT_EQ(applied, 0); // Duplicate vertices never reach the live project.
    EXPECT_THROW((void)editor.region(), std::invalid_argument);
    editor.findChild<QPushButton *>("resetRegion")->click();
    EXPECT_FALSE(editor.has_unapplied_changes());
    editor.findChild<QComboBox *>("regionShape")->setCurrentIndex(0);
    editor.findChild<QPushButton *>("regionGlobal")->click();
    EXPECT_TRUE(editor.region().boundary.vertices.empty());
    EXPECT_NEAR(editor.region().boundary.north_rad, std::numbers::pi / 2, 1e-15);
}
TEST_F(QuantityTest, RegionPropertiesMatchOtherObjectsAndPreserveAllPageDrafts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    RegionEditor editor;
    application::ProjectRegion region;
    region.display_name = "区域 1";
    region.display.opacity = .123456789;
    editor.set_region(region, true);
    EXPECT_FALSE(editor.has_unapplied_changes());
    auto *selector = editor.findChild<QComboBox *>("propertyPageSelector");
    auto *tabs = editor.findChild<QTabBar *>("propertyPageTabs");
    auto *tree = editor.findChild<QTreeWidget *>("propertyNavigation");
    ASSERT_TRUE(selector && tabs && tree);
    auto *name = editor.findChild<QLineEdit *>("regionName");
    auto *description = editor.findChild<QPlainTextEdit *>("regionDescription");
    auto *spacing = editor.findChild<QuantityEditor *>("regionGridSpacing");
    auto *opacity = editor.findChild<QDoubleSpinBox *>("regionOpacity");
    name->setText("未应用名称");
    description->setPlainText("未应用的区域说明");
    spacing->set_unit(Unit::radian);
    spacing->set_value(.125, Unit::radian);
    const auto settle = [] {
        for (int i = 0; i < 12; ++i)
            QCoreApplication::processEvents();
    };
    editor.show();
    const auto directory = qEnvironmentVariable("ORBELLE_REGION_UI_CAPTURE");
    if (!directory.isEmpty())
        ASSERT_TRUE(QDir().mkpath(directory));
    for (auto size :
         {QSize(1920, 900), QSize(960, 640), QSize(560, 600), QSize(320, 360), QSize(900, 640)}) {
        editor.resize(size);
        settle();
        // Native Windows can cap the height of a high-DPI window to the screen;
        // preserve the requested width and check footer reachability below.
        EXPECT_EQ(editor.width(), size.width());
        EXPECT_EQ(tree->isVisible(), size.width() >= 680);
        EXPECT_EQ(tabs->isVisible(), size.width() >= 440 && size.width() < 680);
        EXPECT_EQ(selector->isVisible(), size.width() < 440);
        for (int page = 0; page < 4; ++page) {
            selector->setCurrentIndex(page);
            settle();
            EXPECT_EQ(tree->currentItem()->data(0, Qt::UserRole).toInt(), page);
            EXPECT_LE(name->width(), 276);
            for (auto *scroll : editor.findChildren<QScrollArea *>())
                if (scroll->isVisible()) {
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                    EXPECT_LE(scroll->widget()->width(),
                              scroll->objectName() == "regionBoundaryScroll" ? 960 : 640);
                    EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width());
                }
            auto *apply = editor.findChild<QPushButton *>("applyRegion");
            EXPECT_TRUE(editor.rect().contains(apply->mapTo(&editor, apply->rect().bottomRight())));
            if (!directory.isEmpty())
                EXPECT_TRUE(editor.grab().save(directory + QString("/region-%1x%2-page%3.png")
                                                               .arg(size.width())
                                                               .arg(size.height())
                                                               .arg(page)));
        }
        EXPECT_EQ(name->text(), "未应用名称");
        EXPECT_EQ(description->toPlainText(), "未应用的区域说明");
        EXPECT_EQ(spacing->unit(), Unit::radian);
        EXPECT_DOUBLE_EQ(editor.region().display.opacity, .123456789);
    }
    auto *fill = editor.findChild<QCheckBox *>("regionFill");
    fill->setChecked(false);
    EXPECT_FALSE(opacity->isEnabled());
    opacity->setValue(25);
    editor.findChild<QCheckBox *>("regionShow3d")->setChecked(false);
    auto changed = editor.region();
    EXPECT_FALSE(changed.display.fill);
    EXPECT_FALSE(changed.display.show_3d);
    EXPECT_DOUBLE_EQ(changed.display.opacity, .25);
    editor.accept_region(changed);
    EXPECT_FALSE(editor.has_unapplied_changes());
    opacity->setValue(12.35);
    EXPECT_NEAR(editor.region().display.opacity, .1235, 1e-15);
    editor.findChild<QPushButton *>("resetRegion")->click();
    EXPECT_FALSE(editor.has_unapplied_changes());
    if (!directory.isEmpty()) {
        editor.set_region(region, false);
        selector->setCurrentIndex(0);
        editor.resize(900, 640);
        settle();
        EXPECT_TRUE(editor.grab().save(directory + "/region-boundary.png"));
        constexpr double degree = std::numbers::pi / 180;
        region.boundary.vertices = {{90 * degree, 20 * degree},
                                    {130 * degree, 20 * degree},
                                    {130 * degree, 50 * degree},
                                    {110 * degree, 35 * degree},
                                    {90 * degree, 50 * degree}};
        editor.set_region(region, false);
        settle();
        EXPECT_TRUE(editor.grab().save(directory + "/region-polygon.png"));
    }
}
TEST_F(QuantityTest, RegionBoundaryPreviewTracksDraftsAndClearsInvalidInput) {
    RegionEditor editor;
    application::ProjectRegion region;
    region.display_name = "Preview";
    editor.set_region(region, false);
    editor.show();
    const auto settle = [] {
        for (int i = 0; i < 16; ++i)
            QCoreApplication::processEvents();
    };
    settle();
    auto *preview = editor.findChild<QWidget *>("regionBoundaryPreview");
    ASSERT_TRUE(preview);
    int applied = 0;
    editor.apply = [&](auto) { ++applied; };
    auto before = preview->grab().toImage();
    editor.findChild<QuantityEditor *>("regionWest")->set_value(170, Unit::degree);
    editor.findChild<QuantityEditor *>("regionEast")->set_value(-170, Unit::degree);
    settle();
    const auto seam = preview->grab().toImage();
    EXPECT_NE(seam, before);
    EXPECT_EQ(applied, 0);
    editor.findChild<QuantityEditor *>("regionSouth")->set_value(80, Unit::degree);
    settle();
    EXPECT_EQ(preview->accessibleDescription(), "请输入有效的边界参数以显示示意图。");
    EXPECT_NE(preview->grab().toImage(), seam);
    editor.findChild<QPushButton *>("resetRegion")->click();
    settle();
    EXPECT_EQ(preview->grab().toImage(), before);
    EXPECT_FALSE(editor.has_unapplied_changes());
    editor.findChild<QComboBox *>("regionShape")->setCurrentIndex(1);
    auto *table = editor.findChild<QTableWidget *>("regionVertices");
    static_cast<QuantityEditor *>(table->cellWidget(2, 1))->set_value(65, Unit::degree);
    settle();
    EXPECT_EQ(applied, 0);
    EXPECT_TRUE(editor.has_unapplied_changes());
    EXPECT_NE(preview->grab().toImage(), before);
    auto *boundary_group = editor.findChild<QWidget *>("regionBoundaryGroup");
    auto *preview_group = editor.findChild<QWidget *>("regionPreviewGroup");
    auto *shape = editor.findChild<QComboBox *>("regionShape");
    for (int width : {960, 320, 960}) {
        editor.resize(width, 640);
        for (int index : {0, 1}) {
            shape->setCurrentIndex(index);
            settle();
            const auto fields_position = boundary_group->mapTo(&editor, QPoint{});
            const auto preview_position = preview_group->mapTo(&editor, QPoint{});
            if (width == 960) {
                EXPECT_EQ(fields_position.y(), preview_position.y());
                EXPECT_GT(preview_position.x(), fields_position.x() + boundary_group->width());
            } else {
                EXPECT_EQ(fields_position.x(), preview_position.x());
                EXPECT_GT(preview_position.y(), fields_position.y() + boundary_group->height());
            }
        }
    }
}
} // namespace orbelle::desktop
