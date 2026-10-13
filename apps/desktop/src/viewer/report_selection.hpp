#pragma once
#include <QDialog>
#include <QString>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSplitter;
class QTabBar;

namespace orbitforge::desktop {
struct ReportChoice {
    QString key, name, kind, description, scope;
    bool saved = false;
    QString unavailable_reason;
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
    QLabel *title_, *category_, *description_, *scope_, *warning_, *count_, *empty_;
    QPushButton *confirm_;
};
} // namespace orbitforge::desktop
