#pragma once
#include <QWidget>
#include <orbitforge/application/hpop_covariance.hpp>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QTableWidget;
namespace orbitforge::desktop {
class HpopCovarianceEditor final : public QWidget {
  public:
    explicit HpopCovarianceEditor(const application::HpopCovarianceSettings &settings,
                                  bool supported, QWidget *parent = nullptr);
    [[nodiscard]] application::HpopCovarianceSettings value() const;
    [[nodiscard]] QString summary() const;

  private:
    void refresh();
    QCheckBox *enabled_, *independent_, *consider_, *cross_enabled_;
    QComboBox *frame_, *velocity_;
    QSpinBox *degree_, *order_;
    QLineEdit *tolerance_;
    QWidget *fields_;
    QTableWidget *state_, *parameters_, *cross_;
};
} // namespace orbitforge::desktop
