#pragma once
#include <QDialog>
#include <QImage>
#include <array>
#include <optional>
#include <orbelle/application/project.hpp>
#include <orbelle/core/frames/frame_provider.hpp>
#include <orbelle/core/propagation/orbit_design.hpp>
class QLabel;
class QComboBox;
class QFormLayout;
class QSpinBox;
namespace orbelle::desktop {
class QuantityEditor;
class OrbitDesignPreview;
// A single fixed TLE, with an optional name line. Strict validation is shared with the editor.
[[nodiscard]] core::Tle parse_single_tle(const QString &text);
[[nodiscard]] core::Tle read_single_tle_file(const QString &path);
class OrbitWizardDialog final : public QDialog {
  public:
    explicit OrbitWizardDialog(core::Epoch epoch, QWidget *parent = nullptr);
    void set_preview_context(QImage imagery, std::shared_ptr<const core::IFrameProvider> frames,
                             std::shared_ptr<const core::IEarthOrientationProvider> eop);
    [[nodiscard]] const std::optional<application::OrbitInput> &input() const { return input_; }

  private:
    core::EarthOrbitDesign read_design() const;
    application::OrbitInput read_input() const;
    void update_type();
    void refresh_preview();
    void render_results(const core::EarthOrbitDesign &design);
    core::Epoch epoch_;
    std::array<QuantityEditor *, 6> fields_;
    QuantityEditor *altitude_;
    QuantityEditor *perigee_height_;
    QuantityEditor *apogee_height_;
    QuantityEditor *geo_inclination_;
    QuantityEditor *sun_inclination_;
    QuantityEditor *molniya_periapsis_;
    QComboBox *type_;
    QComboBox *direction_;
    QComboBox *sun_driver_;
    QSpinBox *repeat_revolutions_;
    QSpinBox *repeat_days_;
    QFormLayout *form_;
    QLabel *description_;
    QLabel *convention_;
    OrbitDesignPreview *map_preview_;
    QLabel *preview_;
    QLabel *error_;
    std::optional<application::OrbitInput> input_;
    std::optional<core::EarthOrbitDesign> preview_design_;
    bool compact_results_ = false;
};
} // namespace orbelle::desktop
