#pragma once
#include "display_settings.hpp"
#include <QPen>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QToolButton;
namespace orbitforge::desktop {
inline QPen track_pen(const QColor &color, double width, application::LineStyle style) {
    QPen pen(color, width, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin);
    switch (style) {
    case application::LineStyle::dashed:
        pen.setDashPattern({4, 2});
        break;
    case application::LineStyle::dotted:
        pen.setDashPattern({1, 2});
        break;
    case application::LineStyle::dash_dot:
        pen.setDashPattern({4, 2, 1, 2});
        break;
    default:
        break;
    }
    return pen;
}
class TrackStyleEditor final : public QWidget {
  public:
    explicit TrackStyleEditor(QWidget *parent = nullptr);
    void set_settings(const DisplaySettings &settings);
    void write_settings(DisplaySettings &settings) const;

  private:
    void set_color(QColor color);
    void refresh_preview();
    QColor color_;
    QComboBox *style_ = nullptr;
    QDoubleSpinBox *width_ = nullptr;
    QPushButton *custom_ = nullptr;
    QWidget *preview_ = nullptr;
    std::vector<QToolButton *> swatches_;
};
} // namespace orbitforge::desktop
