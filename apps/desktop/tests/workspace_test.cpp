#include "../src/viewer/workspace.hpp"
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QTabBar>
#include <QVBoxLayout>
#include <gtest/gtest.h>

namespace orbitforge::desktop {
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
    Workspace workspace(new QWidget);
    workspace.add_view("3D", {});
    workspace.add_view("2D", {});
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
    workspace.detach_document(document);
    EXPECT_TRUE(workspace.is_floating(document));
    EXPECT_TRUE(field->isVisible());
    field->setText("changed in window");
    workspace.dock_document(document);
    EXPECT_FALSE(workspace.is_floating(document));
    EXPECT_EQ(workspace.current_document(), document);
    EXPECT_EQ(field->text(), "changed in window");
    document->setWindowTitle("Satellite1 · Properties");
    EXPECT_EQ(workspace.findChild<QTabBar *>()->tabText(2), "Satellite1 · Properties");
    document->close();
    EXPECT_FALSE(workspace.contains(document));
    EXPECT_EQ(workspace.findChild<QTabBar *>()->currentIndex(), 1);
    workspace.open(document);
    EXPECT_EQ(field->text(), "changed in window");
    EXPECT_EQ(workspace.findChild<QTabBar *>()->count(), 3);
}
TEST_F(WorkspaceTest, MultiplePanelsAndFloatingWindowCloseStayIndependent) {
    Workspace workspace(new QWidget);
    workspace.add_view("3D", {});
    auto *one = new QDialog;
    auto *two = new QDialog;
    one->setWindowTitle("Scenario");
    two->setWindowTitle("Satellite");
    workspace.show();
    workspace.open(one);
    workspace.open(two);
    workspace.detach_document(two);
    workspace.open(one);
    EXPECT_EQ(workspace.current_document(), one);
    two->window()->close();
    EXPECT_FALSE(workspace.contains(two));
    EXPECT_TRUE(workspace.contains(one));
    workspace.open(two);
    EXPECT_FALSE(workspace.is_floating(two));
    EXPECT_EQ(workspace.current_document(), two);
}
} // namespace orbitforge::desktop
