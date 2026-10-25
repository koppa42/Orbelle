#pragma once
#include <QImage>
#include <QWidget>
#include <functional>
#include <memory>
#include <orbelle/runtime/report.hpp>

namespace orbelle::desktop {
// Immutable scientific results; display never launches propagation or changes the grid.
class CoverageMap final : public QWidget {
  public:
    explicit CoverageMap(QWidget *parent = nullptr);
    void set_result(std::shared_ptr<const runtime::ReportResult> result,
                    bool reset_selection = true);
    [[nodiscard]] std::size_t point_index() const { return selected_; }
    void set_metric(int metric);
    void select_point(std::size_t index);
    [[nodiscard]] QImage image() const;
    std::function<void(std::size_t)> selected;

  protected:
    bool event(QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

  private:
    std::shared_ptr<const runtime::ReportResult> result_;
    std::vector<core::RegionVertex> outline_;
    QImage background_;
    int metric_ = 0;
    std::size_t selected_ = 0;
    QRectF map_rect() const;
    QRectF cell_rect(const core::CoverageCell &, qreal pixel_ratio = 0) const;
    double value(const results::CoveragePoint &) const;
    double maximum() const;
    void draw_background(QPainter &) const;
    void draw_selection(QPainter &) const;
};
} // namespace orbelle::desktop
