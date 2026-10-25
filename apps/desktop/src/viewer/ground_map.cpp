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

namespace orbelle::desktop {
namespace {
constexpr double maximum_zoom = 1024;
constexpr double preview_width = 2048;
double axis_step(double pixels_per_degree, double spacing) {
    for (const double step : {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,  1.,
                              2.,    5.,    10.,   15.,  30.,  45.,  60., 90., 180., 360.})
        if (step * pixels_per_degree >= spacing)
            return step;
    return 360;
}
QFont axis_font() {
    QFont font("Consolas");
    font.setPixelSize(10);
    return font;
}
QPointF axis_steps(double pixels_per_degree, const QSizeF &viewport) {
    const QFontMetrics metrics(axis_font());
    // Non-overlapping labels can still crowd a large view. Bound the number of
    // intervals as well as the text spacing, all in Qt logical pixels.
    const auto horizontal =
        std::max({112.0, viewport.width() / 7, metrics.horizontalAdvance("180.000 W") + 32.0});
    const auto vertical = std::max({64.0, viewport.height() / 5, metrics.height() * 4.0});
    return {axis_step(pixels_per_degree, horizontal), axis_step(pixels_per_degree, vertical)};
}
double wrap(double value, double period) {
    return value - period * std::floor(value / period + 0.5);
}
QRectF world_rect(const QTransform &transform) {
    return transform.mapRect(QRectF(-180, -90, 360, 180));
}
// Enumerate only the longitude copies intersecting a bounded viewport. Latitude
// never repeats. No bitmap or cache grows with the number of revolutions.
template <typename Draw>
void each_world(const QTransform &transform, const QRectF &bounds, Draw draw) {
    const auto world = world_rect(transform);
    if (world.width() <= 0 || bounds.bottom() <= world.top() || bounds.top() >= world.bottom())
        return;
    const int first = static_cast<int>(std::floor((bounds.left() - world.right()) / world.width()));
    const int last = static_cast<int>(std::ceil((bounds.right() - world.left()) / world.width()));
    for (int index = first; index <= last; ++index) {
        const auto offset = index * world.width();
        const auto area = world.translated(offset, 0);
        if (!bounds.intersects(area))
            continue;
        draw(QTransform(transform.m11(), 0, 0, transform.m22(), transform.dx() + offset,
                        transform.dy()),
             area);
    }
}
void draw_world_image(QPainter &p, const QImage &image, const QRectF &area, const QRectF &bounds) {
    const auto target = area.intersected(bounds);
    if (target.isEmpty())
        return;
    // Crop before drawing: even at maximum zoom the raster engine sees only a
    // viewport-sized target, not a million-pixel world rectangle.
    const QRectF source((target.left() - area.left()) * image.width() / area.width(),
                        (target.top() - area.top()) * image.height() / area.height(),
                        target.width() * image.width() / area.width(),
                        target.height() * image.height() / area.height());
    p.drawImage(target, image, source);
}
bool clip_line(QLineF &line, const QRectF &bounds, double &first) {
    const auto start = line.p1(), delta = line.p2() - start;
    first = 0;
    double last = 1;
    const auto edge = [&](double direction, double distance) {
        if (direction == 0)
            return distance >= 0;
        const auto fraction = distance / direction;
        if (direction < 0)
            first = std::max(first, fraction);
        else
            last = std::min(last, fraction);
        return first <= last;
    };
    if (!edge(-delta.x(), start.x() - bounds.left()) ||
        !edge(delta.x(), bounds.right() - start.x()) ||
        !edge(-delta.y(), start.y() - bounds.top()) ||
        !edge(delta.y(), bounds.bottom() - start.y()))
        return false;
    line = QLineF(start + delta * first, start + delta * last);
    return true;
}
// Facets have at most eight points, including the closing point. Each clipping
// plane can at most double that count, so two fixed buffers cover all four
// planes without allocating for every visible facet.
using FootprintClipBuffer = std::array<QPointF, 8 * 16>;
std::span<const QPointF> clip_footprint_polygon(FootprintClipBuffer &projected, int count,
                                                const QRectF &bounds,
                                                FootprintClipBuffer &scratch) {
    auto *input = &projected;
    auto *output = &scratch;
    for (int side = 0; side < 4 && count; ++side) {
        const auto distance = [&](QPointF point) {
            switch (side) {
            case 0:
                return point.x() - bounds.left();
            case 1:
                return bounds.right() - point.x();
            case 2:
                return point.y() - bounds.top();
            default:
                return bounds.bottom() - point.y();
            }
        };
        int next_count = 0;
        auto previous = (*input)[count - 1];
        auto before = distance(previous);
        for (int index = 0; index < count; ++index) {
            const auto point = (*input)[index];
            const auto after = distance(point);
            if ((before < 0) != (after < 0))
                output->at(next_count++) =
                    previous + (point - previous) * (before / (before - after));
            if (after >= 0)
                output->at(next_count++) = point;
            previous = point;
            before = after;
        }
        count = next_count;
        std::swap(input, output);
    }
    return {input->data(), static_cast<std::size_t>(count)};
}
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
        QLineF segment(previous, point);
        const auto length = segment.length();
        double first = 0;
        if (!element.isMoveTo() && clip_line(segment, clip, first)) {
            if (dashed) {
                pen.setDashOffset((phase + length * first) / pen.widthF());
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
            phase = std::fmod(phase + length, cycle);
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
GroundMap::GroundMap(const QString &imagery, QWidget *parent)
    : GroundMap(QImage(imagery), parent) {}
GroundMap::GroundMap(QImage imagery, QWidget *parent)
    : QWidget(parent), imagery_(std::move(imagery)), refinement_animation_(this) {
    if (imagery_.isNull())
        throw std::runtime_error("Cannot load offline imagery for the 2D map");
    raster_timer_.setInterval(16);
    connect(&raster_timer_, &QTimer::timeout, this, [this] { poll_raster(); });
    footprint_timer_.setInterval(16);
    connect(&footprint_timer_, &QTimer::timeout, this, [this] { poll_footprints(); });
    pan_settle_timer_.setSingleShot(true);
    pan_settle_timer_.setInterval(120);
    connect(&pan_settle_timer_, &QTimer::timeout, this, [this] { update(); });
    zoom_animation_.setDuration(120);
    zoom_animation_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&zoom_animation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
                apply_zoom(value.toDouble(), zoom_anchor_, zoom_location_);
            });
    connect(&zoom_animation_, &QVariantAnimation::finished, this, [this] { update(); });
    refinement_animation_.setDuration(160);
    refinement_animation_.setObjectName("groundMapRefinement");
    refinement_animation_.setStartValue(0.0);
    refinement_animation_.setEndValue(1.0);
    refinement_animation_.setEasingCurve(QEasingCurve::InOutCubic);
    connect(&refinement_animation_, &QVariantAnimation::valueChanged, this, [this] { update(); });
    connect(&refinement_animation_, &QVariantAnimation::finished, this, [this] {
        previous_detail_ = {};
        blended_detail_ = {};
        update();
    });
    setMinimumSize(160, 100);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName(ui::text("ground_map.label"));
    const auto navigation_help = ui::text("ground_map.navigation_help");
    setAccessibleDescription(navigation_help);
}
GroundMap::~GroundMap() {
    footprint_timer_.stop();
    if (cancel_footprints_)
        cancel_footprints_->store(true);
    if (pending_footprints_.valid())
        pending_footprints_.wait();
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
void GroundMap::refine_tracks(PreparedTracks tracks) {
    auto previous =
        !tracks.empty() && tracks.size() == tracks_.size() && tracks_ready() ? trails_ : Raster{};
    publish_tracks(std::move(tracks));
    if (!previous.image.isNull()) {
        previous_detail_ = std::move(previous);
        refinement_pending_ = true;
    }
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
void GroundMap::set_station_name(std::size_t index, const std::string &id, const QString &name) {
    station_names_.at(id) = name;
    const auto marker = tracks_.size() + index;
    if (marker < points_.size() && points_[marker].ground_station)
        points_[marker].name = name;
    update();
}
namespace {
QPainterPath footprint_path(std::span<const core::SensorTriangle> triangles,
                            const std::atomic_bool *cancelled) {
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    for (const auto &triangle : triangles) {
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            break;
        QPolygonF polygon;
        for (const auto &p : triangle) {
            auto point = ground_coordinates(p);
            if (!polygon.empty()) {
                while (point.x() - polygon.back().x() > 180)
                    point.rx() -= 360;
                while (point.x() - polygon.back().x() < -180)
                    point.rx() += 360;
            }
            polygon.push_back(point);
        }
        auto end = polygon.front();
        while (end.x() - polygon.back().x() > 180)
            end.rx() -= 360;
        while (end.x() - polygon.back().x() < -180)
            end.rx() += 360;
        if (std::abs(end.x() - polygon.front().x()) > 180) {
            polygon.push_back(end);
            const double pole =
                (polygon.front().y() + polygon[1].y() + polygon[2].y()) > 0 ? 90 : -90;
            polygon.push_back({end.x(), pole});
            polygon.push_back({polygon.front().x(), pole});
        }
        // Normalize winding before combining facets into one translucent fill.
        double area = 0;
        for (int i = 0; i < polygon.size(); ++i) {
            const auto x = polygon[i], y = polygon[(i + 1) % polygon.size()];
            area += x.x() * y.y() - y.x() * x.y();
        }
        if (area < 0)
            std::reverse(polygon.begin(), polygon.end());
        for (const double offset : {-720., -360., 0., 360., 720.}) {
            const auto shifted = polygon.translated(offset, 0);
            if (shifted.boundingRect().right() >= -180 && shifted.boundingRect().left() <= 180) {
                path.addPolygon(shifted);
                path.closeSubpath();
            }
        }
    }
    return path;
}
} // namespace
QPainterPath ground_footprint_path(std::span<const core::SensorTriangle> triangles) {
    return footprint_path(triangles, nullptr);
}
void GroundMap::set_sensor_footprints(std::span<const visualization::SensorMesh> meshes,
                                      bool continuous) {
    const ProfileScope profile("map_footprint_input_ms");
    if (meshes.empty() && !footprints_)
        return;
    auto footprints = std::make_shared<std::vector<Footprint>>();
    footprints->reserve(meshes.size());
    // Copy only the immutable display input here. Geographic path conversion can
    // dominate large footprints, so coalesce frames before doing it on the worker.
    for (const auto &mesh : meshes)
        footprints->push_back(
            {mesh.ground, mesh.ground_edges, QColor::fromRgb(mesh.color), mesh.opacity, mesh.fill});
    footprints_ = footprints->empty() ? nullptr : std::move(footprints);
    ++footprints_revision_;
    if (!continuous || !footprints_) {
        // Edits, seeks and removals invalidate even in-flight results. Ordinary
        // playback instead lets its worker finish and then samples the latest input.
        footprints_reset_revision_ = footprints_revision_;
        footprint_raster_ = {};
        footprint_preview_ = {};
        if (cancel_footprints_)
            cancel_footprints_->store(true, std::memory_order_relaxed);
    }
    update();
}
void GroundMap::set_snapshot(const visualization::RenderSnapshot &snapshot) {
    if (snapshot.frame != core::FrameId::itrf || snapshot.origin != core::OriginId::earth)
        throw std::invalid_argument("2D map requires an Earth/ITRF snapshot");
    std::vector<Point> points;
    points.reserve(snapshot.markers.size());
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
    // Fill the viewport at 1x without stretching the equirectangular projection.
    // A short split shows a latitude band instead of a tiny, letterboxed world.
    const double width = std::max(plot.width(), plot.height() * 2) * zoom_;
    const QSizeF size(width, width / 2);
    return {plot.center() + pan_ * width - QPointF(size.width() / 2, size.height() / 2), size};
}
QRectF GroundMap::visible_map_rect() const {
    const auto plot = plot_rect(), area = map_rect();
    return plot.intersected(QRectF(plot.left(), area.top(), plot.width(), area.height()));
}
double GroundMap::minimum_zoom() const {
    const auto plot = plot_rect();
    // Zooming out can still reveal the complete world, including both poles.
    return std::min(plot.width(), 2 * plot.height()) / std::max(plot.width(), 2 * plot.height());
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
    auto point = map_transform().map(marker_coordinates(index));
    point.setX(plot_rect().center().x() +
               wrap(point.x() - plot_rect().center().x(), map_rect().width()));
    return point;
}
void GroundMap::clamp_pan() {
    const auto plot = plot_rect(), map = map_rect();
    const auto y = std::max(0.0, (map.height() - plot.height()) / (2 * map.width()));
    pan_.setX(wrap(pan_.x(), 1));
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
    apply_zoom(std::clamp(zoom_ * factor, minimum_zoom(), maximum_zoom), anchor);
}
void GroundMap::zoom_centered(double factor) { request_zoom(factor, plot_rect().center()); }
void GroundMap::apply_zoom(double next, QPointF anchor, std::optional<QPointF> location) {
    if (next == zoom_)
        return;
    const auto plot = plot_rect();
    const auto offset = anchor - plot.center();
    const auto width = map_rect().width();
    const auto fixed_location = location.value_or(offset / width - pan_);
    const auto next_width = std::max(plot.width(), plot.height() * 2) * next;
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
    const auto target = std::clamp(base * factor, minimum_zoom(), maximum_zoom);
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
    if (dragging_ && moved_)
        pan_settle_timer_.start();
    imagery_raster_ = {};
    cancel_obsolete_raster();
    update();
}
void GroundMap::invalidate_tracks(bool geometry_changed) {
    cancel_refinement();
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
    const auto transform = map_transform();
    p.save();
    p.setClipRect(bounds);
    const auto paint_imagery = [&](QPainter &target) {
        target.setRenderHint(QPainter::SmoothPixmapTransform);
        target.fillRect(bounds, theme::color(theme::chrome));
        each_world(transform, bounds, [&](const auto &, const QRectF &area) {
            draw_world_image(target, imagery_, area, bounds);
            target.fillRect(area.intersected(bounds), QColor(4, 12, 24, 35));
        });
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
        each_world(transform, bounds, [&](const auto &, const QRectF &area) {
            draw_world_image(p, night_, area, bounds);
        });
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    if (show_sun_) {
        p.setPen(QPen(QColor("#efd69b"), 1));
        p.setBrush(QColor("#efd69b"));
        each_world(
            transform, bounds.adjusted(-10, -10, 10, 10), [&](const auto &copy, const auto &) {
                const auto sun = copy.map(QPointF(
                    std::atan2(sun_direction_[1], sun_direction_[0]) * 180 / std::numbers::pi,
                    std::asin(std::clamp(sun_direction_[2], -1.0, 1.0)) * 180 / std::numbers::pi));
                if (!bounds.adjusted(-10, -10, 10, 10).contains(sun))
                    return;
                p.drawEllipse(sun, 3, 3);
                for (int i = 0; i < 8; ++i) {
                    const double angle = i * std::numbers::pi / 4;
                    const QPointF ray(std::cos(angle), std::sin(angle));
                    p.drawLine(sun + ray * 6, sun + ray * 9);
                }
            });
    }
    p.restore();
}
void GroundMap::paint_grid(QPainter &p) {
    const ProfileScope profile("map_grid_ms");
    if (!grid_)
        return;
    const auto area = map_rect();
    const auto transform = map_transform();
    const auto steps = axis_steps(area.width() / 360, visible_map_rect().size());
    auto pen = QPen(theme::color(theme::accent, 60), 0.7, Qt::DotLine);
    // Qt's wide/dashed stroker can process the entire world-length line before
    // applying the clip, especially above DPR 1. Trim it first and preserve the
    // geographic dash phase so dots do not crawl when the viewport moves.
    const auto visible = visible_map_rect().adjusted(-2, 0, 2, 0);
    const auto geographic = transform.inverted().mapRect(visible);
    for (auto tick = std::ceil(geographic.left() / steps.x());
         tick <= std::floor(geographic.right() / steps.x()); ++tick) {
        const auto x = transform.map(QPointF(tick * steps.x(), 0)).x();
        pen.setDashOffset((area.bottom() - visible.bottom()) / pen.widthF());
        p.setPen(pen);
        p.drawLine(QPointF(x, visible.bottom()), QPointF(x, visible.top()));
    }
    for (auto tick = std::ceil(std::max(-90.0, geographic.top()) / steps.y());
         tick <= std::floor(std::min(90.0, geographic.bottom()) / steps.y()); ++tick) {
        const auto y = transform.map(QPointF(0, tick * steps.y())).y();
        pen.setDashOffset((visible.left() - area.left()) / pen.widthF());
        p.setPen(pen);
        p.drawLine(QPointF(visible.left(), y), QPointF(visible.right(), y));
    }
}
void GroundMap::paint_axes(QPainter &p) {
    const ProfileScope profile("map_axes_ms");
    const auto plot = plot_rect(), area = map_rect();
    const auto transform = map_transform();
    p.setPen(theme::color(theme::text_muted));
    p.setFont(axis_font());
    const auto steps = axis_steps(area.width() / 360, visible_map_rect().size());
    const auto longitude_step = steps.x(), latitude_step = steps.y();
    const auto geographic = transform.inverted().mapRect(visible_map_rect());
    const auto number = [](double value, double step) {
        return QString::number(std::abs(value), 'f',
                               std::max(0, static_cast<int>(-std::floor(std::log10(step)))));
    };
    for (auto tick = std::ceil(geographic.left() / longitude_step);
         grid_ && tick <= std::floor(geographic.right() / longitude_step); ++tick) {
        const auto longitude = tick * longitude_step;
        const auto lon = wrap(longitude, 360);
        const double x = transform.map(QPointF(longitude, 0)).x();
        const auto label = number(lon, longitude_step) + (lon < 0 ? " W" : lon > 0 ? " E" : "");
        const auto label_width = p.fontMetrics().horizontalAdvance(label) + 4;
        p.drawText(QRectF(x - label_width / 2., std::min(area.bottom(), plot.bottom()) + 3,
                          label_width, 16),
                   Qt::AlignCenter, label);
    }
    for (auto tick = std::ceil(std::max(-90.0, geographic.top()) / latitude_step);
         grid_ && tick <= std::floor(std::min(90.0, geographic.bottom()) / latitude_step); ++tick) {
        const auto lat = tick * latitude_step;
        const double y = transform.map(QPointF(0, lat)).y();
        // Small-degree labels need more than the fixed world-view left margin.
        const auto label = number(lat, latitude_step) + (lat < 0 ? "S" : lat > 0 ? "N" : "");
        const auto label_width = p.fontMetrics().horizontalAdvance(label) + 4;
        const auto left = std::max(0.0, plot.left() - label_width - 4);
        p.fillRect(QRectF(left, y - 9, label_width, 18), theme::color(theme::chrome, 220));
        p.drawText(QRectF(left, y - 9, label_width, 18), Qt::AlignRight | Qt::AlignVCenter, label);
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
        each_world(result.transform, result.bounds, [&](const auto &transform, const auto &area) {
            if (!result.complete)
                return;
            p.setClipRect(result.bounds.intersected(area));
            const auto path = transform.map(stroke.path);
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
                    return;
                }
                p.setPen(track_pen(stroke.color, stroke.width, stroke.style));
            }
            if (!draw_track(p, path, &cancelled)) {
                result.complete = false;
            }
        });
        if (!result.complete)
            break;
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
    request.bounds = QRectF(0, 0, preview_width, preview_width / 2);
    request.transform = QTransform(preview_width / 360, 0, 0, -preview_width / 360,
                                   preview_width / 2, preview_width / 4);
    request.revision = geometry_revision_;
    request.preview = true;
    return request;
}
bool GroundMap::preview_usable() const {
    // A whole-world raster has fixed-pixel strokes. At large scales those would
    // become viewport-wide bands; refresh bounded viewport detail instead.
    return map_rect().width() <= 2 * preview_width;
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
    const auto horizontal = current.dx() - scale * raster.transform.dx();
    return QTransform(scale, 0, 0, scale,
                      raster.preview ? horizontal : wrap(horizontal, map_rect().width()),
                      current.dy() - scale * raster.transform.dy());
}
bool GroundMap::same_scale(const Raster &raster) const {
    return !raster.bounds.isEmpty() && !raster.preview && raster.ratio == devicePixelRatioF() &&
           qFuzzyCompare(raster_transform(raster).m11(), 1.0);
}
QRectF GroundMap::raster_draw_bounds(const Raster &raster) const {
    auto bounds = raster_transform(raster).mapRect(raster.bounds);
    if (same_scale(raster)) {
        // Move cached pixels without filtering them a second time. Navigation,
        // coordinates and vector overlays retain their continuous transform;
        // only this transient bitmap placement rounds by at most half a device pixel.
        bounds.moveTopLeft(QPointF(std::floor(bounds.left() * raster.ratio + .5) / raster.ratio,
                                   std::floor(bounds.top() * raster.ratio + .5) / raster.ratio));
    }
    return bounds;
}
bool GroundMap::useful_detail(const Raster &raster, bool allow_rescale) const {
    return !raster.bounds.isEmpty() && !raster.preview && raster.ratio == devicePixelRatioF() &&
           (allow_rescale || same_scale(raster)) &&
           raster_draw_bounds(raster).intersects(plot_rect());
}
bool GroundMap::prefer_detail(const Raster &candidate, const Raster &current,
                              bool allow_rescale) const {
    if (!useful_detail(candidate, allow_rescale))
        return false;
    if (!useful_detail(current, allow_rescale) || covers_view(candidate))
        return true;
    const auto overlap = [&](const Raster &raster) {
        const auto visible = raster_draw_bounds(raster).intersected(plot_rect());
        return visible.width() * visible.height();
    };
    // A late result after a direction reversal must not reduce visible coverage.
    return overlap(candidate) >= overlap(current);
}
void GroundMap::draw_raster(QPainter &p, const Raster &raster) const {
    if (raster.preview) {
        each_world(map_transform(), plot_rect(), [&](const auto &, const auto &area) {
            draw_world_image(p, raster.image, area, plot_rect());
        });
        return;
    }
    const auto bounds = raster_draw_bounds(raster);
    // Viewport rasters already contain all visible copies. Repeating an overscan
    // bitmap would overlap translucent layers at the wrap boundary.
    // Keep the smooth hint: Qt folds matching DPR into a direct pixel copy;
    // disabling it sends fractional-DPR blits through the general scaler.
    if (same_scale(raster))
        p.drawImage(bounds.topLeft(), raster.image);
    else
        p.drawImage(bounds, raster.image);
}
void GroundMap::draw_layer(QPainter &p, const Raster *detail, const Raster *fallback) const {
    if (fallback && (!detail || !raster_draw_bounds(*detail).contains(plot_rect()))) {
        p.save();
        QPainterPath uncovered;
        uncovered.addRect(plot_rect());
        if (detail)
            uncovered.addRect(raster_draw_bounds(*detail));
        // Odd-even clipping uses the actual rounded bitmap bounds, preventing
        // gaps or double opacity where detailed and preview coverage meet.
        p.setClipPath(uncovered, Qt::IntersectClip);
        draw_raster(p, *fallback);
        p.restore();
    }
    if (detail)
        draw_raster(p, *detail);
}
bool GroundMap::covers_view(const Raster &raster, bool exact_scale, double margin) const {
    if (raster.bounds.isEmpty() || raster.ratio != devicePixelRatioF())
        return false;
    const auto transform = raster_transform(raster);
    if (exact_scale ? transform.m11() != 1 : transform.m11() < 0.5 || transform.m11() > 2)
        return false;
    if (exact_scale) {
        // Bitmap placement stays sharp during a fractional pan, but the settled
        // image must still be rasterized at the exact continuous coordinate phase.
        for (const auto offset : {transform.dx(), transform.dy()}) {
            const auto pixels = offset * raster.ratio;
            if (std::abs(pixels - std::round(pixels)) > 1e-6)
                return false;
        }
    }
    return raster_draw_bounds(raster).contains(
        plot_rect().adjusted(-margin, -margin, margin, margin));
}
void GroundMap::cancel_obsolete_raster() {
    if (cancel_raster_ && (pending_view_.preview ? pending_view_.revision != geometry_revision_
                                                 : pending_view_.revision != trails_revision_ ||
                                                       !useful_detail(pending_view_)))
        cancel_raster_->store(true, std::memory_order_relaxed);
}
void GroundMap::install_raster(Raster result) {
    if (result.preview) {
        if (result.complete && result.revision == geometry_revision_)
            preview_ = std::move(result);
        return;
    }
    if (!result.complete || result.revision != trails_revision_ ||
        !(trails_dirty_ ? useful_detail(result) : prefer_detail(result, trails_)))
        return;
    trails_ = std::move(result);
    trails_dirty_ = false;
    if (refinement_pending_) {
        refinement_pending_ = false;
        refinement_animation_.start();
    }
}
void GroundMap::cancel_refinement() {
    refinement_animation_.stop();
    refinement_pending_ = false;
    previous_detail_ = {};
    blended_detail_ = {};
}
const GroundMap::Raster *GroundMap::refinement_display() {
    if (refinement_pending_)
        return &previous_detail_;
    if (refinement_animation_.state() != QAbstractAnimation::Running)
        return nullptr;
    const double amount = refinement_animation_.currentValue().toDouble();
    if (amount == 0)
        return &previous_detail_;
    if (previous_detail_.bounds != trails_.bounds ||
        previous_detail_.image.size() != trails_.image.size()) {
        cancel_refinement();
        return nullptr;
    }
    if (blended_detail_.image.size() != trails_.image.size()) {
        blended_detail_ = trails_;
        allocate_raster(blended_detail_, QImage::Format_ARGB32_Premultiplied);
    }
    const ProfileScope profile("map_refinement_blend_ms");
    // Interpolate premultiplied channels directly. Source-over dims coincident
    // lines, and QPainter's Plus with global opacity can clamp before weighting.
    const unsigned weight = static_cast<unsigned>(std::lround(amount * 256));
    const auto mix = [weight](QRgb from, QRgb to) {
        constexpr unsigned mask = 0x00ff00ff, round = 0x00800080;
        const unsigned red_blue = ((from & mask) * (256 - weight) + (to & mask) * weight + round);
        const unsigned alpha_green =
            (((from >> 8) & mask) * (256 - weight) + ((to >> 8) & mask) * weight + round);
        return ((red_blue >> 8) & mask) | (alpha_green & 0xff00ff00);
    };
    for (int y = 0; y < trails_.image.height(); ++y) {
        const auto *from = reinterpret_cast<const QRgb *>(previous_detail_.image.constScanLine(y));
        const auto *to = reinterpret_cast<const QRgb *>(trails_.image.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(blended_detail_.image.scanLine(y));
        for (int x = 0; x < trails_.image.width(); ++x)
            out[x] = mix(from[x], to[x]);
    }
    return &blended_detail_;
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
bool GroundMap::footprints_ready() const {
    return !footprints_ ||
           (!footprint_raster_.image.isNull() &&
            footprint_raster_.revision == footprints_revision_ && covers_view(footprint_raster_));
}
GroundMap::Raster GroundMap::rasterize_footprints(Raster result,
                                                  const std::vector<Footprint> &footprints,
                                                  const std::atomic_bool &cancelled) {
    QElapsedTimer elapsed;
    elapsed.start();
    allocate_raster(result, QImage::Format_ARGB32_Premultiplied);
    result.image.fill(Qt::transparent);
    QPainter output(&result.image);
    output.translate(-result.bounds.topLeft());
    output.setClipRect(result.bounds);
    output.setRenderHint(QPainter::Antialiasing);
    struct VisibleWorld {
        QTransform transform;
        QRectF bounds, geographic;
    };
    std::vector<VisibleWorld> worlds;
    each_world(result.transform, result.bounds, [&](const auto &transform, const auto &area) {
        const auto bounds = result.bounds.intersected(area);
        worlds.push_back({transform, bounds, transform.inverted().mapRect(bounds)});
    });
    FootprintClipBuffer projected, scratch;
    Raster mask = result;
    mask.image = {};
    Raster samples = mask;
    samples.ratio *= 2;
    for (const auto &footprint : footprints) {
        if (cancelled.load(std::memory_order_relaxed))
            break;
        bool fill_visible = false;
        QPainterPath fill_path;
        if (footprint.filled && footprint.opacity > 0) {
            fill_path = footprint_path(footprint.ground, &cancelled);
            if (cancelled.load(std::memory_order_relaxed))
                break;
            const auto fill_bounds = fill_path.boundingRect();
            fill_visible = std::any_of(worlds.begin(), worlds.end(), [&](const auto &world) {
                return world.geographic.intersects(fill_bounds);
            });
        }
        if (fill_visible) {
            if (mask.image.isNull()) {
                allocate_raster(mask, QImage::Format_ARGB32_Premultiplied);
                allocate_raster(samples, QImage::Format_Alpha8);
            }
            samples.image.fill(0);
            QPainter fill(&samples.image);
            fill.translate(-result.bounds.topLeft());
            fill.setClipRect(result.bounds);
            fill.setPen(Qt::NoPen);
            fill.setBrush(Qt::white);
            // Resolve coverage at 2x in an opaque one-byte mask, then average
            // each 2x2 block once. Shared facet edges cannot introduce seams or
            // accumulate alpha, and large meshes avoid a costly winding union.
            const auto append = [&](std::span<const QPointF> points) {
                if (points.size() >= 3)
                    fill.drawPolygon(points.data(), static_cast<int>(points.size()));
            };
            std::array<QPointF, 8> polygon;
            int count = 0;
            const auto draw = [&] {
                if (count < 3)
                    return;
                double left = polygon[0].x(), right = left;
                double top = polygon[0].y(), bottom = top;
                for (int j = 1; j < count; ++j) {
                    left = std::min(left, polygon[j].x());
                    right = std::max(right, polygon[j].x());
                    top = std::min(top, polygon[j].y());
                    bottom = std::max(bottom, polygon[j].y());
                }
                const QRectF geographic_bounds(QPointF(left, top), QPointF(right, bottom));
                for (const auto &world : worlds) {
                    if (!world.geographic.intersects(geographic_bounds))
                        continue;
                    bool contained = true;
                    for (int j = 0; j < count; ++j) {
                        projected[j] = world.transform.map(polygon[j]);
                        contained = contained && world.bounds.contains(projected[j]);
                    }
                    if (contained) {
                        append(std::span(projected.data(), static_cast<std::size_t>(count)));
                    } else {
                        const auto clipped =
                            clip_footprint_polygon(projected, count, world.bounds, scratch);
                        append(clipped);
                    }
                }
            };
            for (int i = 0; i < fill_path.elementCount(); ++i) {
                if ((i % 256) == 0 && cancelled.load(std::memory_order_relaxed))
                    break;
                const auto element = fill_path.elementAt(i);
                if (element.isMoveTo()) {
                    draw();
                    count = 0;
                }
                // Ground projection emits triangles or at most six-vertex polar caps.
                polygon.at(count++) = {element.x, element.y};
            }
            if (!cancelled.load(std::memory_order_relaxed))
                draw();
            fill.end();
            std::array<QRgb, 5> palette;
            for (int i = 0; i <= 4; ++i)
                palette[i] = qPremultiply(qRgba(footprint.color.red(), footprint.color.green(),
                                                footprint.color.blue(), (i * 255 + 2) / 4));
            for (int y = 0; y < mask.image.height(); ++y) {
                if ((y % 64) == 0 && cancelled.load(std::memory_order_relaxed))
                    break;
                auto *target = reinterpret_cast<QRgb *>(mask.image.scanLine(y));
                const auto *top = samples.image.constScanLine(2 * y);
                const auto *bottom = samples.image.constScanLine(2 * y + 1);
                for (int x = 0; x < mask.image.width(); ++x) {
                    const auto covered = (top[2 * x] >> 7) + (top[2 * x + 1] >> 7) +
                                         (bottom[2 * x] >> 7) + (bottom[2 * x + 1] >> 7);
                    target[x] = palette[covered];
                }
            }
            if (cancelled.load(std::memory_order_relaxed))
                break;
            output.setOpacity(footprint.opacity);
            output.drawImage(result.bounds.topLeft(), mask.image);
            output.setOpacity(1);
        }
        auto color = footprint.color;
        color.setAlphaF(.85f);
        output.setPen(QPen(color, 1.2));
        output.setBrush(Qt::NoBrush);
        QPainterPath edges;
        for (const auto &edge : footprint.edges) {
            if (cancelled.load(std::memory_order_relaxed))
                break;
            edges.addPath(ground_path({ground_coordinates(edge[0]), ground_coordinates(edge[1])}));
        }
        output.save();
        for (const auto &world : worlds) {
            output.setClipRect(world.bounds);
            if (!draw_track(output, world.transform.map(edges), &cancelled))
                break;
        }
        output.restore();
    }
    output.end();
    result.complete = !cancelled.load(std::memory_order_relaxed);
    result.milliseconds = elapsed.nsecsElapsed() * 1e-6;
    return result;
}
void GroundMap::poll_footprints() {
    if (!pending_footprints_.valid() ||
        pending_footprints_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    try {
        auto result = pending_footprints_.get();
        if (WorkbenchProfile::active)
            WorkbenchProfile::active->record("map_footprint_worker_ms", result.milliseconds);
        if (result.complete && result.revision >= footprints_reset_revision_) {
            if (result.preview)
                footprint_preview_ = std::move(result);
            else if (useful_detail(result, true) &&
                     (result.revision > footprint_raster_.revision ||
                      prefer_detail(result, footprint_raster_, true)))
                footprint_raster_ = std::move(result);
        }
    } catch (const std::exception &) {
        // A later paint can retry without blocking interaction or publishing stale geometry.
    }
    footprint_timer_.stop();
    cancel_footprints_.reset();
    update();
}
void GroundMap::prepare_footprints(bool navigating) {
    if (cancel_footprints_ &&
        (footprint_request_.revision < footprints_reset_revision_ ||
         (!footprint_request_.preview && !useful_detail(footprint_request_, true))))
        cancel_footprints_->store(true, std::memory_order_relaxed);
    poll_footprints();
    if (!footprints_ || pending_footprints_.valid())
        return;
    const bool ready =
        footprint_raster_.revision == footprints_revision_ &&
        (navigating ? same_scale(footprint_raster_) && covers_view(footprint_raster_, false, 64)
                    : footprints_ready());
    const bool preview_ready =
        footprint_preview_.revision == footprints_revision_ && !footprint_preview_.image.isNull();
    if (ready && (!preview_usable() || preview_ready))
        return;
    // Alternate overview and detail during playback so neither fallback can age
    // indefinitely. Only one immutable request runs; intermediate times are coalesced.
    const bool preview =
        preview_usable() && (footprint_preview_.image.isNull() ||
                             footprint_preview_.revision < footprint_raster_.revision);
    if (zoom_animation_.state() == QAbstractAnimation::Running && !preview && preview_usable() &&
        preview_ready)
        return;
    // Refill before the overscan runs out. Zooming reprojects completed coverage
    // and keeps useful workers running until current-scale detail replaces it.
    auto request = preview ? preview_request() : raster_request();
    request.revision = footprints_revision_;
    footprint_request_ = request;
    cancel_footprints_ = std::make_shared<std::atomic_bool>(false);
    pending_footprints_ =
        std::async(std::launch::async, [request = std::move(request), shapes = footprints_,
                                        cancel = cancel_footprints_]() mutable {
            return rasterize_footprints(std::move(request), *shapes, *cancel);
        });
    footprint_timer_.start();
}
void GroundMap::paint_foreground(QPainter &p, const Raster *display, const Raster *fallback) {
    p.save();
    p.setClipRect(plot_rect());
    paint_grid(p);
    draw_layer(p, display, fallback);
    p.restore();
}
void GroundMap::paint_base(QPainter &p, const Raster *display, const Raster *fallback,
                           bool cached_foreground) {
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), theme::color(theme::chrome));
    p.save();
    p.setClipRect(plot_rect());
    {
        const ProfileScope profile("map_background_blit_ms");
        draw_raster(p, background_);
    }
    p.restore();
    if (cached_foreground)
        p.drawImage(QPointF(0, 0), foreground_image_);
    else
        paint_foreground(p, display, fallback);
    // Text stays on the opaque target: transparent glyph caching can change
    // Qt's subpixel antialiasing and gamma-correct blending.
    paint_axes(p);
}
void GroundMap::paintEvent(QPaintEvent *event) {
    const ProfileScope profile("map_paint_ms");
    const auto plot = plot_rect();
    const bool footer_only = event->rect().top() >= height() - 24;
    const bool zooming = zoom_animation_.state() == QAbstractAnimation::Running;
    const bool navigating = (dragging_ && moved_ && pan_settle_timer_.isActive()) || zooming;
    if (!footer_only)
        prepare_footprints(navigating);
    if (tracks_transitioning() &&
        (previous_detail_.transform != map_transform() ||
         previous_detail_.ratio != devicePixelRatioF() || !covers_view(previous_detail_)))
        cancel_refinement();
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
    const bool detail_ready =
        !trails_dirty_ &&
        (navigating ? same_scale(trails_) && covers_view(trails_, false, 64) : tracks_ready());
    if (!footer_only && !detail_ready && !pending_raster_.valid()) {
        const auto paths = strokes();
        std::size_t vertices = 0;
        for (const auto &stroke : paths)
            vertices += stroke.path.elementCount();
        auto request = raster_request();
        if (vertices > 1024) {
            const bool needs_preview =
                preview_usable() && !refinement_pending_ &&
                (preview_.image.isNull() || preview_.revision != geometry_revision_);
            // Panning always refills detail, including before the pointer is
            // released. Zoom bursts may reproject the overview until they settle.
            if (!zooming || needs_preview || !preview_usable()) {
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
        // Preserve every useful portion of detailed coverage. Only newly exposed
        // regions use the overview while the next immutable viewport is prepared.
        const Raster *display = nullptr;
        if (!trails_dirty_ && !trails_.image.isNull() && trails_.ratio == devicePixelRatioF()) {
            const auto scale = raster_transform(trails_).m11();
            if (scale >= .5 && scale <= 2 && raster_draw_bounds(trails_).intersects(plot))
                display = &trails_;
        }
        const Raster *fallback =
            preview_usable() && !preview_.image.isNull() && preview_.revision == geometry_revision_
                ? &preview_
                : nullptr;
        if (const auto *detail = refinement_display())
            display = detail;
        const BaseKey key{map_transform(),
                          size(),
                          devicePixelRatioF(),
                          background_.image.cacheKey(),
                          display ? display->image.cacheKey() : 0,
                          fallback ? fallback->image.cacheKey() : 0,
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
            foreground_image_ = {};
            paint_base(p, display, fallback);
        } else {
            if (base_image_.isNull() || key != base_key_) {
                const ProfileScope compose("map_composite_ms");
                const QSize pixels(static_cast<int>(std::ceil(width() * key.ratio)),
                                   static_cast<int>(std::ceil(height() * key.ratio)));
                // Playback changes sunlight independently of the grid and
                // track raster. Keep one viewport-sized transparent layer so those
                // static elements need not be stroked again each time.
                auto foreground_key = key;
                foreground_key.background = 0;
                if (foreground_image_.isNull() || foreground_key != foreground_key_) {
                    const ProfileScope foreground("map_foreground_ms");
                    if (foreground_image_.size() != pixels)
                        foreground_image_ = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
                    foreground_image_.setDevicePixelRatio(key.ratio);
                    foreground_image_.fill(Qt::transparent);
                    QPainter overlay(&foreground_image_);
                    overlay.setRenderHint(QPainter::Antialiasing);
                    overlay.setRenderHint(QPainter::SmoothPixmapTransform);
                    overlay.setFont(font());
                    paint_foreground(overlay, display, fallback);
                    foreground_key_ = foreground_key;
                }
                if (base_image_.size() != pixels) {
                    base_image_ = QImage(pixels, QImage::Format_RGB32);
                }
                base_image_.setDevicePixelRatio(key.ratio);
                QPainter cache(&base_image_);
                cache.setFont(font());
                paint_base(cache, display, fallback, true);
            }
            p.drawImage(QPointF(0, 0), base_image_);
        }
        base_key_ = key;
        p.setFont(font());
        p.save();
        p.setClipRect(plot);
        const Raster *footprint = nullptr;
        if (!footprint_raster_.image.isNull() && useful_detail(footprint_raster_, true))
            footprint = &footprint_raster_;
        const auto *footprint_fallback =
            preview_usable() && !footprint_preview_.image.isNull() ? &footprint_preview_ : nullptr;
        draw_layer(p, footprint, footprint_fallback);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        const auto label_context = font().toString() + '/' + QString::number(devicePixelRatioF());
        if (access_visible_ && access_pair_ && access_pair_->first < points_.size() &&
            access_pair_->second < points_.size()) {
            p.setPen(QPen(theme::color(theme::success), 2, Qt::DashLine));
            const auto path = ground_path({points_[access_pair_->first].geographic,
                                           points_[access_pair_->second].geographic});
            each_world(map_transform(), plot, [&](const auto &transform, const auto &) {
                draw_track(p, transform.map(path));
            });
        }
        each_world(map_transform(), plot, [&](const auto &transform, const auto &) {
            for (std::size_t i = 0; i < points_.size(); ++i) {
                const auto point = transform.map(points_[i].geographic);
                if (!plot.contains(point))
                    continue;
                p.setPen(QPen(theme::color(theme::canvas), 1.5));
                p.setBrush(selected_ == i              ? theme::color(theme::sakura)
                           : points_[i].ground_station ? theme::color(theme::success)
                           : i < tracks_.size() ? QColor::fromRgb(tracks_[i].display.line_color)
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
                const QRectF label(
                    std::clamp(point.x() + 12, plot.left(), plot.right() - label_width),
                    std::clamp(point.y() - 12, plot.top(), plot.bottom() - 22), label_width, 22);
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
        });
        p.restore();
    }
    p.setFont(font());
    p.setPen(theme::color(theme::text_muted));
    QString caption = ui::text("ground_map.caption");
    if (cursor_position_) {
        auto ll = map_transform().inverted().map(*cursor_position_);
        ll.setX(wrap(ll.x(), 360));
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
    const int scale_width = fontMetrics().horizontalAdvance(scale);
    const int caption_width = std::max(0, width() - 48 - scale_width);
    p.drawText(QRectF(16, height() - 22, caption_width, 18), Qt::AlignVCenter,
               fontMetrics().elidedText(caption, Qt::ElideRight, caption_width));
    p.drawText(QRectF(width() - 16 - scale_width, height() - 22, scale_width, 18),
               Qt::AlignVCenter | Qt::AlignRight, scale);
}
void GroundMap::mousePressEvent(QMouseEvent *event) {
    if ((event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton) ||
        !visible_map_rect().contains(event->position())) {
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
    cursor_position_ = visible_map_rect().contains(event->position())
                           ? std::optional<QPointF>(event->position())
                           : std::nullopt;
    update(QRect(0, height() - 24, width(), 24));
    if (dragging_)
        drag_to(event->position());
    else if (cursor_position_)
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
    pan_settle_timer_.stop();
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
                      visible_map_rect().contains(event->position());
    finish_drag();
    if (pick && select) {
        std::optional<std::size_t> hit;
        double nearest = 12;
        for (std::size_t i = 0; i < points_.size(); ++i) {
            auto point = marker_position(i);
            point.setX(event->position().x() +
                       wrap(point.x() - event->position().x(), map_rect().width()));
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
    if (event->button() != Qt::LeftButton || !visible_map_rect().contains(event->position())) {
        event->ignore();
        return;
    }
    request_zoom(event->modifiers().testFlag(Qt::ShiftModifier) ? 0.5 : 2.0, event->position());
    event->accept();
}
void GroundMap::wheelEvent(QWheelEvent *event) {
    if (!visible_map_rect().contains(event->position()) || dragging_) {
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
} // namespace orbelle::desktop
