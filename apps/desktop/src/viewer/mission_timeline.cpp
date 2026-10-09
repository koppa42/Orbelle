#include "mission_timeline.hpp"
#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <orbitforge/runtime/time_frames.hpp>

namespace orbitforge::desktop {
MissionTimeline::MissionTimeline(QWidget *parent) : QSlider(Qt::Horizontal, parent) {
    setRange(0, 10000);
    setSingleStep(10);
    setPageStep(1000);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(
        QStringLiteral("分析时间轴，方向键微调，Page Up / Down 跳转，Home / End 到达端点"));
    setMinimumHeight(84);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}
QSize MissionTimeline::sizeHint() const { return {700, 84}; }
double MissionTimeline::track_left() const { return width() < 650 ? 116.0 : 174.0; }
void MissionTimeline::set_interval(qint64 first, qint64 last, const QString &object,
                                   bool object_interval) {
    first_ = first;
    last_ = last;
    object_ = object;
    object_interval_ = object_interval;
    update();
}
void MissionTimeline::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#16222e"));
    const double left = track_left(), right = width() - 28.0;
    const double span = std::max(1.0, right - left);
    p.setPen(QColor("#7890a6"));
    const QString name = scale_ == CalendarScale::utc   ? "UTC"
                         : scale_ == CalendarScale::tai ? "TAI"
                                                        : "TT";
    p.drawText(QRectF(16, 6, left - 30, 20), Qt::AlignVCenter,
               QStringLiteral("分析时段 · ") + name);
    p.setPen(QColor("#9bb5ce"));
    p.drawText(QRectF(16, 42, left - 30, 22), Qt::AlignVCenter,
               fontMetrics().elidedText(object_, Qt::ElideRight, static_cast<int>(left - 32)));
    QFont mono("Consolas");
    mono.setPixelSize(10);
    p.setFont(mono);
    const double desired_ms = (last_ - first_) / std::max(2.0, span / 105);
    qint64 tick_ms = 86'400'000;
    for (const qint64 candidate :
         {1'000LL, 2'000LL, 5'000LL, 10'000LL, 15'000LL, 30'000LL, 60'000LL, 120'000LL, 300'000LL,
          600'000LL, 900'000LL, 1'800'000LL, 3'600'000LL, 7'200'000LL, 21'600'000LL,
          43'200'000LL}) {
        if (candidate >= desired_ms) {
            tick_ms = candidate;
            break;
        }
    }
    const auto draw_tick = [&](qint64 value, bool endpoint) {
        const double x =
            left + span * static_cast<double>(value - first_) / std::max<qint64>(1, last_ - first_);
        p.setPen(QPen(QColor("#2b4053"), 1));
        p.drawLine(QPointF(x, 29), QPointF(x, 75));
        const bool days = last_ - first_ > 86'400'000;
        p.setPen(QColor("#829bb2"));
        const auto text = calendar_text(core::Epoch{value * 1'000'000}, scale_, days);
        if (endpoint || (x - left > 80 && right - x > 80))
            p.drawText(QRectF(x - 42, 5, 84, 20), Qt::AlignCenter, text);
    };
    draw_tick(first_, true);
    draw_tick(last_, true);
    static const auto time = runtime::make_builtin_time_provider();
    const qint64 offset = scale_ == CalendarScale::tt ? 32184
                          : scale_ == CalendarScale::utc
                              ? -time->tai_minus_utc_seconds(core::Epoch{first_ * 1'000'000}) * 1000
                              : 0;
    const auto first_tick =
        static_cast<qint64>(std::floor(static_cast<double>(first_ + offset) / tick_ms) + 1) *
            tick_ms -
        offset;
    for (auto value = first_tick; value < last_; value += tick_ms) {
        draw_tick(value, false);
    }
    p.setPen(QColor("#355571"));
    p.setBrush(QColor("#243f58"));
    p.drawRoundedRect(QRectF(left, 44, span, 18), 3, 3);
    p.setFont(font());
    p.setPen(QColor("#9abbd8"));
    p.drawText(QRectF(left + 10, 44, span - 20, 18), Qt::AlignVCenter,
               object_interval_ ? QStringLiteral("轨道传播") : QStringLiteral("场景分析时段"));
    const double cursor = left + span * (value() - minimum()) / std::max(1, maximum() - minimum());
    p.setPen(QPen(QColor("#95c6ec"), 1.5));
    p.drawLine(QPointF(cursor, 28), QPointF(cursor, 75));
    p.setBrush(QColor("#95c6ec"));
    p.setPen(Qt::NoPen);
    p.drawPolygon(QPolygonF{QPointF(cursor - 4, 25), QPointF(cursor + 4, 25), QPointF(cursor, 32)});
    if (hasFocus()) {
        p.setPen(QPen(QColor("#7a9cbd"), 1, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(2, 2, -3, -3));
    }
}
void MissionTimeline::seek(double x) {
    const auto fraction =
        std::clamp((x - track_left()) / std::max(1.0, width() - 28.0 - track_left()), 0.0, 1.0);
    setValue(minimum() + qRound(fraction * (maximum() - minimum())));
}
void MissionTimeline::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    setFocus(Qt::MouseFocusReason);
    setSliderDown(true);
    seek(event->position().x());
    event->accept();
}
void MissionTimeline::mouseMoveEvent(QMouseEvent *event) {
    if (isSliderDown())
        seek(event->position().x());
    event->accept();
}
void MissionTimeline::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && isSliderDown()) {
        seek(event->position().x());
        setSliderDown(false);
        event->accept();
    } else
        event->ignore();
}
} // namespace orbitforge::desktop
