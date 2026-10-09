#include "ground_map.hpp"
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
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
    setMinimumSize(360, 260);
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
        points.reserve(orbit->track.samples().states().size());
        for (const auto &state : orbit->track.samples().states())
            points.push_back(ground_coordinates(state.position_m));
        tracks.push_back({QString::fromStdString(orbit->input.object_id), ground_path(points)});
    }
    return tracks;
}
void GroundMap::publish_tracks(PreparedTracks tracks) {
    tracks_.swap(tracks);
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
QRectF GroundMap::plot_rect() const { return QRectF(rect()).adjusted(36, 36, -16, -44); }
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
    p.fillRect(rect(), QColor("#101d2b"));
    const auto plot = plot_rect(), area = map_rect();
    const auto transform = map_transform();
    p.setPen(QColor("#8fa5bc"));
    p.drawText(QRectF(16, 8, width() - 32, 20), Qt::AlignVCenter,
               QStringLiteral("地面轨迹  ·  等距圆柱 / WGS84"));
    p.save();
    p.setClipRect(plot);
    p.drawImage(area, imagery_);
    p.fillRect(area, QColor(6, 19, 33, 90));
    p.setPen(QPen(QColor(177, 208, 233, 48), 0.7));
    for (int lon = -180; lon <= 180; lon += 30)
        p.drawLine(transform.map(QPointF(lon, -90)), transform.map(QPointF(lon, 90)));
    for (int lat = -90; lat <= 90; lat += 30)
        p.drawLine(transform.map(QPointF(-180, lat)), transform.map(QPointF(180, lat)));
    const QColor colors[] = {QColor("#80cff2"), QColor("#f1b078"), QColor("#abcf91")};
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const auto path = transform.map(tracks_[i].path);
        p.setPen(QPen(QColor(4, 15, 26, 140), 2.8));
        p.drawPath(path);
        p.setPen(QPen(colors[i % 3], selected_ == i ? 1.5 : 1.0));
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
    for (int lon = -180; lon <= 180; lon += 60) {
        const double x = transform.map(QPointF(lon, 0)).x();
        if (x >= plot.left() && x <= plot.right())
            p.drawText(QRectF(x - 24, std::min(area.bottom(), plot.bottom()) + 3, 48, 16),
                       Qt::AlignCenter,
                       QString::number(std::abs(lon)) + (lon < 0   ? " W"
                                                         : lon > 0 ? " E"
                                                                   : ""));
    }
    for (int lat = -60; lat <= 60; lat += 30) {
        const double y = transform.map(QPointF(0, lat)).y();
        if (y >= plot.top() && y <= plot.bottom())
            p.drawText(QRectF(0, y - 9, 31, 18), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(std::abs(lat)) + (lat < 0   ? "S"
                                                         : lat > 0 ? "N"
                                                                   : ""));
    }
    p.setFont(font());
    p.setPen(QColor("#92aac2"));
    QString caption = QStringLiteral("滚轮缩放 · 拖动平移 · 双击复位");
    if (selected_ && *selected_ < points_.size()) {
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
