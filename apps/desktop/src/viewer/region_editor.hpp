#pragma once
#include <QDialog>
#include <array>
#include <functional>
#include <orbelle/application/project.hpp>
class QLineEdit;
class QPlainTextEdit;
class QComboBox;
class QTableWidget;
class QStackedWidget;
class QLabel;
class QPushButton;
class QCheckBox;
class QDoubleSpinBox;
class QTimer;
namespace orbelle::desktop {
class QuantityEditor;
class RegionBoundaryPreview;
class RegionEditor final : public QDialog {
  public:
    explicit RegionEditor(QWidget *parent = nullptr);
    void set_region(const application::ProjectRegion &, bool creating);
    [[nodiscard]] application::ProjectRegion region() const;
    [[nodiscard]] bool has_unapplied_changes() const;
    [[nodiscard]] const std::string &bound_object_id() const { return original_.object_id; }
    void accept_region(const application::ProjectRegion &);
    void sync_name(const std::string &);
    void set_busy(bool);
    void show_result(const QString &);
    std::function<void(application::ProjectRegion)> apply;

  private:
    void append_vertex(core::RegionVertex);
    void update_color();
    [[nodiscard]] core::RegionBoundary boundary() const;
    void request_preview();
    void update_preview();
    application::ProjectRegion original_;
    bool creating_ = false;
    QLineEdit *name_;
    QPlainTextEdit *description_;
    QComboBox *shape_;
    std::array<QuantityEditor *, 4> bounds_;
    QuantityEditor *grid_spacing_;
    QTableWidget *vertices_;
    QWidget *fields_;
    QLabel *result_;
    QPushButton *apply_, *reset_;
    QCheckBox *show_2d_, *show_3d_, *fill_;
    QPushButton *color_;
    QDoubleSpinBox *opacity_;
    std::uint32_t color_value_ = 0;
    double loaded_opacity_percent_ = 0;
    RegionBoundaryPreview *preview_ = nullptr;
    QTimer *preview_timer_ = nullptr;
};
} // namespace orbelle::desktop
