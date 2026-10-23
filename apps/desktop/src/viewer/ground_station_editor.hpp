#pragma once
#include <QDialog>
#include <functional>
#include <orbelle/application/project.hpp>

class QLineEdit;
class QLabel;
class QPushButton;
class QComboBox;
class QPlainTextEdit;
class QStackedWidget;
namespace orbelle::desktop {
class FormGroup;
class QuantityEditor;
class GroundStationEditor final : public QDialog {
  public:
    explicit GroundStationEditor(QWidget *parent = nullptr);
    void set_station(const application::ProjectGroundStation &station, bool creating);
    void set_busy(bool busy);
    void show_result(const QString &text);
    [[nodiscard]] application::ProjectGroundStation station() const;
    [[nodiscard]] bool has_unapplied_changes() const;
    [[nodiscard]] const std::string &bound_object_id() const { return original_.object_id; }
    void accept_station(const application::ProjectGroundStation &station);
    void sync_name(const std::string &name);
    std::function<void(application::ProjectGroundStation)> apply;

  private:
    void preview();
    void switch_position_type(int index);
    void load_coordinates(core::GroundStationLocation location);
    [[nodiscard]] core::GroundStationLocation location() const;
    application::ProjectGroundStation original_;
    core::GroundStationLocation canonical_;
    core::GroundPositionType position_type_ = core::GroundPositionType::geodetic;
    std::array<double, 3> represented_{};
    std::array<std::array<QuantityEditor *, 3>, 5> coordinates_{};
    bool creating_ = false;
    QLineEdit *name_;
    QComboBox *position_type_box_;
    QStackedWidget *coordinate_pages_;
    QPlainTextEdit *description_;
    QuantityEditor *above_ground_, *minimum_elevation_;
    FormGroup *preview_group_;
    QLabel *preview_, *result_;
    QWidget *fields_;
    QPushButton *apply_, *reset_;
};
} // namespace orbelle::desktop
