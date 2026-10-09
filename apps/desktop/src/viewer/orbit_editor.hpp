#pragma once
#include "orbit_scene.hpp"
#include <QWidget>
#include <functional>

class QDoubleSpinBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QComboBox;
class QListWidget;
class QStackedWidget;
namespace orbitforge::desktop {
class EpochEditor;
class OrbitEditor final : public QWidget {
  public:
    explicit OrbitEditor(const OrbitInput &input, QWidget *parent = nullptr);
    std::function<void(OrbitInput)> apply;
    std::function<void()> cancel;
    std::function<void(int, QString, double)> appearance;
    void set_input(const OrbitInput &input);
    void set_create_mode(bool create);
    void set_state_mode(int mode);
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
    void write_elements(const core::CartesianState &state);
    bool create_mode_ = false, orbit_busy_ = false, model_busy_ = false;
    QListWidget *navigation_ = nullptr;
    QLineEdit *object_ = nullptr, *mu_ = nullptr;
    std::array<QLineEdit *, 3> position_{}, velocity_{};
    EpochEditor *epoch_ = nullptr, *first_ = nullptr, *last_ = nullptr;
    QComboBox *state_mode_ = nullptr;
    QStackedWidget *state_pages_ = nullptr;
    std::array<QDoubleSpinBox *, 6> elements_{};
    int state_mode_index_ = 0;
    QSpinBox *step_ = nullptr;
    QPushButton *apply_ = nullptr, *cancel_ = nullptr;
    QLabel *message_ = nullptr;
    QWidget *fields_ = nullptr;
    QWidget *model_fields_ = nullptr;
    QComboBox *model_kind_ = nullptr;
    QLineEdit *model_path_ = nullptr;
    QDoubleSpinBox *model_diameter_ = nullptr;
    QPushButton *apply_model_ = nullptr;
    QLabel *model_message_ = nullptr;
};
} // namespace orbitforge::desktop
