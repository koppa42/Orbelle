#include "mission_timeline.hpp"
#include "workbench_style.hpp"
#include <QDateTime>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QTimeZone>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <orbitforge/runtime/time_frames.hpp>

namespace orbitforge::desktop {
namespace {
constexpr qint64 day_ms = 86'400'000;
constexpr qint64 minimum_span = 1'000;
const QDateTime origin(QDate(2000, 1, 1), QTime(12, 0), QTimeZone::UTC);
const QColor cursor_color("#e4b16c");
const core::ITimeProvider &time_provider() {
    static const auto provider = runtime::make_builtin_time_provider();
    return *provider;
}

QString scale_name(CalendarScale scale) {
    return scale == CalendarScale::utc ? "UTC" : scale == CalendarScale::tai ? "TAI" : "TT";
}
QString full_time(qint64 epoch, CalendarScale scale) {
    return calendar_text(core::Epoch{epoch * 1'000'000}, scale);
}
QString duration_text(qint64 milliseconds) {
    if (milliseconds % day_ms == 0)
        return QStringLiteral("%1 天").arg(milliseconds / day_ms);
    if (milliseconds % 3'600'000 == 0)
        return QStringLiteral("%1 h").arg(milliseconds / 3'600'000);
    if (milliseconds >= 3'600'000)
        return QStringLiteral("%1 h").arg(milliseconds / 3'600'000.0, 0, 'f', 1);
    if (milliseconds >= 60'000)
        return QStringLiteral("%1 min").arg(milliseconds / 60'000.0, 0, 'f', 1);
    return QStringLiteral("%1 s").arg(milliseconds / 1000.0, 0, 'f', 1);
}
qint64 floor_multiple(qint64 value, qint64 step) {
    const auto remainder = (value % step + step) % step;
    return value - remainder;
}
// Calendar coordinates are used only to align ticks. Every UTC tick is converted
// separately: a single TAI-UTC offset would misalign ticks after a leap second.
qint64 calendar_coordinate(qint64 epoch, CalendarScale scale) {
    if (scale != CalendarScale::utc)
        return origin.toMSecsSinceEpoch() + epoch + (scale == CalendarScale::tt ? 32184 : 0);
    const auto p = time_provider().to_utc(core::Epoch{epoch * 1'000'000});
    return QDateTime(QDate(p.year, p.month, p.day),
                     QTime(p.hour, p.minute, std::min(p.second, 59), p.nanosecond / 1'000'000),
                     QTimeZone::UTC)
        .toMSecsSinceEpoch();
}
qint64 from_coordinate(qint64 calendar, CalendarScale scale) {
    if (scale != CalendarScale::utc)
        return calendar - origin.toMSecsSinceEpoch() - (scale == CalendarScale::tt ? 32184 : 0);
    const auto value = QDateTime::fromMSecsSinceEpoch(calendar, QTimeZone::UTC);
    const auto date = value.date();
    const auto clock = value.time();
    return time_provider()
               .from_utc({date.year(), date.month(), date.day(), clock.hour(), clock.minute(),
                          clock.second(), clock.msec() * 1'000'000})
               .tai_nanoseconds() /
           1'000'000;
}
} // namespace

MissionTimeline::MissionTimeline(QWidget *parent) : QSlider(Qt::Horizontal, parent) {
    setObjectName("missionTimeline");
    setRange(0, 10000);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("场景时间轴"));
    setAccessibleDescription(QStringLiteral(
        "上方总览拖动浏览范围，两端拖动缩放；下方单击或拖动跳转。滚轮缩放，Shift 拖动平移。"
        "左右键微调时间，Home/End 到达场景端点，Page Up/Down 平移，F 显示全时段，C 定位游标。"));
    setMinimumHeight(sizeHint().height());
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(this, &QSlider::valueChanged, this, [this](int value) {
        current_ = first_ + std::llround((last_ - first_) * (value / 10000.0));
        update();
    });
}
QSize MissionTimeline::sizeHint() const { return {720, show_object_interval_ ? 144 : 118}; }
double MissionTimeline::track_left() const { return width() < 900 ? 154.0 : 190.0; }
QRectF MissionTimeline::overview_rect() const {
    return {track_left(), 23, std::max(1.0, width() - 18.0 - track_left()), 13};
}
QRectF MissionTimeline::detail_rect() const {
    return {track_left(), 44, overview_rect().width(), static_cast<double>(height() - 48)};
}
void MissionTimeline::set_interval(qint64 first, qint64 last, const QString &scenario,
                                   const QString &object) {
    if (last <= first)
        throw std::invalid_argument("Timeline interval must be positive");
    const bool changed = first != first_ || last != last_;
    first_ = first;
    last_ = last;
    scenario_ = scenario;
    object_ = object;
    if (changed) {
        drag_ = Drag::none;
        setSliderDown(false);
        reset_view();
        set_current_time(std::clamp(current_, first_, last_));
    }
    update();
}
void MissionTimeline::set_scale(CalendarScale scale) {
    scale_ = scale;
    update();
}
void MissionTimeline::set_show_object_interval(bool show) {
    if (show_object_interval_ == show)
        return;
    show_object_interval_ = show;
    setMinimumHeight(sizeHint().height());
    updateGeometry();
    update();
}
void MissionTimeline::set_current_time(qint64 epoch, bool reveal) {
    current_ = std::clamp(epoch, first_, last_);
    const QSignalBlocker block(this);
    setValue(qRound(10000.0 * (current_ - first_) / (last_ - first_)));
    if (reveal && (current_ < view_first_ || current_ > view_last_))
        center_current();
    update();
}
void MissionTimeline::set_view(qint64 first, qint64 span) {
    span = std::clamp(span, std::min(minimum_span, last_ - first_), last_ - first_);
    view_first_ = std::clamp(first, first_, last_ - span);
    view_last_ = view_first_ + span;
    update();
}
void MissionTimeline::reset_view() { set_view(first_, last_ - first_); }
void MissionTimeline::center_current() {
    const auto span = view_last_ - view_first_;
    set_view(current_ - span / 2, span);
}
void MissionTimeline::zoom(double factor) {
    const auto fraction =
        current_ >= view_first_ && current_ <= view_last_
            ? static_cast<double>(current_ - view_first_) / (view_last_ - view_first_)
            : 0.5;
    zoom_at(factor, fraction);
}
void MissionTimeline::zoom_at(double factor, double fraction) {
    if (!std::isfinite(factor) || factor <= 0)
        return;
    const auto old_span = view_last_ - view_first_;
    const auto span = static_cast<qint64>(std::llround(
        std::clamp(old_span / factor, static_cast<double>(std::min(minimum_span, last_ - first_)),
                   static_cast<double>(last_ - first_))));
    set_view(view_first_ + std::llround((old_span - span) * fraction), span);
}
qint64 MissionTimeline::time_at(double x, bool overview) const {
    const auto fraction = std::clamp((x - track_left()) / overview_rect().width(), 0.0, 1.0);
    const auto first = overview ? first_ : view_first_;
    const auto last = overview ? last_ : view_last_;
    return first + std::llround(fraction * (last - first));
}
double MissionTimeline::position_at(qint64 epoch, bool overview) const {
    const auto first = overview ? first_ : view_first_;
    const auto last = overview ? last_ : view_last_;
    return track_left() +
           overview_rect().width() * static_cast<double>(epoch - first) / (last - first);
}
qint64 MissionTimeline::tick_step() const {
    const double desired = (view_last_ - view_first_) / std::max(1.0, detail_rect().width() / 96);
    for (const qint64 step :
         {1LL,          2LL,          5LL,          10LL,          20LL,         50LL,
          100LL,        200LL,        500LL,        1'000LL,       2'000LL,      5'000LL,
          10'000LL,     15'000LL,     30'000LL,     60'000LL,      120'000LL,    300'000LL,
          600'000LL,    900'000LL,    1'800'000LL,  3'600'000LL,   7'200'000LL,  10'800'000LL,
          21'600'000LL, 43'200'000LL, 86'400'000LL, 172'800'000LL, 604'800'000LL})
        if (step >= desired)
            return step;
    return static_cast<qint64>(std::ceil(desired / day_ms)) * day_ms;
}
std::vector<qint64> MissionTimeline::ruler_ticks() const {
    std::vector<qint64> result;
    const auto step = tick_step();
    const auto begin = floor_multiple(calendar_coordinate(view_first_, scale_), step);
    const auto end = calendar_coordinate(view_last_, scale_) + step +
                     (scale_ == CalendarScale::utc && step <= 1000 ? 1000 : 0);
    std::optional<qint64> previous;
    for (auto calendar = begin; calendar <= end; calendar += step) {
        qint64 epoch;
        try {
            epoch = from_coordinate(calendar, scale_);
        } catch (const std::out_of_range &) {
            // An aligned tick can be just outside the time provider's coverage.
            continue;
        }
        if (previous && step <= 1000) {
            // Qt calendars omit 23:59:60. Fill that SI second on fine rulers.
            for (auto leap = *previous + step; leap < epoch; leap += step)
                if (leap >= view_first_ && leap <= view_last_)
                    result.push_back(leap);
        }
        if (epoch >= view_first_ && epoch <= view_last_)
            result.push_back(epoch);
        previous = epoch;
    }
    return result;
}

void MissionTimeline::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#151b24"));
    const auto overview = overview_rect();
    const auto detail = detail_rect();
    const auto left = detail.left(), right = detail.right();
    p.fillRect(QRectF(0, 0, left - 10, height()), QColor("#1c2430"));
    p.setPen(QColor("#303c4b"));
    p.drawLine(QPointF(0, 41), QPointF(width(), 41));
    p.drawLine(QPointF(left - 10, 0), QPointF(left - 10, height()));

    p.setPen(QColor("#b6c5d7"));
    p.drawText(QRectF(16, 2, left - 32, 20), Qt::AlignVCenter, QStringLiteral("场景总览"));
    p.setPen(QColor("#8396ac"));
    p.drawText(QRectF(16, 21, left - 32, 16), Qt::AlignVCenter, duration_text(last_ - first_));
    p.drawText(QRectF(16, 48, left - 32, 18), Qt::AlignVCenter,
               QStringLiteral("时间 / ") + scale_name(scale_));
    p.drawText(QRectF(16, 66, left - 32, 16), Qt::AlignVCenter,
               QStringLiteral("可视 ") + duration_text(view_last_ - view_first_));

    QFont mono("Consolas");
    mono.setPixelSize(11);
    p.setFont(mono);
    p.setPen(QColor("#90a4bc"));
    // Always include both endpoint dates, even for an exact one-day scenario.
    p.drawText(QRectF(left, 1, detail.width() / 2, 18), Qt::AlignLeft | Qt::AlignVCenter,
               full_time(first_, scale_).left(20));
    p.drawText(QRectF(left + detail.width() / 2, 1, detail.width() / 2, 18),
               Qt::AlignRight | Qt::AlignVCenter, full_time(last_, scale_).left(20));
    p.fillRect(overview, QColor("#101721"));
    p.setPen(QColor("#2b3b4d"));
    for (int i = 0; i <= 24; ++i) {
        const auto x = left + overview.width() * i / 24;
        p.drawLine(QPointF(x, overview.top()), QPointF(x, overview.bottom()));
    }
    const double a = position_at(view_first_, true), b = position_at(view_last_, true);
    p.fillRect(QRectF(a, overview.top(), std::max(2.0, b - a), overview.height()),
               QColor("#2a435b"));
    p.setPen(QPen(QColor("#5a83a6"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(a, overview.top(), std::max(2.0, b - a), overview.height()));
    if (b - a >= 18) {
        p.setPen(QPen(QColor("#a6c8e3"), 2));
        for (double x : {a + 3, b - 3})
            p.drawLine(QPointF(x, overview.top() + 3), QPointF(x, overview.bottom() - 3));
    }
    p.setPen(QPen(cursor_color, 2));
    const double overview_cursor = position_at(current_, true);
    p.drawLine(QPointF(overview_cursor, overview.top() - 2),
               QPointF(overview_cursor, overview.bottom() + 2));

    p.save();
    p.setClipRect(detail.adjusted(-1, 0, 1, 0));
    p.setPen(QColor("#91a6bc"));
    p.drawText(QRectF(left, 45, 110, 17), Qt::AlignLeft | Qt::AlignVCenter,
               full_time(view_first_, scale_).left(10));
    p.drawText(QRectF(right - 110, 45, 110, 17), Qt::AlignRight | Qt::AlignVCenter,
               full_time(view_last_, scale_).left(10));
    const auto step = tick_step();
    const auto label = [&](qint64 epoch) {
        return full_time(epoch, scale_).mid(12, step < 1000 ? 12 : 8);
    };
    const auto ticks = ruler_ticks();
    // Five subdivisions per major SI interval, with calendar-aligned major ticks.
    for (std::size_t i = 1; i < ticks.size(); ++i) {
        p.setPen(QColor("#40536a"));
        for (int part = 1; part < 5; ++part) {
            const auto epoch = ticks[i - 1] + (ticks[i] - ticks[i - 1]) * part / 5;
            const auto x = position_at(epoch);
            p.drawLine(QPointF(x, 81), QPointF(x, 85));
        }
    }
    const double label_width = step < 1000 ? 84 : 62;
    for (const auto epoch : ticks) {
        const auto x = position_at(epoch);
        p.setPen(QColor("#263445"));
        p.drawLine(QPointF(x, 86), QPointF(x, 139));
        p.setPen(QColor("#5c7690"));
        p.drawLine(QPointF(x, 78), QPointF(x, 86));
        if (x - left > label_width * 1.6 && right - x > label_width * 1.6) {
            p.setPen(QColor("#a5bcd3"));
            p.drawText(QRectF(x - label_width / 2, 62, label_width, 17), Qt::AlignCenter,
                       label(epoch));
            if (view_last_ - view_first_ >= day_ms * 2) {
                p.setPen(QColor("#8396ac"));
                p.drawText(QRectF(x - 40, 45, 80, 17), Qt::AlignCenter,
                           full_time(epoch, scale_).mid(5, 5));
            }
        }
    }
    p.setPen(QColor("#c1d0e1"));
    p.drawText(QRectF(left, 62, label_width + 4, 17), Qt::AlignLeft | Qt::AlignVCenter,
               label(view_first_));
    p.drawText(QRectF(right - label_width - 4, 62, label_width + 4, 17),
               Qt::AlignRight | Qt::AlignVCenter, label(view_last_));
    p.setPen(QColor("#455d76"));
    p.drawLine(QPointF(left, 85), QPointF(right, 85));
    p.restore();

    static const auto scenario_icon = workbench_icon(WorkbenchIcon::earth);
    static const auto satellite_icon = workbench_icon(WorkbenchIcon::satellite);
    const auto track = [&](double y, const QString &name, const QString &caption, const QIcon &icon,
                           const QColor &fill, const QColor &edge) {
        p.setFont(font());
        p.setPen(QColor("#bacadd"));
        p.drawPixmap(QRect(16, static_cast<int>(y) + 2, 14, 14), icon.pixmap(28, 28));
        p.drawText(QRectF(38, y, left - 56, 18), Qt::AlignVCenter,
                   fontMetrics().elidedText(name, Qt::ElideRight, static_cast<int>(left - 58)));
        p.setPen(edge);
        p.setBrush(fill);
        p.drawRect(QRectF(left, y, detail.width(), 18));
        p.setPen(QColor("#b4cfe6"));
        p.drawText(QRectF(left + 9, y, detail.width() - 18, 18), Qt::AlignVCenter, caption);
    };
    track(91, scenario_, QStringLiteral("场景分析区间"), scenario_icon, QColor("#23374b"),
          QColor("#3b5974"));
    if (show_object_interval_ && !object_.isEmpty())
        track(117, object_, QStringLiteral("轨道传播"), satellite_icon, QColor("#213e49"),
              QColor("#3e6b7d"));
    else if (show_object_interval_) {
        p.setFont(font());
        p.setPen(QColor("#8c9eb4"));
        p.drawText(QRectF(left + 9, 117, detail.width() - 18, 18), Qt::AlignVCenter,
                   QStringLiteral("选择卫星查看传播区间"));
    }
    p.setPen(QPen(cursor_color, 1.5));
    p.setBrush(cursor_color);
    if (current_ >= view_first_ && current_ <= view_last_) {
        const auto x = position_at(current_);
        p.drawLine(QPointF(x, 84), QPointF(x, height() - 4));
        p.drawPolygon(QPolygonF{QPointF(x - 4, 79), QPointF(x + 4, 79), QPointF(x + 4, 84),
                                QPointF(x, 89), QPointF(x - 4, 84)});
    } else {
        const auto x = current_ < view_first_ ? left : right;
        const double direction = current_ < view_first_ ? 1 : -1;
        p.drawPolygon(QPolygonF{QPointF(x, 84), QPointF(x + direction * 6, 79),
                                QPointF(x + direction * 6, 89)});
    }
    if (hasFocus()) {
        p.setPen(QPen(QColor("#74b9ee"), 1));
        p.drawLine(QPointF(left, height() - 2), QPointF(right, height() - 2));
    }
}

void MissionTimeline::request_seek(qint64 epoch) {
    set_current_time(epoch, true);
    if (seek_requested)
        seek_requested(current_);
}
void MissionTimeline::mousePressEvent(QMouseEvent *event) {
    const auto point = event->position();
    if (point.x() < track_left() || point.x() > detail_rect().right() ||
        (event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton)) {
        event->ignore();
        return;
    }
    setFocus(Qt::MouseFocusReason);
    QToolTip::hideText();
    drag_x_ = point.x();
    drag_first_ = view_first_;
    drag_last_ = view_last_;
    if (point.y() >= 20 && point.y() <= 40) {
        const auto a = position_at(view_first_, true), b = position_at(view_last_, true);
        if (b - a >= 18 && std::abs(point.x() - a) <= 6)
            drag_ = Drag::first;
        else if (b - a >= 18 && std::abs(point.x() - b) <= 6)
            drag_ = Drag::last;
        else {
            drag_ = Drag::overview;
            if (point.x() < a || point.x() > b) {
                set_view(time_at(point.x(), true) - (view_last_ - view_first_) / 2,
                         view_last_ - view_first_);
                drag_first_ = view_first_;
                drag_last_ = view_last_;
            }
        }
    } else if (detail_rect().contains(point)) {
        if (event->button() == Qt::MiddleButton || event->modifiers().testFlag(Qt::ShiftModifier))
            drag_ = Drag::pan;
        else {
            drag_ = Drag::seek;
            setSliderDown(true);
            request_seek(time_at(point.x()));
        }
    }
    if (drag_ == Drag::pan || drag_ == Drag::overview)
        setCursor(Qt::ClosedHandCursor);
    event->accept();
}
void MissionTimeline::drag_to(double x) {
    const auto span = drag_last_ - drag_first_;
    switch (drag_) {
    case Drag::seek:
        request_seek(time_at(x));
        break;
    case Drag::pan:
        set_view(drag_first_ - std::llround((x - drag_x_) * span / detail_rect().width()), span);
        break;
    case Drag::overview:
        set_view(drag_first_ +
                     std::llround((x - drag_x_) * (last_ - first_) / overview_rect().width()),
                 span);
        break;
    case Drag::first: {
        const auto first = std::clamp(time_at(x, true), first_,
                                      drag_last_ - std::min(minimum_span, last_ - first_));
        set_view(first, drag_last_ - first);
        break;
    }
    case Drag::last:
        set_view(drag_first_, time_at(x, true) - drag_first_);
        break;
    case Drag::none:
        break;
    }
}
void MissionTimeline::mouseMoveEvent(QMouseEvent *event) {
    if (drag_ != Drag::none) {
        drag_to(event->position().x());
    } else if (overview_rect().adjusted(-5, -3, 5, 4).contains(event->position())) {
        const auto a = position_at(view_first_, true), b = position_at(view_last_, true);
        const bool edge = b - a >= 18 && (std::abs(event->position().x() - a) <= 6 ||
                                          std::abs(event->position().x() - b) <= 6);
        setCursor(edge ? Qt::SizeHorCursor : Qt::OpenHandCursor);
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("拖动浏览范围 · 两端拖动缩放 · 双击显示全时段"), this);
    } else if (detail_rect().contains(event->position())) {
        setCursor(Qt::CrossCursor);
        QToolTip::showText(event->globalPosition().toPoint(),
                           full_time(time_at(event->position().x()), scale_) + " " +
                               scale_name(scale_) +
                               QStringLiteral("\n单击跳转 · 滚轮缩放 · Shift 拖动平移"),
                           this);
    } else {
        unsetCursor();
        QToolTip::hideText();
    }
    event->accept();
}
void MissionTimeline::mouseReleaseEvent(QMouseEvent *event) {
    if ((event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) &&
        drag_ != Drag::none) {
        drag_to(event->position().x());
        drag_ = Drag::none;
        setSliderDown(false);
        unsetCursor();
        event->accept();
    } else
        event->ignore();
}
void MissionTimeline::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && event->position().y() <= 41) {
        drag_ = Drag::none;
        setSliderDown(false);
        reset_view();
        event->accept();
    } else
        event->ignore();
}
void MissionTimeline::wheelEvent(QWheelEvent *event) {
    if (event->position().x() < track_left()) {
        event->ignore();
        return;
    }
    const auto delta = event->angleDelta();
    const double steps = (delta.y() != 0 ? delta.y() : delta.x()) / 120.0;
    if (event->modifiers().testFlag(Qt::ShiftModifier) || delta.y() == 0)
        set_view(view_first_ + std::llround((view_last_ - view_first_) * steps * 0.2),
                 view_last_ - view_first_);
    else
        zoom_at(
            std::pow(1.4, steps),
            std::clamp((event->position().x() - track_left()) / detail_rect().width(), 0.0, 1.0));
    QToolTip::hideText();
    event->accept();
}
void MissionTimeline::keyPressEvent(QKeyEvent *event) {
    const auto span = view_last_ - view_first_;
    const auto step = std::max<qint64>(1, span / 1000);
    switch (event->key()) {
    case Qt::Key_Left:
        request_seek(current_ - step);
        break;
    case Qt::Key_Right:
        request_seek(current_ + step);
        break;
    case Qt::Key_Home:
        request_seek(first_);
        break;
    case Qt::Key_End:
        request_seek(last_);
        break;
    case Qt::Key_PageUp:
        set_view(view_first_ - span * 4 / 5, span);
        break;
    case Qt::Key_PageDown:
        set_view(view_first_ + span * 4 / 5, span);
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoom(2);
        break;
    case Qt::Key_Minus:
        zoom(0.5);
        break;
    case Qt::Key_F:
        reset_view();
        break;
    case Qt::Key_C:
        center_current();
        break;
    default:
        QSlider::keyPressEvent(event);
        return;
    }
    event->accept();
}
void MissionTimeline::leaveEvent(QEvent *event) {
    if (drag_ == Drag::none)
        unsetCursor();
    QToolTip::hideText();
    QSlider::leaveEvent(event);
}
} // namespace orbitforge::desktop
