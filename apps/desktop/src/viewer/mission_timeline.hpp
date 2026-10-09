#pragma once
#include "epoch_editor.hpp"
#include <QSlider>

namespace orbitforge::desktop {
// Retains QSlider signals and keyboard accessibility, with an actual time ruler.
class MissionTimeline final : public QSlider {
  public:
    explicit MissionTimeline(QWidget *parent = nullptr);
    void set_interval(qint64 first_tai_ms, qint64 last_tai_ms, const QString &object,
                      bool object_interval = true);
    QSize sizeHint() const override;
    void set_scale(CalendarScale scale) {
        scale_ = scale;
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

  private:
    void seek(double x);
    double track_left() const;
    qint64 first_ = 0, last_ = 7'200'000;
    QString object_;
    bool object_interval_ = true;
    CalendarScale scale_ = CalendarScale::utc;
};
} // namespace orbitforge::desktop
