#pragma once
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
                    QString label, std::string object = {});
    void set_field(std::size_t column, QString label);
    void set_object(std::string id); // Empty = all report objects.
    void reset_view();
    [[nodiscard]] QImage image(qreal scale = 2) const;
    [[nodiscard]] std::pair<double, double> visible_interval() const { return {left_, right_}; }
    [[nodiscard]] std::size_t series_count() const { return series_.size(); }
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

  private:
    struct Point {
        double x, y;
        std::size_t row;
        bool gap;
    };
    struct Series {
        QString label;
        QColor color;
        std::vector<Point> points;
        QPainterPath path;
    };
    std::shared_ptr<const runtime::ReportResult> result_;
    std::size_t column_ = 0, time_column_ = 0;
    QString label_;
    std::string object_;
    std::vector<Series> series_;
    QRectF plot_;
    double full_left_ = 0, full_right_ = 1, left_ = 0, right_ = 1, low_ = 0, high_ = 1,
           y_step_ = .25;
    QPointF cursor_, drag_start_;
    bool hover_ = false, dragging_ = false;
    void rebuild();
    void layout_plot();
    void set_interval(double left, double right);
    void draw(QPainter &painter, bool interaction) const;
    [[nodiscard]] QPointF pixel(const Point &point) const;
    [[nodiscard]] double time_at(double x) const;
    [[nodiscard]] const Point *nearest(QPointF position, const Series **series = nullptr) const;
};
} // namespace orbitforge::desktop
