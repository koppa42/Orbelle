#pragma once
#include <QDialog>
#include <functional>
#include <orbitforge/application/project.hpp>
class QLineEdit;
class QLabel;
class QButtonGroup;
class QPushButton;
namespace orbitforge::desktop {
class QuantityEditor;
class SensorAnglePreview;
class SensorParentLabel;
class ObjectSelectionField;
class SensorEditor final : public QDialog {
  public:
    explicit SensorEditor(QWidget *parent = nullptr);
    void set_sensor(application::ProjectSensor sensor, const application::Project &project,
                    bool creating);
    [[nodiscard]] application::ProjectSensor sensor() const;
    [[nodiscard]] bool has_unapplied_changes() const;
    [[nodiscard]] const std::string &bound_object_id() const { return original_.object_id; }
    void accept_sensor(const application::ProjectSensor &sensor);
    void update_project(const application::Project &project);
    void sync_name(const std::string &name);
    [[nodiscard]] const std::string &bound_parent_id() const {
        return original_.parent_satellite_id;
    }
    std::function<void(application::ProjectSensor)> apply;

  private:
    void update_pointing();
    void update_preview();
    void show_error(const QString &text);
    application::ProjectSensor original_;
    QLineEdit *name_;
    QButtonGroup *pointing_;
    ObjectSelectionField *target_;
    QuantityEditor *angle_;
    SensorParentLabel *parent_;
    SensorAnglePreview *preview_;
    QLabel *title_, *full_angle_, *pointing_hint_, *target_hint_, *error_;
    QWidget *target_row_;
    QPushButton *apply_, *close_;
};
} // namespace orbitforge::desktop
