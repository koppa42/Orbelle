#pragma once
#include <QDialog>
#include <functional>
#include <orbelle/application/project.hpp>
class QLineEdit;
class QLabel;
class QButtonGroup;
class QPushButton;
class QComboBox;
class QTableWidget;
class QCheckBox;
class QSpinBox;
namespace orbelle::desktop {
class QuantityEditor;
class SensorAnglePreview;
class SensorOrientationPreview;
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
    void update_color();
    void load_fields();
    void update_type();
    [[nodiscard]] core::SensorPattern pattern() const;
    void show_error(const QString &text);
    application::ProjectSensor original_;
    QLineEdit *name_;
    QButtonGroup *pointing_;
    ObjectSelectionField *target_;
    QuantityEditor *angle_;
    QuantityEditor *inner_, *clock_min_, *clock_max_, *horizontal_, *vertical_, *roll_, *range_;
    QuantityEditor *tilt_x_, *tilt_y_;
    QComboBox *type_;
    QTableWidget *points_;
    QWidget *cone_fields_, *inner_fields_, *complex_fields_, *rectangle_fields_, *custom_fields_;
    QCheckBox *show_2d_, *show_3d_, *fill_;
    QSpinBox *opacity_;
    QPushButton *color_;
    std::uint32_t color_value_ = 0xf1a6ca;
    SensorParentLabel *parent_;
    SensorAnglePreview *preview_;
    SensorOrientationPreview *orientation_preview_ = nullptr;
    QLabel *title_, *angle_label_, *full_angle_, *pointing_hint_, *target_hint_, *error_;
    QWidget *target_row_;
    QPushButton *apply_, *close_;
};
} // namespace orbelle::desktop
