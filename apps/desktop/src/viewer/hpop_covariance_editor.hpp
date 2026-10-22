#pragma once
#include <QWidget>
#include <orbelle/application/hpop_covariance.hpp>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QTableWidget;
namespace orbelle::desktop {
class HpopCovarianceEditor final : public QWidget {
  public:
    explicit HpopCovarianceEditor(const application::HpopCovarianceSettings &settings,
                                  bool supported, QWidget *parent = nullptr);
    [[nodiscard]] application::HpopCovarianceSettings value() const;
    [[nodiscard]] QString summary() const;

  private:
    void refresh();
    application::HpopCovarianceSettings initial_;
    QCheckBox *enabled_, *independent_, *consider_, *cross_enabled_;
    QComboBox *frame_, *velocity_;
    QSpinBox *degree_, *order_;
    QLineEdit *tolerance_;
    QWidget *fields_;
    QTableWidget *state_, *parameters_, *cross_;
};
} // namespace orbelle::desktop
