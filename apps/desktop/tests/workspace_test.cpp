#include "../src/viewer/workbench_style.hpp"
#include "../src/viewer/workspace.hpp"
#include <QApplication>
#include <QDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSplitter>
#include <QStyleOptionTab>
#include <QTabBar>
#include <QVBoxLayout>
#include <gtest/gtest.h>

namespace orbelle::desktop {
namespace {
QTabBar *tabs_for(Workspace &workspace, const QString &title) {
    for (auto *tabs : workspace.findChildren<QTabBar *>())
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == title)
                return tabs;
    return nullptr;
}
int index_for(QTabBar *tabs, const QString &title) {
    for (int i = 0; i < tabs->count(); ++i)
        if (tabs->tabText(i) == title)
            return i;
    return -1;
}
void drag_tab(Workspace &workspace, const QString &title, const QPoint &target,
              bool cancel = false) {
    auto *tabs = tabs_for(workspace, title);
    ASSERT_NE(tabs, nullptr);
    const auto start = tabs->tabRect(index_for(tabs, title)).center();
    QMouseEvent press(QEvent::MouseButtonPress, start, tabs->mapToGlobal(start), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, tabs->mapFromGlobal(target), target, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, tabs->mapFromGlobal(target), target,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(tabs, &press);
    QCoreApplication::sendEvent(tabs, &move);
    if (cancel) {
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(tabs, &escape);
    }
    QCoreApplication::sendEvent(tabs, &release);
    QCoreApplication::processEvents();
}
} // namespace
class WorkspaceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        if (!QApplication::instance()) {
            static int argc = 1;
            static char executable[] = "workspace_test";
            static char *argv[] = {executable, nullptr};
            static QApplication app(argc, argv);
        }
    }
};
TEST_F(WorkspaceTest, SwitchingDetachingAndClosingPreserveDocumentDraft) {
    Workspace workspace;
    workspace.add_view(new QWidget, "3D", {});
    workspace.add_view(new QWidget, "2D", {});
    auto *document = new QDialog;
    document->setWindowTitle("Satellite1");
    auto *layout = new QVBoxLayout(document);
    auto *field = new QLineEdit("draft");
    layout->addWidget(field);
    workspace.resize(800, 500);
    workspace.show();
    workspace.open(document);
    EXPECT_EQ(workspace.current_document(), document);
    EXPECT_FALSE(document->isWindow());
    workspace.select_view(1);
    EXPECT_EQ(workspace.current_document(), nullptr);
    EXPECT_EQ(field->text(), "draft");
    workspace.open(document);
    workspace.detach_page(document);
    EXPECT_TRUE(workspace.is_floating(document));
    EXPECT_TRUE(field->isVisible());
    document->window()->resize(320, 400);
    QCoreApplication::processEvents();
    EXPECT_EQ(document->window()->width(), 320);
    field->setText("changed in window");
    workspace.dock_page(document);
    EXPECT_FALSE(workspace.is_floating(document));
    EXPECT_EQ(workspace.current_document(), document);
    EXPECT_EQ(field->text(), "changed in window");
    document->setWindowTitle("Satellite1 · Properties");
    EXPECT_EQ(workspace.findChild<QTabBar *>()->tabText(2), "Satellite1 · Properties");
    EXPECT_EQ(workspace.findChild<QTabBar *>()->tabToolTip(2), "Satellite1 · Properties");
    document->close();
    EXPECT_FALSE(workspace.contains(document));
    EXPECT_EQ(workspace.findChild<QTabBar *>()->currentIndex(), 1);
    workspace.open(document);
    EXPECT_EQ(field->text(), "changed in window");
    EXPECT_EQ(workspace.findChild<QTabBar *>()->count(), 3);
}
TEST_F(WorkspaceTest, TabLabelsAndCloseControlsShareCentreline) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    Workspace workspace;
    auto *map = new QWidget;
    workspace.add_view(map, "2D", workbench_icon(WorkbenchIcon::map));
    workspace.add_view(new QWidget, "3D", workbench_icon(WorkbenchIcon::earth));
    workspace.show();
    const auto check = [&] {
        QCoreApplication::processEvents();
        for (auto *tabs : workspace.findChildren<QTabBar *>()) {
            for (int i = 0; i < tabs->count(); ++i) {
                auto *close = tabs->tabButton(i, QTabBar::RightSide);
                ASSERT_NE(close, nullptr);
                QStyleOptionTab option;
                option.initFrom(tabs);
                option.rect = tabs->tabRect(i);
                option.shape = tabs->shape();
                option.tabIndex = i;
                option.text = tabs->tabText(i);
                option.icon = tabs->tabIcon(i);
                option.iconSize = tabs->iconSize();
                option.rightButtonSize = close->size();
                option.state.setFlag(QStyle::State_Selected, i == tabs->currentIndex());
                const auto label =
                    tabs->style()->subElementRect(QStyle::SE_TabBarTabText, &option, tabs);
                EXPECT_NEAR(QRectF(label).center().y(), QRectF(close->geometry()).center().y(),
                            0.5);
                EXPECT_TRUE(tabs->rect().contains(close->geometry()));
            }
        }
    };
    for (int width : {800, 320, 600}) {
        workspace.resize(width, 260);
        for (int selected : {0, 1}) {
            workspace.select_view(selected);
            check();
        }
    }
    workspace.detach_page(map);
    check();
    workspace.dock_page(map);
    check();
}
TEST_F(WorkspaceTest, NarrowSplitRespectsActiveDocumentWithoutWideningHiddenViews) {
    Workspace workspace;
    auto *view = new QWidget;
    workspace.add_view(view, "3D", {});
    auto *document = new QDialog;
    document->setWindowTitle("Properties");
    document->setMinimumWidth(320);
    auto *layout = new QVBoxLayout(document);
    auto *draft = new QLineEdit("unapplied draft");
    layout->addWidget(draft);
    workspace.resize(1000, 560);
    workspace.show();
    workspace.open(document, false);
    workspace.split_page(document, view, Qt::RightEdge);
    workspace.resize(600, 440);
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
    EXPECT_EQ(workspace.width(), 600);
    EXPECT_GE(document->width(), 320);
    EXPECT_TRUE(document->parentWidget()->rect().contains(document->geometry()));
    EXPECT_TRUE(workspace.rect().contains(draft->mapTo(&workspace, draft->rect().bottomRight())));
    workspace.merge_page(document, view);
    workspace.select_view(0);
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
    workspace.resize(360, 440);
    QCoreApplication::processEvents();
    EXPECT_EQ(workspace.width(), 360);
    EXPECT_FALSE(document->isVisible());
    EXPECT_TRUE(view->isVisible());
    workspace.open(document, false);
    QCoreApplication::processEvents();
    EXPECT_EQ(draft->text(), "unapplied draft");
    EXPECT_TRUE(document->parentWidget()->rect().contains(document->geometry()));
}
TEST_F(WorkspaceTest, MultiplePanelsAndFloatingWindowCloseStayIndependent) {
    Workspace workspace;
    workspace.add_view(new QWidget, "3D", {});
    auto *one = new QDialog;
    auto *two = new QDialog;
    one->setWindowTitle("Scenario");
    two->setWindowTitle("Satellite");
    workspace.show();
    workspace.open(one);
    workspace.open(two);
    workspace.detach_page(two);
    workspace.open(one);
    EXPECT_EQ(workspace.current_document(), one);
    two->window()->close();
    EXPECT_FALSE(workspace.contains(two));
    EXPECT_TRUE(workspace.contains(one));
    workspace.open(two);
    EXPECT_FALSE(workspace.is_floating(two));
    EXPECT_EQ(workspace.current_document(), two);
}
TEST_F(WorkspaceTest, DragViewsToSplitMergeAndReorderWithoutReplacingContent) {
    Workspace workspace;
    auto *globe = new QLineEdit("camera draft");
    auto *map = new QLineEdit("map zoom draft");
    workspace.add_view(globe, "3D", {});
    workspace.add_view(map, "2D", {});
    workspace.resize(1000, 600);
    workspace.show();
    QCoreApplication::processEvents();
    drag_tab(workspace, "2D", workspace.mapToGlobal(QPoint{workspace.width() - 12, 300}));
    EXPECT_TRUE(globe->isVisible());
    EXPECT_TRUE(map->isVisible());
    EXPECT_LT(globe->mapTo(&workspace, QPoint{0, 0}).x(), map->mapTo(&workspace, QPoint{0, 0}).x());
    EXPECT_NE(tabs_for(workspace, "2D"), tabs_for(workspace, "3D"));
    auto *left = tabs_for(workspace, "3D");
    drag_tab(workspace, "2D", left->mapToGlobal(QPoint{left->width() - 8, 16}));
    EXPECT_EQ(tabs_for(workspace, "2D"), tabs_for(workspace, "3D"));
    EXPECT_TRUE(map->isVisible());
    EXPECT_FALSE(globe->isVisible());
    auto *tabs = tabs_for(workspace, "2D");
    drag_tab(workspace, "2D", tabs->mapToGlobal(QPoint{2, 16}));
    EXPECT_EQ(tabs->tabText(0), "2D");
    workspace.select_view(0);
    EXPECT_TRUE(globe->isVisible());
    EXPECT_FALSE(map->isVisible());
    EXPECT_EQ(globe->text(), "camera draft");
    EXPECT_EQ(map->text(), "map zoom draft");
    workspace.close_page(globe);
    EXPECT_FALSE(workspace.contains(globe));
    workspace.select_view(0);
    EXPECT_TRUE(globe->isVisible());
    EXPECT_EQ(tabs->count(), 2);
}
TEST_F(WorkspaceTest, EscapeCancelsTabDrag) {
    Workspace workspace;
    auto *globe = new QWidget;
    auto *map = new QWidget;
    workspace.add_view(globe, "3D", {});
    workspace.add_view(map, "2D", {});
    workspace.resize(1000, 600);
    workspace.show();
    QCoreApplication::processEvents();
    drag_tab(workspace, "2D", workspace.mapToGlobal(QPoint{workspace.width() + 100, 300}), true);
    EXPECT_FALSE(workspace.is_floating(map));
    EXPECT_EQ(tabs_for(workspace, "2D"), tabs_for(workspace, "3D"));
    EXPECT_EQ(tabs_for(workspace, "3D")->tabText(0), "3D");
}
TEST_F(WorkspaceTest, ViewsCanFloatCloseAndReopenWithSameState) {
    Workspace workspace;
    auto *globe = new QLineEdit("same camera");
    auto *map = new QWidget;
    workspace.add_view(globe, "3D", {});
    workspace.add_view(map, "2D", {});
    workspace.resize(1000, 600);
    workspace.show();
    QCoreApplication::processEvents();
    drag_tab(workspace, "3D", workspace.mapToGlobal(QPoint{workspace.width() + 80, 100}));
    EXPECT_TRUE(workspace.is_floating(globe));
    EXPECT_TRUE(globe->isVisible());
    EXPECT_GE(globe->window()->width(), 480);
    EXPECT_GE(globe->window()->height(), 320);
    workspace.dock_page(globe);
    EXPECT_FALSE(workspace.is_floating(globe));
    workspace.detach_page(globe);
    globe->window()->close();
    EXPECT_FALSE(workspace.contains(globe));
    workspace.select_view(0);
    EXPECT_TRUE(globe->isVisible());
    EXPECT_EQ(globe->text(), "same camera");
}
TEST_F(WorkspaceTest, PropertyDraftSurvivesNestedSplitsAndEmptyGroupsCollapse) {
    Workspace workspace;
    auto *globe = new QWidget;
    auto *map = new QWidget;
    workspace.add_view(globe, "3D", {});
    workspace.add_view(map, "2D", {});
    auto *document = new QDialog;
    document->setWindowTitle("Properties");
    auto *layout = new QVBoxLayout(document);
    auto *field = new QLineEdit("unsaved orbit");
    layout->addWidget(field);
    workspace.resize(1000, 700);
    workspace.show();
    workspace.open(document);
    workspace.select_view(0);
    workspace.split_page(map, globe, Qt::RightEdge);
    QCoreApplication::processEvents();
    const auto point = map->mapToGlobal(QPoint{map->width() / 2, map->height() - 8});
    drag_tab(workspace, "Properties", point);
    EXPECT_TRUE(globe->isVisible());
    EXPECT_TRUE(map->isVisible());
    EXPECT_TRUE(field->isVisible());
    EXPECT_GT(document->mapTo(&workspace, QPoint{0, 0}).y(),
              map->mapTo(&workspace, QPoint{0, 0}).y());
    workspace.merge_page(map, globe);
    workspace.merge_page(document, globe);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    EXPECT_EQ(tabs_for(workspace, "3D"), tabs_for(workspace, "Properties"));
    EXPECT_EQ(workspace.findChildren<QSplitter *>().size(), 1);
    EXPECT_EQ(field->text(), "unsaved orbit");
    document->close();
    workspace.open(document);
    EXPECT_EQ(field->text(), "unsaved orbit");
}
} // namespace orbelle::desktop
