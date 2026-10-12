#pragma once
#include <QDialog>
#include <functional>
#include <orbitforge/application/project.hpp>
class QLineEdit;
class QComboBox;
class QLabel;
class QButtonGroup;
class QPushButton;
namespace orbitforge::desktop {
class QuantityEditor;
class SensorAnglePreview;
class SensorParentLabel;
class SensorEditor final : public QDialog {
  public:
    explicit SensorEditor(QWidget *parent = nullptr);
    void set_sensor(application::ProjectSensor sensor, const application::Project &project,
                    bool creating);
    [[nodiscard]] application::ProjectSensor sensor() const;
    std::function<void(application::ProjectSensor)> apply;

  private:
    void update_pointing();
    void update_preview();
    void show_error(const QString &text);
    application::ProjectSensor original_;
    QLineEdit *name_;
    QButtonGroup *pointing_;
    QComboBox *target_;
    QuantityEditor *angle_;
    SensorParentLabel *parent_;
    SensorAnglePreview *preview_;
    QLabel *title_, *full_angle_, *pointing_hint_, *target_hint_, *error_;
    QWidget *target_row_;
    QPushButton *apply_, *close_;
};
} // namespace orbitforge::desktop
