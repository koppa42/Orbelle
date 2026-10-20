#include "ground_map.hpp"
#include "../ui_text.hpp"
#include "track_style_editor.hpp"
#include "workbench_profile.hpp"
#include "workbench_theme.hpp"
#include <QApplication>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <rocky/Ellipsoid.h>
#include <stdexcept>

namespace orbitforge::desktop {
namespace {
bool draw_track(QPainter &painter, const QPainterPath &path,
                const std::atomic_bool *cancelled = nullptr) {
    // Independent line primitives avoid the raster stroker's large intersecting
    // polygons. Keep every segment and preserve all antimeridian breaks.
    std::array<QLineF, 256> lines;
    const auto margin = painter.pen().widthF() / 2 + 2;
    const auto clip = painter.clipBoundingRect().adjusted(-margin, -margin, margin, margin);
    auto pen = painter.pen();
    const bool dashed = pen.style() != Qt::SolidLine;
    double phase = 0;
    double cycle = 0;
    for (const auto length : pen.dashPattern())
        cycle += length * pen.widthF();
    int count = 0;
    QPointF previous;
    for (int i = 0; i < path.elementCount(); ++i) {
        if ((i % 256) == 0 && cancelled && cancelled->load(std::memory_order_relaxed))
            return false;
        const auto element = path.elementAt(i);
        const QPointF point(element.x, element.y);
        if (element.isMoveTo())
            phase = 0;
        const QLineF segment(previous, point);
        const bool outside = (previous.x() < clip.left() && point.x() < clip.left()) ||
                             (previous.x() > clip.right() && point.x() > clip.right()) ||
                             (previous.y() < clip.top() && point.y() < clip.top()) ||
                             (previous.y() > clip.bottom() && point.y() > clip.bottom());
        if (!element.isMoveTo() && !outside) {
            if (dashed) {
                pen.setDashOffset(phase / pen.widthF());
                painter.setPen(pen);
                painter.drawLine(segment);
            } else
                lines[count++] = segment;
            if (count == lines.size()) {
                painter.drawLines(lines.data(), count);
                count = 0;
            }
        }
        if (dashed && !element.isMoveTo() && cycle > 0)
            phase = std::fmod(phase + segment.length(), cycle);
        previous = point;
    }
    if (count)
        painter.drawLines(lines.data(), count);
    return !cancelled || !cancelled->load(std::memory_order_relaxed);
}
} // namespace
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
QPainterPath ground_path(std::span<const QPointF> points) {
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
    raster_timer_.setInterval(16);
    connect(&raster_timer_, &QTimer::timeout, this, [this] { poll_raster(); });
    zoom_animation_.setDuration(120);
    zoom_animation_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&zoom_animation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
                apply_zoom(value.toDouble(), zoom_anchor_, zoom_location_);
            });
    connect(&zoom_animation_, &QVariantAnimation::finished, this, [this] { update(); });
    setMinimumSize(160, 100);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName(ui::text("ground_map.label"));
    const auto navigation_help = ui::text("ground_map.navigation_help");
    setAccessibleDescription(navigation_help);
    setToolTip(navigation_help);
}
GroundMap::~GroundMap() {
    raster_timer_.stop();
    if (cancel_raster_)
        cancel_raster_->store(true);
    if (pending_raster_.valid())
        pending_raster_.wait();
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
    invalidate_tracks();
}
void GroundMap::replace_track(std::size_t index, Track track) {
    tracks_.at(index) = std::move(track);
    invalidate_tracks();
}
void GroundMap::append_track(Track track) {
    tracks_.push_back(std::move(track));
    invalidate_tracks();
}
void GroundMap::set_name(std::size_t index, const QString &name) {
    tracks_.at(index).name = name;
    if (index < points_.size())
        points_[index].name = name;
    update();
}
void GroundMap::set_snapshot(const visualization::RenderSnapshot &snapshot) {
    if (snapshot.frame != core::FrameId::itrf || snapshot.origin != core::OriginId::earth)
        throw std::invalid_argument("2D map requires an Earth/ITRF snapshot");
    std::vector<Point> points;
    for (std::size_t i = 0; i < snapshot.markers.size(); ++i) {
        const auto &marker = snapshot.markers[i];
        const auto station = station_names_.find(marker.object_id);
        points.push_back({station != station_names_.end() ? station->second
                          : i < tracks_.size()            ? tracks_[i].name
                                               : QString::fromStdString(marker.object_id),
                          ground_coordinates(marker.position_m), station != station_names_.end()});
    }
    epoch_ = snapshot.epoch;
    if (points_ == points)
        return;
    points_ = std::move(points);
    update();
}
void GroundMap::set_ground_stations(
    const std::vector<application::ProjectGroundStation> &stations) {
    station_names_.clear();
    for (const auto &station : stations)
        station_names_.emplace(station.object_id, QString::fromStdString(station.name()));
}
void GroundMap::set_selection(std::optional<std::size_t> index) {
    if (selected_ == index)
        return;
    selected_ = index;
    update();
}
void GroundMap::set_display(std::size_t index, const DisplaySettings &settings, core::Epoch first,
                            core::Epoch last) {
    auto &track = tracks_.at(index);
    if (track.visible != settings.track_2d || track.display.line_color != settings.line_color ||
        track.display.line_style != settings.line_style ||
        track.display.line_width != settings.line_width)
        invalidate_tracks();
    if (track.label != settings.labels_2d)
        update();
    track.visible = settings.track_2d;
    track.label = settings.labels_2d;
    track.display = settings;
    if (first == last || first.tai_nanoseconds() > track.epochs.back().tai_nanoseconds() ||
        last.tai_nanoseconds() < track.epochs.front().tai_nanoseconds()) {
        if (track.last != 0) {
            track.first = track.last = 0;
            track.path = {};
            invalidate_tracks();
        }
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
        track.path = ground_path(std::span<const QPointF>(track.points).subspan(a, b - a));
        invalidate_tracks();
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
    night_dirty_ = true;
    if (lighting_ || show_sun_) {
        background_dirty_ = true;
        update();
    }
}
void GroundMap::rebuild_night() {
    const ProfileScope profile("map_night_ms");
    night_ = ground_lighting_mask(sun_direction_);
    night_dirty_ = false;
}
QImage ground_lighting_mask(const std::array<double, 3> &direction) {
    const auto length = std::hypot(direction[0], direction[1], direction[2]);
    if (!std::isfinite(length) || std::abs(length - 1) > 1e-6)
        throw std::invalid_argument("Map sunlight requires a finite unit vector");
    QImage image(1440, 720, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        throw std::bad_alloc();
    std::array<double, 1440> longitude_component;
    for (int x = 0; x < image.width(); ++x) {
        const double lon = ((x + 0.5) / image.width() * 360 - 180) * std::numbers::pi / 180;
        longitude_component[x] = std::cos(lon) * direction[0] + std::sin(lon) * direction[1];
    }
    struct Run {
        int first, last;
        bool ascending;
    };
    std::vector<Run> runs;
    int first = 0;
    bool ascending = longitude_component[1] >= longitude_component[0];
    for (int x = 2; x < image.width(); ++x) {
        const bool next = longitude_component[x] >= longitude_component[x - 1];
        if (next != ascending) {
            runs.push_back({first, x, ascending});
            first = x;
            ascending = next;
        }
    }
    runs.push_back({first, image.width(), ascending});
    for (int y = 0; y < image.height(); ++y) {
        const double lat = (90 - (y + 0.5) / image.height() * 180) * std::numbers::pi / 180;
        const double cos_lat = std::cos(lat), axial = std::sin(lat) * direction[2];
        auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (const auto &run : runs) {
            const auto begin = longitude_component.begin() + run.first;
            const auto end = longitude_component.begin() + run.last;
            // Longitude is monotone inside each run. Find the two twilight edges
            // with the original cosine expression, then bulk-fill constant areas.
            // No angular approximation or reduced-resolution mask is introduced.
            const auto left = std::partition_point(begin, end, [&](double value) {
                const double cosine = cos_lat * value + axial;
                return run.ascending ? cosine <= -0.003 : cosine >= 0.003;
            });
            const auto right = std::partition_point(left, end, [&](double value) {
                const double cosine = cos_lat * value + axial;
                return run.ascending ? cosine < 0.003 : cosine > -0.003;
            });
            const auto from = left - longitude_component.begin();
            const auto to = right - longitude_component.begin();
            std::fill(pixels + run.first, pixels + from,
                      run.ascending ? qRgb(42, 64, 90) : qRgb(255, 255, 255));
            std::fill(pixels + to, pixels + run.last,
                      run.ascending ? qRgb(255, 255, 255) : qRgb(42, 64, 90));
            for (auto x = from; x < to; ++x) {
                const double cosine = cos_lat * longitude_component[x] + axial;
                const double t = std::clamp(0.5 - cosine / 0.006, 0.0, 1.0);
                const double night = t * t * (3 - 2 * t);
                pixels[x] = qRgb(qRound(255 - 213 * night), qRound(255 - 191 * night),
                                 qRound(255 - 165 * night));
            }
        }
    }
    return image;
}
void GroundMap::set_lighting(bool enabled) {
    if (lighting_ == enabled)
        return;
    lighting_ = enabled;
    background_dirty_ = true;
    update();
}
void GroundMap::set_grid_visible(bool enabled) {
    if (grid_ == enabled)
        return;
    grid_ = enabled;
    update();
}
void GroundMap::set_tracks_visible(bool enabled) {
    if (show_tracks_ == enabled)
        return;
    show_tracks_ = enabled;
    invalidate_tracks();
}
void GroundMap::set_labels_visible(bool enabled) {
    labels_ = enabled;
    update();
}
void GroundMap::set_sun_visible(bool enabled) {
    if (show_sun_ == enabled)
        return;
    show_sun_ = enabled;
    background_dirty_ = true;
    update();
}
void GroundMap::focus_selection() {
    if (!selected_ || *selected_ >= points_.size())
        return;
    zoom(std::max(1.0, 2.0 / zoom_), plot_rect().center());
    pan_by(plot_rect().center() - marker_position(*selected_));
}
QRectF GroundMap::plot_rect() const { return QRectF(rect()).adjusted(38, 14, -16, -44); }
QRectF GroundMap::map_rect() const {
    const auto plot = plot_rect();
    const double width = std::min(plot.width(), plot.height() * 2) * zoom_;
    const QSizeF size(width, width / 2);
    return {plot.center() + pan_ * width - QPointF(size.width() / 2, size.height() / 2), size};
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
    const auto x = std::max(0.0, (map.width() - plot.width()) / (2 * map.width()));
    const auto y = std::max(0.0, (map.height() - plot.height()) / (2 * map.width()));
    pan_.setX(std::clamp(pan_.x(), -x, x));
    pan_.setY(std::clamp(pan_.y(), -y, y));
}
void GroundMap::reset_view() {
    zoom_animation_.stop();
    finish_drag();
    zoom_ = 1;
    pan_ = {};
    cancel_obsolete_raster();
    update();
}
void GroundMap::zoom(double factor, QPointF anchor) {
    if (!std::isfinite(factor) || factor <= 0 || !std::isfinite(anchor.x()) ||
        !std::isfinite(anchor.y()))
        return;
    zoom_animation_.stop();
    finish_drag();
    apply_zoom(std::clamp(zoom_ * factor, 1.0, 12.0), anchor);
}
void GroundMap::zoom_centered(double factor) { request_zoom(factor, plot_rect().center()); }
void GroundMap::apply_zoom(double next, QPointF anchor, std::optional<QPointF> location) {
    if (next == zoom_)
        return;
    const auto plot = plot_rect();
    const auto offset = anchor - plot.center();
    const auto width = map_rect().width();
    const auto fixed_location = location.value_or(offset / width - pan_);
    const auto next_width = std::min(plot.width(), plot.height() * 2) * next;
    pan_ = offset / next_width - fixed_location;
    zoom_ = next;
    clamp_pan();
    imagery_raster_ = {};
    cancel_obsolete_raster();
    update();
}
void GroundMap::request_zoom(double factor, QPointF anchor) {
    if (!std::isfinite(factor) || factor <= 0 || factor == 1)
        return;
    finish_drag();
    double base = zoom_;
    bool continuing = false;
    if (zoom_animation_.state() == QAbstractAnimation::Running) {
        const auto target = zoom_animation_.endValue().toDouble();
        // Reverse from the visible scale immediately, without finishing old intent.
        if ((factor > 1) == (target > zoom_)) {
            base = target;
            continuing = true;
        }
    }
    zoom_animation_.stop();
    const auto target = std::clamp(base * factor, 1.0, 12.0);
    if (target == zoom_)
        return;
    zoom_anchor_ = anchor;
    zoom_location_ = (anchor - plot_rect().center()) / map_rect().width() - pan_;
    // A new gesture or reversal responds immediately. During a burst, let the
    // next animation tick consume the target without a second paint in that frame.
    if (!continuing)
        apply_zoom(zoom_ + (target - zoom_) * 0.25, anchor, zoom_location_);
    {
        const QSignalBlocker blocker(&zoom_animation_);
        zoom_animation_.setStartValue(zoom_);
        zoom_animation_.setEndValue(target);
        zoom_animation_.setCurrentTime(0);
    }
    zoom_animation_.start();
}
void GroundMap::resizeEvent(QResizeEvent *) {
    if (navigation_size_ == size())
        return;
    navigation_size_ = size();
    zoom_animation_.stop();
    finish_drag();
    clamp_pan();
    imagery_raster_ = {};
    cancel_obsolete_raster();
}
void GroundMap::pan_by(QPointF delta) {
    const auto before = pan_;
    pan_ += delta / map_rect().width();
    clamp_pan();
    if (pan_ == before)
        return;
    imagery_raster_ = {};
    cancel_obsolete_raster();
    update();
}
void GroundMap::invalidate_tracks(bool geometry_changed) {
    ++trails_revision_;
    if (geometry_changed)
        ++geometry_revision_;
    trails_dirty_ = true;
    cancel_obsolete_raster();
    update();
}
void GroundMap::paint_background(QPainter &p, const QRectF &bounds, bool cache_imagery) {
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const auto area = map_rect();
    const auto transform = map_transform();
    p.save();
    p.setClipRect(bounds);
    const auto paint_imagery = [&](QPainter &target) {
        target.setRenderHint(QPainter::SmoothPixmapTransform);
        target.fillRect(bounds, theme::color(theme::chrome));
        target.drawImage(area, imagery_);
        target.fillRect(area, QColor(4, 12, 24, 35));
    };
    if (cache_imagery) {
        // Sunlight changes while the viewport stays still. Keep its expensive
        // imagery resampling separate, at the exact device-pixel sampling phase.
        // A single viewport bounds memory; navigation draws directly until settled.
        const auto ratio = devicePixelRatioF();
        if (imagery_raster_.image.isNull() || imagery_raster_.transform != transform ||
            imagery_raster_.bounds != bounds || imagery_raster_.ratio != ratio) {
            const ProfileScope sample("map_imagery_ms");
            imagery_raster_ = raster_request(96);
            allocate_raster(imagery_raster_, QImage::Format_RGB32);
            QPainter cache(&imagery_raster_.image);
            cache.setRenderHint(QPainter::Antialiasing);
            cache.translate(-bounds.topLeft());
            cache.setClipRect(bounds);
            paint_imagery(cache);
        }
        p.drawImage(bounds.topLeft(), imagery_raster_.image);
    } else {
        imagery_raster_ = {};
        paint_imagery(p);
    }
    if (lighting_) {
        if (night_dirty_)
            rebuild_night();
        p.setCompositionMode(QPainter::CompositionMode_Multiply);
        p.drawImage(area, night_);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    if (show_sun_) {
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
    p.restore();
}
void GroundMap::paint_grid(QPainter &p) {
    if (!grid_)
        return;
    const auto area = map_rect();
    const auto transform = map_transform();
    const int grid_step = area.width() > 6000   ? 5
                          : area.width() > 3000 ? 10
                          : area.width() > 1600 ? 15
                                                : 30;
    p.setPen(QPen(theme::color(theme::accent, 60), 0.7, Qt::DotLine));
    for (int lon = -180; lon <= 180; lon += grid_step)
        p.drawLine(transform.map(QPointF(lon, -90)), transform.map(QPointF(lon, 90)));
    for (int lat = -90; lat <= 90; lat += grid_step)
        p.drawLine(transform.map(QPointF(-180, lat)), transform.map(QPointF(180, lat)));
}
void GroundMap::paint_axes(QPainter &p) {
    const auto plot = plot_rect(), area = map_rect();
    const auto transform = map_transform();
    const int grid_step = area.width() > 6000   ? 5
                          : area.width() > 3000 ? 10
                          : area.width() > 1600 ? 15
                                                : 30;
    p.setPen(theme::color(theme::text_muted));
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
}
std::vector<GroundMap::Stroke> GroundMap::strokes() const {
    std::vector<Stroke> result;
    for (std::size_t i = 0; show_tracks_ && i < tracks_.size(); ++i)
        if (tracks_[i].visible)
            result.push_back({tracks_[i].path, QColor::fromRgb(tracks_[i].display.line_color),
                              tracks_[i].display.line_width, tracks_[i].display.line_style});
    return result;
}
GroundMap::Raster GroundMap::rasterize(Raster result, const std::vector<Stroke> &strokes,
                                       const std::atomic_bool &cancelled) {
    QElapsedTimer elapsed;
    elapsed.start();
    if (cancelled.load()) {
        result.complete = false;
        return result;
    }
    allocate_raster(result, QImage::Format_ARGB32_Premultiplied);
    result.image.fill(Qt::transparent);
    QPainter p(&result.image);
    p.translate(-result.bounds.topLeft());
    p.setClipRect(result.bounds);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    for (const auto &stroke : strokes) {
        if (cancelled.load()) {
            result.complete = false;
            break;
        }
        const auto path = result.transform.map(stroke.path);
        if (result.preview) {
            QPen pen = track_pen(stroke.color, stroke.width, stroke.style);
            pen.setCosmetic(true);
            p.setPen(pen);
        } else {
            auto halo = track_pen(theme::color(theme::canvas, 140), stroke.width, stroke.style);
            const auto dash = halo.dashPattern();
            halo.setWidthF(stroke.width + 1.5);
            if (stroke.style != application::LineStyle::solid) {
                auto pattern = dash;
                for (auto &length : pattern)
                    length *= stroke.width / halo.widthF();
                halo.setDashPattern(pattern);
            }
            p.setPen(halo);
            if (!draw_track(p, path, &cancelled)) {
                result.complete = false;
                break;
            }
            p.setPen(track_pen(stroke.color, stroke.width, stroke.style));
        }
        if (!draw_track(p, path, &cancelled)) {
            result.complete = false;
            break;
        }
    }
    p.end();
    result.milliseconds = elapsed.nsecsElapsed() * 1e-6;
    return result;
}
bool GroundMap::tracks_ready() const {
    return !trails_dirty_ && !trails_.image.isNull() && covers_view(trails_);
}
GroundMap::Raster GroundMap::raster_request(double margin) const {
    Raster request;
    request.ratio = devicePixelRatioF();
    request.bounds = plot_rect().adjusted(-margin, -margin, margin, margin);
    request.bounds.moveLeft(std::floor(request.bounds.left() * request.ratio) / request.ratio);
    request.bounds.moveTop(std::floor(request.bounds.top() * request.ratio) / request.ratio);
    request.bounds.setSize(QSizeF(qCeil(request.bounds.width() * request.ratio) / request.ratio,
                                  qCeil(request.bounds.height() * request.ratio) / request.ratio));
    request.transform = map_transform();
    request.revision = trails_revision_;
    return request;
}
GroundMap::Raster GroundMap::preview_request() const {
    Raster request;
    // A bounded whole-map image covers every viewport, including zooming out and
    // crossing a cached viewport edge. It is only a transient visual fallback.
    request.bounds = QRectF(0, 0, 2048, 1024);
    request.transform = QTransform(2048.0 / 360, 0, 0, -1024.0 / 180, 1024, 512);
    request.revision = geometry_revision_;
    request.preview = true;
    return request;
}
void GroundMap::allocate_raster(Raster &raster, QImage::Format format) {
    // Request bounds have already been aligned to physical pixels. A second ceil
    // can add a pixel due to floating-point roundoff at fractional DPI.
    raster.image = QImage(QSize(qRound(raster.bounds.width() * raster.ratio),
                                qRound(raster.bounds.height() * raster.ratio)),
                          format);
    if (raster.image.isNull())
        throw std::bad_alloc();
    raster.image.setDevicePixelRatio(raster.ratio);
}
QTransform GroundMap::raster_transform(const Raster &raster) const {
    const auto current = map_transform();
    const auto scale = current.m11() / raster.transform.m11();
    return QTransform(scale, 0, 0, scale, current.dx() - scale * raster.transform.dx(),
                      current.dy() - scale * raster.transform.dy());
}
bool GroundMap::covers_view(const Raster &raster, bool exact_scale) const {
    if (raster.bounds.isEmpty() || raster.ratio != devicePixelRatioF())
        return false;
    const auto transform = raster_transform(raster);
    if (exact_scale ? transform.m11() != 1 : transform.m11() < 0.5 || transform.m11() > 2)
        return false;
    if (exact_scale) {
        // Fractional device-pixel pans may reuse a preview, but the settled image
        // must be rasterized at its current phase rather than remain resampled.
        for (const auto offset : {transform.dx(), transform.dy()}) {
            const auto pixels = offset * raster.ratio;
            if (std::abs(pixels - std::round(pixels)) > 1e-6)
                return false;
        }
    }
    return transform.mapRect(raster.bounds).contains(plot_rect());
}
void GroundMap::cancel_obsolete_raster() {
    if (cancel_raster_ && (pending_view_.preview ? pending_view_.revision != geometry_revision_
                                                 : pending_view_.revision != trails_revision_ ||
                                                       !covers_view(pending_view_)))
        cancel_raster_->store(true, std::memory_order_relaxed);
}
void GroundMap::install_raster(Raster result) {
    if (result.preview) {
        if (result.complete && result.revision == geometry_revision_)
            preview_ = std::move(result);
        return;
    }
    if (!result.complete || result.revision != trails_revision_ || !covers_view(result))
        return;
    trails_ = std::move(result);
    trails_dirty_ = false;
}
void GroundMap::poll_raster() {
    if (!pending_raster_.valid() ||
        pending_raster_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    try {
        auto result = pending_raster_.get();
        if (WorkbenchProfile::active)
            WorkbenchProfile::active->record(!result.complete ? "map_tracks_cancelled_ms"
                                             : result.preview ? "map_preview_worker_ms"
                                                              : "map_tracks_worker_ms",
                                             result.milliseconds);
        install_raster(std::move(result));
    } catch (const std::exception &) {
        // Keep any valid cached image and allow a later paint to retry.
    }
    raster_timer_.stop();
    cancel_raster_.reset();
    update();
}
void GroundMap::set_access_link(std::optional<std::pair<std::size_t, std::size_t>> pair,
                                bool visible) {
    if (access_pair_ == pair && access_visible_ == visible)
        return;
    access_pair_ = pair;
    access_visible_ = visible;
    update();
}
void GroundMap::paint_base(QPainter &p, const Raster *display) {
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), theme::color(theme::chrome));
    p.save();
    p.setClipRect(plot_rect());
    const auto draw = [&](const Raster &raster) {
        const auto transform = raster_transform(raster);
        const auto bounds = transform.mapRect(raster.bounds);
        // Preserve the native pixel size when a cached layer only translates.
        if (qFuzzyCompare(transform.m11(), 1.0))
            p.drawImage(bounds.topLeft(), raster.image);
        else
            p.drawImage(bounds, raster.image);
    };
    draw(background_);
    paint_grid(p);
    if (display)
        draw(*display);
    p.restore();
    paint_axes(p);
}
void GroundMap::paintEvent(QPaintEvent *event) {
    const ProfileScope profile("map_paint_ms");
    const auto plot = plot_rect();
    const bool footer_only = event->rect().top() >= height() - 24;
    const bool navigating =
        (dragging_ && moved_) || zoom_animation_.state() == QAbstractAnimation::Running;
    if (!footer_only && (background_dirty_ || background_.image.isNull() ||
                         !covers_view(background_, !navigating))) {
        const ProfileScope rebuild("map_background_ms");
        background_ = raster_request(96);
        allocate_raster(background_, QImage::Format_RGB32);
        QPainter cache(&background_.image);
        cache.translate(-background_.bounds.topLeft());
        paint_background(cache, background_.bounds, !navigating);
        background_dirty_ = false;
    }
    // One immutable worker request at a time; never queue intermediate viewports.
    // Heavy rasterization must not hold the shared Qt input / Vulkan frame thread.
    cancel_obsolete_raster();
    poll_raster();
    if (!footer_only && !tracks_ready() && !pending_raster_.valid()) {
        const auto paths = strokes();
        std::size_t vertices = 0;
        for (const auto &stroke : paths)
            vertices += stroke.path.elementCount();
        auto request = raster_request();
        if (vertices > 1024) {
            const bool needs_preview =
                preview_.image.isNull() || preview_.revision != geometry_revision_;
            // Continuous input reprojects immutable coverage. Build full detail
            // once input stops, instead of cancelling a new worker every frame.
            if (!navigating || needs_preview) {
                if (needs_preview)
                    request = preview_request();
                pending_view_ = request;
                cancel_raster_ = std::make_shared<std::atomic_bool>(false);
                pending_raster_ =
                    std::async(std::launch::async, [request = std::move(request), paths,
                                                    cancel = cancel_raster_]() mutable {
                        return rasterize(std::move(request), paths, *cancel);
                    });
                raster_timer_.start();
            }
        } else {
            const ProfileScope rebuild("map_tracks_ms");
            const std::atomic_bool cancelled{false};
            install_raster(rasterize(std::move(request), paths, cancelled));
        }
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (footer_only)
        p.fillRect(event->rect(), theme::color(theme::chrome));
    else {
        // Reproject a complete cached image during zooming, only while its coverage
        // includes the whole viewport. A new exact-scale image still replaces it.
        const Raster *display = nullptr;
        if (!trails_dirty_ && !trails_.image.isNull() && covers_view(trails_, false))
            display = &trails_;
        else if (!preview_.image.isNull() && preview_.revision == geometry_revision_)
            display = &preview_;
        const BaseKey key{map_transform(),
                          size(),
                          devicePixelRatioF(),
                          background_.image.cacheKey(),
                          display ? display->image.cacheKey() : 0,
                          grid_,
                          font()};
        const bool stable_view = key.transform == base_key_.transform &&
                                 key.size == base_key_.size && key.ratio == base_key_.ratio;
        if (!stable_view)
            view_stable_.start();
        if (!stable_view || !view_stable_.isValid() || view_stable_.elapsed() < 100) {
            // During dragging/zooming draw directly; avoid allocating an extra image
            // for a viewport that may only be shown once.
            base_image_ = {};
            paint_base(p, display);
        } else {
            if (base_image_.isNull() || key != base_key_) {
                const ProfileScope compose("map_composite_ms");
                const QSize pixels(static_cast<int>(std::ceil(width() * key.ratio)),
                                   static_cast<int>(std::ceil(height() * key.ratio)));
                if (base_image_.size() != pixels) {
                    base_image_ = QImage(pixels, QImage::Format_RGB32);
                    base_image_.setDevicePixelRatio(key.ratio);
                }
                QPainter cache(&base_image_);
                cache.setFont(font());
                paint_base(cache, display);
            }
            p.drawImage(QPointF(0, 0), base_image_);
        }
        base_key_ = key;
        p.setFont(font());
        p.save();
        p.setClipRect(plot);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        const auto label_context = font().toString() + '/' + QString::number(devicePixelRatioF());
        if (access_visible_ && access_pair_ && access_pair_->first < points_.size() &&
            access_pair_->second < points_.size()) {
            p.setPen(QPen(theme::color(theme::success), 2, Qt::DashLine));
            p.drawPath(
                map_transform().map(ground_path({points_[access_pair_->first].geographic,
                                                 points_[access_pair_->second].geographic})));
        }
        for (std::size_t i = 0; i < points_.size(); ++i) {
            const auto point = marker_position(i);
            if (!plot.contains(point))
                continue;
            p.setPen(QPen(theme::color(theme::canvas), 1.5));
            p.setBrush(selected_ == i              ? theme::color(theme::sakura)
                       : points_[i].ground_station ? theme::color(theme::success)
                       : i < tracks_.size()        ? QColor::fromRgb(tracks_[i].display.line_color)
                                                   : theme::color(theme::accent_bright));
            if (points_[i].ground_station)
                p.drawPolygon(QPolygonF{point + QPointF(0, -6), point + QPointF(6, 0),
                                        point + QPointF(0, 6), point + QPointF(-6, 0)});
            else
                p.drawEllipse(point, 4, 4);
            if (selected_ == i) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(theme::color(theme::sakura), 1));
                p.drawEllipse(point, 8, 8);
            }
            if (!labels_ || (i < tracks_.size() && !tracks_[i].label))
                continue;
            const auto name = fontMetrics().elidedText(points_[i].name, Qt::ElideRight,
                                                       static_cast<int>(plot.width()) - 12);
            const auto label_width = fontMetrics().horizontalAdvance(name) + 12;
            const QRectF label(std::clamp(point.x() + 12, plot.left(), plot.right() - label_width),
                               std::clamp(point.y() - 12, plot.top(), plot.bottom() - 22),
                               label_width, 22);
            const auto ratio = devicePixelRatioF();
            // Resolve half-pixel ties consistently after repeated anchored zooms.
            const auto label_pixel = [ratio](double value) {
                return std::floor(value * ratio + 0.5 + 1e-7) / ratio;
            };
            const QPointF label_origin(label_pixel(label.x()), label_pixel(label.y()));
            const auto label_key = label_context + '/' + name;
            auto *image = labels_cache_.object(label_key);
            if (!image) {
                auto prepared = std::make_unique<QImage>(
                    QSize(static_cast<int>(std::ceil(label.width() * ratio)),
                          static_cast<int>(std::ceil(label.height() * ratio))),
                    QImage::Format_ARGB32_Premultiplied);
                prepared->setDevicePixelRatio(ratio);
                prepared->fill(Qt::transparent);
                {
                    QPainter text(prepared.get());
                    text.setRenderHint(QPainter::Antialiasing);
                    text.setFont(font());
                    text.setPen(Qt::NoPen);
                    text.setBrush(theme::color(theme::chrome, 220));
                    const QRectF bounds(QPointF(0, 0), label.size());
                    text.drawRoundedRect(bounds, 3, 3);
                    text.setPen(theme::color(theme::text));
                    text.drawText(bounds, Qt::AlignCenter, name);
                }
                image = prepared.get();
                const auto cost = static_cast<int>((image->sizeInBytes() + 1023) / 1024);
                // An unusually wide label can exceed the cache; still draw it once.
                p.drawImage(label_origin, *image);
                labels_cache_.insert(label_key, prepared.release(), cost);
            } else
                p.drawImage(label_origin, *image);
        }
        p.restore();
    }
    p.setFont(font());
    p.setPen(theme::color(theme::text_muted));
    QString caption = ui::text("ground_map.caption");
    if (cursor_position_) {
        const auto ll = map_transform().inverted().map(*cursor_position_);
        caption = ui::text("ground_map.ll")
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
    const auto scale = QStringLiteral("%1×").arg(zoom_, 0, 'f', 2);
    const int scale_width = fontMetrics().horizontalAdvance(QStringLiteral("12.00×"));
    const int caption_width = std::max(0, width() - 48 - scale_width);
    p.drawText(QRectF(16, height() - 22, caption_width, 18), Qt::AlignVCenter,
               fontMetrics().elidedText(caption, Qt::ElideRight, caption_width));
    p.drawText(QRectF(width() - 16 - scale_width, height() - 22, scale_width, 18),
               Qt::AlignVCenter | Qt::AlignRight, scale);
}
void GroundMap::mousePressEvent(QMouseEvent *event) {
    if ((event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton) ||
        !plot_rect().intersected(map_rect()).contains(event->position())) {
        event->ignore();
        return;
    }
    setFocus(Qt::MouseFocusReason);
    zoom_animation_.stop();
    update();
    if (dragging_)
        return;
    dragging_ = true;
    moved_ = false;
    drag_button_ = event->button();
    press_pan_ = pan_;
    press_ = last_mouse_ = event->position();
    event->accept();
}
void GroundMap::mouseMoveEvent(QMouseEvent *event) {
    cursor_position_ = plot_rect().intersected(map_rect()).contains(event->position())
                           ? std::optional<QPointF>(event->position())
                           : std::nullopt;
    update(QRect(0, height() - 24, width(), 24));
    if (dragging_)
        drag_to(event->position());
    else if (cursor_position_ && zoom_ > 1)
        setCursor(Qt::OpenHandCursor);
    else
        unsetCursor();
}
void GroundMap::drag_to(QPointF position) {
    if (!moved_ && (position - press_).manhattanLength() >= QApplication::startDragDistance()) {
        moved_ = true;
        setCursor(Qt::ClosedHandCursor);
    }
    if (moved_) {
        // Keep the full press-to-drag displacement; after reaching an edge, reverse
        // immediately rather than making the pointer travel back through overshoot.
        pan_by(position - last_mouse_);
        last_mouse_ = position;
    }
}
void GroundMap::finish_drag(bool restore) {
    if (!dragging_)
        return;
    dragging_ = false;
    drag_button_ = Qt::NoButton;
    unsetCursor();
    if (restore) {
        pan_ = press_pan_;
        clamp_pan();
        cancel_obsolete_raster();
        update();
    }
    if (moved_)
        update(); // Restore exact device-pixel sampling after the last drag frame.
}
void GroundMap::leaveEvent(QEvent *) {
    cursor_position_.reset();
    update(QRect(0, height() - 24, width(), 24));
}
void GroundMap::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != drag_button_ || !dragging_) {
        event->ignore();
        return;
    }
    drag_to(event->position());
    const bool pick = event->button() == Qt::LeftButton && !moved_ &&
                      plot_rect().intersected(map_rect()).contains(event->position());
    finish_drag();
    if (pick && select) {
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
    event->accept();
}
void GroundMap::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton ||
        !plot_rect().intersected(map_rect()).contains(event->position())) {
        event->ignore();
        return;
    }
    request_zoom(event->modifiers().testFlag(Qt::ShiftModifier) ? 0.5 : 2.0, event->position());
    event->accept();
}
void GroundMap::wheelEvent(QWheelEvent *event) {
    if (!plot_rect().intersected(map_rect()).contains(event->position()) || dragging_) {
        event->ignore();
        return;
    }
    const bool pixel_scroll = !event->pixelDelta().isNull();
    const auto delta = pixel_scroll ? event->pixelDelta().y() : event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    const auto factor = std::pow(1.2, std::clamp(delta / (pixel_scroll ? 60.0 : 120.0), -8.0, 8.0));
    if (pixel_scroll)
        zoom(factor, event->position());
    else
        request_zoom(factor, event->position());
    event->accept();
}
void GroundMap::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Home)
        reset_view();
    else if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal)
        zoom_centered(1.3);
    else if (event->key() == Qt::Key_Minus)
        zoom_centered(1 / 1.3);
    else if (event->key() == Qt::Key_Escape) {
        zoom_animation_.stop();
        finish_drag(true);
        update();
    } else if (event->key() >= Qt::Key_Left && event->key() <= Qt::Key_Down) {
        zoom_animation_.stop();
        finish_drag();
        update();
        const auto plot = plot_rect();
        const double fraction = event->modifiers().testFlag(Qt::ShiftModifier) ? 0.02 : 0.1;
        switch (event->key()) {
        case Qt::Key_Left:
            pan_by({plot.width() * fraction, 0});
            break;
        case Qt::Key_Right:
            pan_by({-plot.width() * fraction, 0});
            break;
        case Qt::Key_Up:
            pan_by({0, plot.height() * fraction});
            break;
        case Qt::Key_Down:
            pan_by({0, -plot.height() * fraction});
            break;
        }
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}
bool GroundMap::event(QEvent *event) {
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide ||
        event->type() == QEvent::WindowDeactivate) {
        zoom_animation_.stop();
        finish_drag();
        update();
    }
    if (event->type() == QEvent::UngrabMouse)
        finish_drag();
    return QWidget::event(event);
}
} // namespace orbitforge::desktop
