#include "report_test_support.hpp"
#include "../../src/viewer/coverage_map.hpp"
#include "../../src/viewer/epoch_editor.hpp"
#include "../../src/viewer/object_selection.hpp"
#include "../../src/viewer/report_dialog.hpp"
#include "../../src/viewer/report_plot.hpp"
#include "../../src/viewer/report_selection.hpp"
#include "../../src/viewer/workbench_style.hpp"
#include "../../src/viewer/workspace.hpp"
#include "../window_show_observer.hpp"
#include "ui_test_support.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <numbers>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>
#include <stdexcept>

namespace orbitforge::desktop::report_test {
QApplication &test_app() {
    auto &app = test_application("report_ui_test");
    apply_workbench_style(app);
    return app;
}

int table_column(QTableView *table, const char *id) {
    for (int c = 0; c < table->model()->columnCount(); ++c)
        if (table->model()->headerData(c, Qt::Horizontal, Qt::UserRole).toString() == id)
            return c;
    throw std::runtime_error(std::string("Missing displayed report column: ") + id);
}
const results::Cell &source_cell(const runtime::ReportResult &result, std::size_t row,
                                 const char *id) {
    const auto &source = result.source_table ? *result.source_table : result.table;
    const auto found = std::ranges::find(source.columns(), id, &results::Column::id);
    if (found == source.columns().end())
        throw std::runtime_error(std::string("Missing source report column: ") + id);
    return source.rows().at(row).at(found - source.columns().begin());
}
void save_template(ReportDialog &dialog, const QString &name) {
    QTimer::singleShot(0, [name] {
        auto *naming = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(naming);
        naming->setTextValue(name);
        naming->accept();
    });
    dialog.findChild<QAction *>("saveReport")->trigger();
}
void choose_report(ReportDialog &dialog, int row) {
    QTimer::singleShot(0, [row] {
        auto *chooser = QApplication::activeModalWidget();
        ASSERT_TRUE(chooser);
        auto *list = chooser->findChild<QListWidget *>("reportSelectionList");
        ASSERT_TRUE(list);
        ASSERT_LT(row, list->count());
        list->setCurrentRow(row);
        auto *confirm = chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        EXPECT_TRUE(confirm->isEnabled());
        if (confirm->isEnabled())
            confirm->click();
        else
            qobject_cast<QDialog *>(chooser)->reject();
    });
    dialog.findChild<QPushButton *>("reportLibrary")->click();
}
bool report_available(ReportDialog &dialog, int row) {
    bool available = false;
    QTimer::singleShot(0, [&] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->findChild<QListWidget *>("reportSelectionList")->setCurrentRow(row);
        available =
            chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled();
        chooser->reject();
    });
    dialog.findChild<QPushButton *>("reportLibrary")->click();
    return available;
}
QStringList available_curves(ReportDialog &dialog) {
    QStringList ids;
    QTimer::singleShot(0, [&] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        auto *list = chooser->findChild<QTreeWidget *>("reportCurveList");
        ASSERT_TRUE(list);
        for (int i = 0; i < list->topLevelItemCount(); ++i)
            ids << list->topLevelItem(i)->data(0, Qt::UserRole).toString();
        chooser->reject();
    });
    dialog.findChild<QPushButton *>("reportPlotFields")->click();
    return ids;
}
void select_curves(ReportDialog &dialog, const QStringList &ids, bool accept, bool from_table) {
    QTimer::singleShot(0, [ids, accept] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        auto *list = chooser->findChild<QTreeWidget *>("reportCurveList");
        ASSERT_TRUE(list);
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            auto *item = list->topLevelItem(i);
            item->setCheckState(0, ids.contains(item->data(0, Qt::UserRole).toString())
                                       ? Qt::Checked
                                       : Qt::Unchecked);
        }
        if (!accept) {
            chooser->reject();
            return;
        }
        auto *confirm = chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        EXPECT_TRUE(confirm->isEnabled());
        if (confirm->isEnabled())
            confirm->click();
        else
            chooser->reject();
    });
    if (from_table)
        dialog.findChild<QAction *>("reportChooseCurves")->trigger();
    else
        dialog.findChild<QPushButton *>("reportPlotFields")->click();
}
void select_columns(ReportDialog &dialog, const QStringList &ids, bool accept) {
    auto *action = dialog.findChild<QAction *>("reportChooseColumns");
    ASSERT_TRUE(action && action->isEnabled());
    QTimer::singleShot(0, [ids, accept] {
        auto *chooser =
            dynamic_cast<ReportColumnSelectionDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        auto *list = chooser->findChild<QTreeWidget *>("reportColumnList");
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            auto *item = list->topLevelItem(i);
            item->setCheckState(0, ids.contains(item->data(0, Qt::UserRole).toString())
                                       ? Qt::Checked
                                       : Qt::Unchecked);
        }
        // Exercise an arbitrary chosen order; the chooser's Up/Down controls are
        // covered separately, including search and boundary behavior.
        for (int target = 0; target < ids.size(); ++target)
            for (int i = target; i < list->topLevelItemCount(); ++i)
                if (list->topLevelItem(i)->data(0, Qt::UserRole).toString() == ids[target]) {
                    auto *item = list->takeTopLevelItem(i);
                    list->insertTopLevelItem(target, item);
                    break;
                }
        EXPECT_EQ(chooser->selection(), ids);
        if (accept && !ids.empty())
            chooser->accept();
        else
            chooser->reject();
    });
    action->trigger();
}
void wait_for_report(ReportDialog &dialog, int timeout) {
    const auto *generate = dialog.findChild<QPushButton *>("generateReport");
    ASSERT_TRUE(wait_for_ui([&] { return generate->isEnabled(); }, timeout));
}
} // namespace orbitforge::desktop::report_test
