#include "ground_map.hpp"
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <rocky/Ellipsoid.h>
#include <stdexcept>

namespace orbitforge::desktop {
QPointF ground_coordinates(const std::array<double, 3> &position) {
    static const rocky::Ellipsoid earth;
    for (const auto value : position)
        if (!std::isfinite(value))
            throw std::invalid_argument("Ground map requires a finite ITRF position");
    if (position[0] == 0 && position[1] == 0 && position[2] == 0)
        throw std::invalid_argument("Earth center has no ground location");
    const auto llh = earth.geocentricToGeodetic({position[0], position[1], position[2]});
    if (!std::isfinite(llh.x) || !std::isfinite(llh.y) || std::abs(llh.y) > 90)
        throw std::invalid_argument("Cannot project the ITRF position onto WGS84");
    return {std::remainder(llh.x, 360.0), llh.y};
}
QPainterPath ground_path(const std::vector<QPointF> &points) {
    QPainterPath result;
    if (points.empty())
        return result;
    result.moveTo(points.front());
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto a = points[i - 1], b = points[i];
        if (std::abs(b.x() - a.x()) > 180) {
            const auto delta = std::remainder(b.x() - a.x(), 360.0);
            if (delta == 0) {
                result.lineTo(a.x(), b.y());
                result.moveTo(b);
                continue;
            }
            const double seam = delta > 0 ? 180.0 : -180.0;
            const double latitude = a.y() + (b.y() - a.y()) * (seam - a.x()) / delta;
            result.lineTo(seam, latitude);
            result.moveTo(-seam, latitude);
        }
        result.lineTo(b);
    }
    return result;
}
GroundMap::GroundMap(const QString &imagery, QWidget *parent) : QWidget(parent), imagery_(imagery) {
    if (imagery_.isNull())
        throw std::runtime_error("Cannot load offline imagery for the 2D map");
    setMinimumSize(160, 100);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("二维地面轨迹，滚轮缩放、拖动平移、点击选择卫星、Home 复位"));
}
void GroundMap::set_orbits(const std::vector<std::shared_ptr<const OrbitScene>> &orbits) {
    publish_tracks(prepare_tracks(orbits));
}
GroundMap::PreparedTracks
GroundMap::prepare_tracks(const std::vector<std::shared_ptr<const OrbitScene>> &orbits) {
    PreparedTracks tracks;
    for (const auto &orbit : orbits) {
        std::vector<QPointF> points;
        std::vector<core::Epoch> epochs;
        points.reserve(orbit->track.samples().states().size());
        for (const auto &state : orbit->track.samples().states()) {
            points.push_back(ground_coordinates(state.position_m));
            epochs.push_back(state.epoch);
        }
        const auto size = points.size();
        tracks.push_back({QString::fromStdString(orbit->input.object_id), ground_path(points),
                          std::move(points), std::move(epochs), 0, size});
    }
    return tracks;
}
void GroundMap::publish_tracks(PreparedTracks tracks) {
    tracks_.swap(tracks);
    update();
}
void GroundMap::replace_track(std::size_t index, Track track) {
    tracks_.at(index) = std::move(track);
    update();
}
void GroundMap::set_snapshot(const visualization::RenderSnapshot &snapshot) {
    if (snapshot.frame != core::FrameId::itrf || snapshot.origin != core::OriginId::earth)
        throw std::invalid_argument("2D map requires an Earth/ITRF snapshot");
    std::vector<Point> points;
    for (const auto &marker : snapshot.markers)
        points.push_back(
            {QString::fromStdString(marker.object_id), ground_coordinates(marker.position_m)});
    points_ = std::move(points);
    epoch_ = snapshot.epoch;
    update();
}
void GroundMap::set_selection(std::optional<std::size_t> index) {
    selected_ = index;
    update();
}
void GroundMap::set_display(std::size_t index, const DisplaySettings &settings, core::Epoch first,
                            core::Epoch last) {
    auto &track = tracks_.at(index);
    track.visible = settings.track_2d;
    track.label = settings.labels_2d;
    if (first == last || first.tai_nanoseconds() > track.epochs.back().tai_nanoseconds() ||
        last.tai_nanoseconds() < track.epochs.front().tai_nanoseconds()) {
        track.first = track.last = 0;
        track.path = {};
        return;
    }
    const auto compare = [](core::Epoch a, core::Epoch b) {
        return a.tai_nanoseconds() < b.tai_nanoseconds();
    };
    auto start = std::upper_bound(track.epochs.begin(), track.epochs.end(), first, compare);
    if (start != track.epochs.begin())
        --start;
    auto end = std::lower_bound(track.epochs.begin(), track.epochs.end(), last, compare);
    if (end != track.epochs.end())
        ++end;
    const auto a = static_cast<std::size_t>(std::distance(track.epochs.begin(), start));
    const auto b = static_cast<std::size_t>(std::distance(track.epochs.begin(), end));
    if (a != track.first || b != track.last) {
        track.first = a;
        track.last = b;
        track.path =
            ground_path(std::vector<QPointF>(track.points.begin() + a, track.points.begin() + b));
    }
}
void GroundMap::set_sun_direction(const std::array<double, 3> &direction) {
    const auto length = std::hypot(direction[0], direction[1], direction[2]);
    if (!std::isfinite(length) || std::abs(length - 1) > 1e-6)
        throw std::invalid_argument("Map sunlight requires a finite unit vector");
    const double change =
        std::hypot(direction[0] - sun_direction_[0], direction[1] - sun_direction_[1],
                   direction[2] - sun_direction_[2]);
    if (!night_.isNull() && change < 0.00035)
        return;
    sun_direction_ = direction;
    night_ = QImage(1440, 720, QImage::Format_ARGB32_Premultiplied);
    std::array<double, 1440> longitude_component;
    for (int x = 0; x < night_.width(); ++x) {
        const double lon = ((x + 0.5) / night_.width() * 360 - 180) * std::numbers::pi / 180;
        longitude_component[x] = std::cos(lon) * direction[0] + std::sin(lon) * direction[1];
    }
    for (int y = 0; y < night_.height(); ++y) {
        const double lat = (90 - (y + 0.5) / night_.height() * 180) * std::numbers::pi / 180;
        auto *pixels = reinterpret_cast<QRgb *>(night_.scanLine(y));
        for (int x = 0; x < night_.width(); ++x) {
            const double cosine =
                std::cos(lat) * longitude_component[x] + std::sin(lat) * direction[2];
            // A narrow visual twilight transition around the geometric terminator.
            const double t = std::clamp(0.5 - cosine / 0.006, 0.0, 1.0);
            const double night = t * t * (3 - 2 * t);
            // Multiply preserves local texture contrast and gives ice a blue night tone.
            // White is the identity on the day side; the narrow edge is display antialiasing.
            pixels[x] = qRgb(qRound(255 - 213 * night), qRound(255 - 191 * night),
                             qRound(255 - 165 * night));
        }
    }
    update();
}
void GroundMap::set_lighting(bool enabled) {
    lighting_ = enabled;
    update();
}
void GroundMap::set_grid_visible(bool enabled) {
    grid_ = enabled;
    update();
}
void GroundMap::set_tracks_visible(bool enabled) {
    show_tracks_ = enabled;
    update();
}
void GroundMap::set_labels_visible(bool enabled) {
    labels_ = enabled;
    update();
}
void GroundMap::set_sun_visible(bool enabled) {
    show_sun_ = enabled;
    update();
}
void GroundMap::focus_selection() {
    if (!selected_ || *selected_ >= points_.size())
        return;
    zoom(std::max(1.0, 2.0 / zoom_), plot_rect().center());
    pan_ += plot_rect().center() - marker_position(*selected_);
    clamp_pan();
    update();
}
QRectF GroundMap::plot_rect() const { return QRectF(rect()).adjusted(38, 14, -16, -44); }
QRectF GroundMap::map_rect() const {
    const auto plot = plot_rect();
    const double width = std::min(plot.width(), plot.height() * 2) * zoom_;
    const QSizeF size(width, width / 2);
    return {plot.center() + pan_ - QPointF(size.width() / 2, size.height() / 2), size};
}
QTransform GroundMap::map_transform() const {
    const auto area = map_rect();
    return QTransform(area.width() / 360, 0, 0, -area.height() / 180, area.center().x(),
                      area.center().y());
}
QPointF GroundMap::marker_coordinates(std::size_t index) const {
    return points_.at(index).geographic;
}
QPointF GroundMap::marker_position(std::size_t index) const {
    return map_transform().map(marker_coordinates(index));
}
void GroundMap::clamp_pan() {
    const auto plot = plot_rect(), map = map_rect();
    const auto x = std::max(0.0, (map.width() - plot.width()) / 2);
    const auto y = std::max(0.0, (map.height() - plot.height()) / 2);
    pan_.setX(std::clamp(pan_.x(), -x, x));
    pan_.setY(std::clamp(pan_.y(), -y, y));
}
void GroundMap::reset_view() {
    zoom_ = 1;
    pan_ = {};
    update();
}
void GroundMap::zoom(double factor, QPointF anchor) {
    const auto before = map_rect();
    const auto next = std::clamp(zoom_ * factor, 1.0, 12.0);
    pan_ = anchor - plot_rect().center() - (anchor - before.center()) * (next / zoom_);
    zoom_ = next;
    clamp_pan();
    update();
}
void GroundMap::resizeEvent(QResizeEvent *) { clamp_pan(); }
void GroundMap::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), QColor("#080f18"));
    const auto plot = plot_rect(), area = map_rect();
    const auto transform = map_transform();
    p.save();
    p.setClipRect(plot);
    p.drawImage(area, imagery_);
    p.fillRect(area, QColor(4, 12, 24, 35));
    if (lighting_ && !night_.isNull()) {
        p.setCompositionMode(QPainter::CompositionMode_Multiply);
        p.drawImage(area, night_);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    const int grid_step = area.width() > 6000   ? 5
                          : area.width() > 3000 ? 10
                          : area.width() > 1600 ? 15
                                                : 30;
    if (grid_) {
        p.setPen(QPen(QColor(177, 208, 233, 75), 0.7, Qt::DotLine));
        for (int lon = -180; lon <= 180; lon += grid_step)
            p.drawLine(transform.map(QPointF(lon, -90)), transform.map(QPointF(lon, 90)));
        for (int lat = -90; lat <= 90; lat += grid_step)
            p.drawLine(transform.map(QPointF(-180, lat)), transform.map(QPointF(180, lat)));
    }
    if (show_sun_ && !night_.isNull()) {
        const auto sun = transform.map(
            QPointF(std::atan2(sun_direction_[1], sun_direction_[0]) * 180 / std::numbers::pi,
                    std::asin(std::clamp(sun_direction_[2], -1.0, 1.0)) * 180 / std::numbers::pi));
        p.setPen(QPen(QColor("#efd69b"), 1));
        p.setBrush(QColor("#efd69b"));
        p.drawEllipse(sun, 3, 3);
        for (int i = 0; i < 8; ++i) {
            const double angle = i * std::numbers::pi / 4;
            const QPointF ray(std::cos(angle), std::sin(angle));
            p.drawLine(sun + ray * 6, sun + ray * 9);
        }
    }
    p.setBrush(Qt::NoBrush);
    const QColor colors[] = {QColor("#6cb9d6"), QColor("#dda16d"), QColor("#9bbb88")};
    for (std::size_t i = 0; show_tracks_ && i < tracks_.size(); ++i) {
        if (!tracks_[i].visible)
            continue;
        const auto path = transform.map(tracks_[i].path);
        p.setPen(QPen(QColor(4, 15, 26, 140), 2.8));
        p.drawPath(path);
        p.setPen(QPen(colors[i % 3], selected_ == i ? 1.2 : 0.9));
        p.drawPath(path);
    }
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const auto point = marker_position(i);
        if (!plot.contains(point))
            continue;
        p.setPen(QPen(QColor("#172d42"), 1.5));
        p.setBrush(selected_ == i ? QColor("#ffce81") : QColor("#d0efff"));
        p.drawEllipse(point, 4, 4);
        if (selected_ == i) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor("#ffce81"), 1));
            p.drawEllipse(point, 8, 8);
        }
        if (!labels_ || (i < tracks_.size() && !tracks_[i].label))
            continue;
        const auto name = fontMetrics().elidedText(points_[i].name, Qt::ElideRight,
                                                   static_cast<int>(plot.width()) - 12);
        const auto label_width = fontMetrics().horizontalAdvance(name) + 12;
        const QRectF label(std::clamp(point.x() + 12, plot.left(), plot.right() - label_width),
                           std::clamp(point.y() - 12, plot.top(), plot.bottom() - 22), label_width,
                           22);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(13, 32, 49, 215));
        p.drawRoundedRect(label, 3, 3);
        p.setPen(QColor("#d9e9f6"));
        p.drawText(label, Qt::AlignCenter, name);
    }
    p.restore();
    p.setPen(QColor("#7c94ad"));
    QFont mono("Consolas");
    mono.setPixelSize(10);
    p.setFont(mono);
    for (int lon = -180; grid_ && lon <= 180; lon += grid_step * (area.width() < 800 ? 2 : 1)) {
        const double x = transform.map(QPointF(lon, 0)).x();
        if (x >= plot.left() && x <= plot.right())
            p.drawText(QRectF(x - 24, std::min(area.bottom(), plot.bottom()) + 3, 48, 16),
                       Qt::AlignCenter,
                       QString::number(std::abs(lon)) + (lon < 0   ? " W"
                                                         : lon > 0 ? " E"
                                                                   : ""));
    }
    for (int lat = -90; grid_ && lat <= 90; lat += grid_step) {
        const double y = transform.map(QPointF(0, lat)).y();
        if (y >= plot.top() && y <= plot.bottom())
            p.drawText(QRectF(0, y - 9, 31, 18), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(std::abs(lat)) + (lat < 0   ? "S"
                                                         : lat > 0 ? "N"
                                                                   : ""));
    }
    p.setFont(font());
    p.setPen(QColor("#92aac2"));
    QString caption = QStringLiteral("WGS84 · 等距圆柱   |   滚轮缩放 · 拖动平移");
    if (cursor_position_) {
        const auto ll = map_transform().inverted().map(*cursor_position_);
        caption = QStringLiteral("经度 %1°%2    纬度 %3°%4")
                      .arg(std::abs(ll.x()), 0, 'f', 3)
                      .arg(ll.x() < 0 ? "W" : "E")
                      .arg(std::abs(ll.y()), 0, 'f', 3)
                      .arg(ll.y() < 0 ? "S" : "N");
    } else if (selected_ && *selected_ < points_.size()) {
        const auto &point = points_[*selected_];
        caption = point.name + QStringLiteral("   %1°%2   %3°%4")
                                   .arg(std::abs(point.geographic.y()), 0, 'f', 3)
                                   .arg(point.geographic.y() < 0 ? "S" : "N")
                                   .arg(std::abs(point.geographic.x()), 0, 'f', 3)
                                   .arg(point.geographic.x() < 0 ? "W" : "E");
    }
    p.drawText(QRectF(16, height() - 22, width() - 32, 18), Qt::AlignVCenter,
               fontMetrics().elidedText(caption, Qt::ElideRight, width() - 32));
}
void GroundMap::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton)
        return;
    setFocus(Qt::MouseFocusReason);
    dragging_ = true;
    moved_ = false;
    press_ = last_mouse_ = event->position();
    setCursor(Qt::ClosedHandCursor);
}
void GroundMap::mouseMoveEvent(QMouseEvent *event) {
    cursor_position_ = plot_rect().intersected(map_rect()).contains(event->position())
                           ? std::optional<QPointF>(event->position())
                           : std::nullopt;
    update();
    if (!dragging_)
        return;
    if ((event->position() - press_).manhattanLength() > 4)
        moved_ = true;
    if (moved_) {
        pan_ += event->position() - last_mouse_;
        clamp_pan();
        update();
    }
    last_mouse_ = event->position();
}
void GroundMap::leaveEvent(QEvent *) {
    cursor_position_.reset();
    update();
}
void GroundMap::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !dragging_)
        return;
    dragging_ = false;
    unsetCursor();
    if (!moved_ && select) {
        std::optional<std::size_t> hit;
        double nearest = 12;
        for (std::size_t i = 0; i < points_.size(); ++i) {
            const auto point = marker_position(i);
            const auto distance = QLineF(point, event->position()).length();
            if (plot_rect().contains(point) && distance < nearest) {
                nearest = distance;
                hit = i;
            }
        }
        select(hit);
    }
}
void GroundMap::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        unsetCursor();
        reset_view();
    }
}
void GroundMap::wheelEvent(QWheelEvent *event) {
    zoom(std::pow(1.2, event->angleDelta().y() / 120.0), event->position());
    event->accept();
}
void GroundMap::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Home)
        reset_view();
    else if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal)
        zoom(1.3, plot_rect().center());
    else if (event->key() == Qt::Key_Minus)
        zoom(1 / 1.3, plot_rect().center());
    else
        QWidget::keyPressEvent(event);
}
} // namespace orbitforge::desktop
