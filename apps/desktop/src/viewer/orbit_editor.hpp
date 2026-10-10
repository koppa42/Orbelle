#pragma once
#include "display_settings.hpp"
#include "orbit_scene.hpp"
#include <QWidget>
#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QComboBox;
class QCheckBox;
class QTreeWidget;
class QStackedWidget;
namespace orbitforge::desktop {
class EpochEditor;
class QuantityEditor;
class OrbitEditor final : public QWidget {
  public:
    explicit OrbitEditor(const OrbitInput &input, QWidget *parent = nullptr);
    std::function<void(OrbitInput)> apply;
    std::function<void()> cancel;
    std::function<void()> close_requested;
    std::function<void(int, QString, double)> appearance;
    std::function<void(DisplaySettings)> display;
    std::function<core::CartesianState(core::CartesianState, core::FrameId)> transform_state;
    void set_display(const DisplaySettings &settings);
    void set_input(const OrbitInput &input);
    void set_create_mode(bool create);
    void set_state_mode(int mode);
    void set_reference_frame(core::FrameId frame);
    void set_propagator(OrbitPropagator propagator);
    void open_propagator_settings();
    void set_appearance(int kind, const QString &path, double diameter_km);
    void submit();
    void set_busy(bool busy);
    void show_result(const QString &message, bool error = false);
    void configure_appearance(int kind, const QString &path, double diameter_km);
    void set_appearance_busy(bool busy);
    void show_appearance_result(const QString &message, bool error = false);

  private:
    void refresh_enabled();
    core::CartesianState read_state() const;
    core::CartesianState in_frame(core::CartesianState state, core::FrameId frame) const;
    void write_cartesian(const core::CartesianState &state);
    void write_elements(const core::CartesianState &state);
    void update_state_labels();
    core::OriginId body_ = core::OriginId::earth;
    core::FrameId input_frame_ = core::FrameId::gcrf;
    OrbitPropagator propagator_kind_ = OrbitPropagator::two_body;
    core::J2SecularOptions j2_options_;
    bool create_mode_ = false, orbit_busy_ = false, model_busy_ = false;
    QTreeWidget *navigation_ = nullptr;
    QLineEdit *object_ = nullptr;
    QComboBox *propagator_ = nullptr, *reference_frame_ = nullptr;
    QLabel *anomaly_label_ = nullptr, *state_title_ = nullptr;
    std::array<QuantityEditor *, 3> position_{}, velocity_{};
    EpochEditor *epoch_ = nullptr, *first_ = nullptr, *last_ = nullptr;
    QComboBox *state_mode_ = nullptr;
    QStackedWidget *state_pages_ = nullptr;
    std::array<QuantityEditor *, 6> elements_{};
    int state_mode_index_ = 0;
    QuantityEditor *step_ = nullptr;
    QPushButton *apply_ = nullptr, *cancel_ = nullptr;
    QLabel *message_ = nullptr;
    QWidget *fields_ = nullptr;
    QWidget *model_fields_ = nullptr;
    QComboBox *model_kind_ = nullptr;
    QLineEdit *model_path_ = nullptr;
    QuantityEditor *model_diameter_ = nullptr;
    QPushButton *apply_model_ = nullptr;
    QLabel *model_message_ = nullptr;
    QComboBox *track_extent_ = nullptr;
    QuantityEditor *past_ = nullptr, *future_ = nullptr;
    QCheckBox *track_2d_ = nullptr, *label_2d_ = nullptr, *track_3d_ = nullptr;
    std::vector<QPushButton *> display_buttons_;
    std::vector<QLabel *> display_messages_;
    void submit_display();
};
} // namespace orbitforge::desktop
