#pragma once
#include "report_curve_field.hpp"
#include <QImage>
#include <QPainterPath>
#include <QWidget>
#include <functional>
#include <orbitforge/runtime/report.hpp>

namespace orbitforge::desktop {
// A view of one immutable report. Interaction never propagates or resamples an orbit.
class ReportPlot final : public QWidget {
  public:
    explicit ReportPlot(QWidget *parent = nullptr);
    void set_result(std::shared_ptr<const runtime::ReportResult> result, std::size_t column,
                    QString label, std::string object = {}, bool preserve_view = false);
    void set_field(std::size_t column, QString label);
    void set_result(std::shared_ptr<const runtime::ReportResult> result,
                    std::vector<ReportCurveField> fields, std::string object = {},
                    bool preserve_view = false);
    void set_fields(std::vector<ReportCurveField> fields);
    void set_object(std::string id); // Empty = all report objects.
    void reset_view();
    void set_pan_mode(bool enabled);
    void zoom(double factor);
    [[nodiscard]] QImage image(qreal scale = 2) const;
    [[nodiscard]] std::pair<double, double> visible_interval() const { return {left_, right_}; }
    [[nodiscard]] std::size_t series_count() const { return series_.size(); }
    [[nodiscard]] std::size_t panel_count() const { return panels_.size(); }
    [[nodiscard]] const std::vector<ReportCurveField> &fields() const { return fields_; }
    std::function<void(core::Epoch)> seek;

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void leaveEvent(QEvent *) override;
    void changeEvent(QEvent *) override;

  private:
    struct Point {
        double x, y;
        std::size_t row;
        bool gap;
    };
    struct Series {
        QString label;
        QColor color;
        Qt::PenStyle style;
        std::size_t column, panel;
        std::vector<Point> points;
        QPainterPath path;
        std::vector<std::pair<double, double>> extents;
    };
    static constexpr std::size_t extent_block_size = 128;
    struct Panel {
        QString unit, title;
        std::vector<std::size_t> series;
        QRectF rect;
        double low = 0, high = 1, step = .25;
        bool fraction = false;
    };
    std::shared_ptr<const runtime::ReportResult> result_;
    std::size_t time_column_ = 0;
    std::vector<ReportCurveField> fields_;
    std::vector<Panel> panels_;
    std::string object_;
    std::vector<Series> series_;
    QImage plot_image_;
    QRectF plot_;
    double full_left_ = 0, full_right_ = 1, left_ = 0, right_ = 1;
    QPointF cursor_, drag_start_;
    double drag_left_ = 0, drag_right_ = 1, drag_width_ = 1;
    bool hover_ = false, dragging_ = false, pan_mode_ = false, panning_ = false;
    Qt::MouseButton drag_button_ = Qt::NoButton;
    void rebuild(bool preserve_view = false);
    void layout_plot();
    void set_interval(double left, double right);
    void draw(QPainter &painter) const;
    void draw_interaction(QPainter &painter) const;
    [[nodiscard]] QPointF pixel(const Point &point, const Series &series) const;
    [[nodiscard]] bool inside_plot(QPointF point) const;
    [[nodiscard]] double time_at(double x) const;
    [[nodiscard]] const Point *nearest(QPointF position, const Series **series = nullptr) const;
};
} // namespace orbitforge::desktop
