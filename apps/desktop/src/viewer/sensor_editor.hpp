#pragma once
#include <QDialog>
#include <functional>
#include <orbitforge/application/project.hpp>
class QLineEdit;
class QComboBox;
class QLabel;
namespace orbitforge::desktop {
class QuantityEditor;
class SensorEditor final : public QDialog {
  public:
    explicit SensorEditor(QWidget *parent = nullptr);
    void set_sensor(application::ProjectSensor sensor, const application::Project &project,
                    bool creating);
    [[nodiscard]] application::ProjectSensor sensor() const;
    std::function<void(application::ProjectSensor)> apply;

  private:
    application::ProjectSensor original_;
    QLineEdit *name_;
    QComboBox *pointing_, *target_;
    QuantityEditor *angle_;
    QLabel *parent_, *error_;
};
} // namespace orbitforge::desktop
