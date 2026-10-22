#pragma once
#include "report_curve_field.hpp"
#include "workbench_style.hpp"
#include <QDialog>
#include <QList>
#include <QString>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSplitter;
class QTabBar;
class QTreeWidget;

namespace orbelle::desktop {
struct ReportChoice {
    QString key, name, kind, description, scope;
    bool saved = false;
    QString unavailable_reason;
    QString output, note;
    WorkbenchIcon icon = WorkbenchIcon::properties;
};

// The choice remains a draft until accepted. Keys survive renames, reordering and deletion.
class ReportSelectionDialog final : public QDialog {
  public:
    ReportSelectionDialog(std::vector<ReportChoice> choices, QString selected,
                          QWidget *parent = nullptr);
    void set_choices(std::vector<ReportChoice> choices);
    [[nodiscard]] QString selection() const;
    void accept() override;

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    void filter();
    void preview();
    std::vector<ReportChoice> choices_;
    QLineEdit *search_;
    QTabBar *source_;
    QListWidget *list_;
    QSplitter *split_ = nullptr;
    QScrollArea *preview_scroll_;
    QList<int> wide_sizes_, narrow_sizes_;
    QLabel *title_, *category_, *description_, *scope_, *warning_, *count_, *empty_;
    QLabel *symbol_, *output_, *note_, *output_heading_, *scope_heading_;
    QPushButton *confirm_;
};

class ReportCurveSelectionDialog final : public QDialog {
  public:
    ReportCurveSelectionDialog(const std::vector<ReportCurveField> &fields,
                               const QStringList &selected, QWidget *parent = nullptr);
    [[nodiscard]] QStringList selection() const;
    void accept() override;

  private:
    void update_selection();
    void filter();
    QTreeWidget *list_;
    QLineEdit *search_;
    QLabel *count_;
    QPushButton *confirm_;
};

struct ReportColumnChoice {
    QString id, title, unit;
};
class ReportColumnSelectionDialog final : public QDialog {
  public:
    ReportColumnSelectionDialog(std::vector<ReportColumnChoice> fields, QStringList selected,
                                QStringList defaults, QWidget *parent = nullptr);
    [[nodiscard]] QStringList selection() const;
    void accept() override;

  private:
    void populate(const QStringList &selected);
    void filter();
    void update_controls();
    void move_current(int offset);
    std::vector<ReportColumnChoice> fields_;
    QStringList defaults_;
    QTreeWidget *list_;
    QLineEdit *search_;
    QLabel *count_;
    QPushButton *confirm_, *up_, *down_;
};
} // namespace orbelle::desktop
