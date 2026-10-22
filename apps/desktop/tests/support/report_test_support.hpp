#pragma once

#include "../../src/viewer/report_dialog.hpp"
#include <QStringList>
class QApplication;
class QTableView;

namespace orbelle::desktop::report_test {
QApplication &test_app();
int table_column(QTableView *table, const char *id);
const results::Cell &source_cell(const runtime::ReportResult &result, std::size_t row,
                                 const char *id);
void save_template(ReportDialog &dialog, const QString &name);
void choose_report(ReportDialog &dialog, int row);
bool report_available(ReportDialog &dialog, int row);
QStringList available_curves(ReportDialog &dialog);
void select_curves(ReportDialog &dialog, const QStringList &ids, bool accept = true,
                   bool from_table = false);
void select_columns(ReportDialog &dialog, const QStringList &ids, bool accept = true);
void wait_for_report(ReportDialog &dialog, int timeout = 5000);
} // namespace orbelle::desktop::report_test
