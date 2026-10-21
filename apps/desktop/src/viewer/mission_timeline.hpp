#pragma once
#include "epoch_editor.hpp"
#include <QSlider>
#include <functional>
#include <vector>

namespace orbitforge::desktop {
// The inherited slider remains a normalized compatibility API. Mouse/keyboard seeks use
// TAI milliseconds directly, so zooming does not lose precision to its 10,000 steps.
class MissionTimeline final : public QSlider {
  public:
    explicit MissionTimeline(QWidget *parent = nullptr);
    void set_interval(qint64 first_tai_ms, qint64 last_tai_ms, const QString &scenario,
                      const QString &selected_object = {});
    void set_current_time(qint64 tai_ms, bool reveal = false);
    void set_scale(CalendarScale scale);
    void set_time_provider(std::shared_ptr<const core::ITimeProvider> time);
    void set_show_object_interval(bool show);
    void set_access_intervals(std::vector<std::pair<qint64, qint64>> intervals, QString label);
    void set_access_label(QString label);
    void reset_view();
    void zoom(double factor);
    void center_current();
    [[nodiscard]] qint64 visible_first() const { return view_first_; }
    [[nodiscard]] qint64 visible_last() const { return view_last_; }
    [[nodiscard]] qint64 current_time() const { return current_; }
    [[nodiscard]] QRectF overview_rect() const;
    [[nodiscard]] QRectF detail_rect() const;
    [[nodiscard]] std::vector<qint64> ruler_ticks() const;
    std::function<void(qint64)> seek_requested;
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;

  private:
    enum class Drag { none, seek, pan, overview, first, last };
    void request_seek(qint64 epoch);
    void drag_to(double x);
    void set_view(qint64 first, qint64 span);
    void zoom_at(double factor, double fraction);
    qint64 time_at(double x, bool overview = false) const;
    double position_at(qint64 epoch, bool overview = false) const;
    qint64 tick_step() const;
    double track_left() const;
    qint64 first_ = 0, last_ = 7'200'000, current_ = 0;
    qint64 view_first_ = 0, view_last_ = 7'200'000;
    qint64 drag_first_ = 0, drag_last_ = 0;
    double drag_x_ = 0;
    Drag drag_ = Drag::none;
    QString scenario_, object_;
    QString access_label_;
    std::vector<std::pair<qint64, qint64>> access_intervals_;
    bool show_object_interval_ = false;
    CalendarScale scale_ = CalendarScale::utc;
    std::shared_ptr<const core::ITimeProvider> time_;
};
} // namespace orbitforge::desktop
