#pragma once
#include <QDialog>
#include <functional>
#include <orbitforge/application/project.hpp>

class QLineEdit;
class QLabel;
class QPushButton;
namespace orbitforge::desktop {
class QuantityEditor;
class GroundStationEditor final : public QDialog {
  public:
    explicit GroundStationEditor(QWidget *parent = nullptr);
    void set_station(const application::ProjectGroundStation &station, bool creating);
    void set_busy(bool busy);
    void show_result(const QString &text);
    [[nodiscard]] application::ProjectGroundStation station() const;
    std::function<void(application::ProjectGroundStation)> apply;

  private:
    void preview();
    application::ProjectGroundStation original_;
    QLineEdit *name_;
    QuantityEditor *longitude_, *latitude_, *height_;
    QLabel *preview_, *result_;
    QWidget *fields_;
    QPushButton *apply_, *reset_;
};
} // namespace orbitforge::desktop
