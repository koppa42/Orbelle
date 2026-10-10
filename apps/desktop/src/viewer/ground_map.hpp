#pragma once
#include "display_settings.hpp"
#include "orbit_scene.hpp"
#include <QImage>
#include <QPainterPath>
#include <QTimer>
#include <QTransform>
#include <QWidget>
#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <span>

namespace orbitforge::desktop {
// Display projection only: ITRF Cartesian positions onto the WGS84 reference ellipsoid.
QPointF ground_coordinates(const std::array<double, 3> &position_m);
QPainterPath ground_path(std::span<const QPointF> longitude_latitude);
inline QPainterPath ground_path(std::initializer_list<QPointF> points) {
    return ground_path(std::span<const QPointF>(points.begin(), points.size()));
}

class GroundMap final : public QWidget {
  public:
    struct Track {
        QString name;
        QPainterPath path;
        std::vector<QPointF> points;
        std::vector<core::Epoch> epochs;
        std::size_t first = 0, last = 0;
        bool visible = true, label = true;
    };
    using PreparedTracks = std::vector<Track>;
    explicit GroundMap(const QString &imagery, QWidget *parent = nullptr);
    ~GroundMap() override;
    std::function<void(std::optional<std::size_t>)> select;
    void set_orbits(const std::vector<std::shared_ptr<const OrbitScene>> &orbits);
    static PreparedTracks
    prepare_tracks(const std::vector<std::shared_ptr<const OrbitScene>> &orbits);
    void publish_tracks(PreparedTracks tracks);
    void append_track(Track track);
    void replace_track(std::size_t index, Track track);
    void set_name(std::size_t index, const QString &name);
    void set_ground_stations(const std::vector<application::ProjectGroundStation> &stations);
    void set_snapshot(const visualization::RenderSnapshot &snapshot);
    void set_selection(std::optional<std::size_t> index);
    void set_display(std::size_t index, const DisplaySettings &settings, core::Epoch first,
                     core::Epoch last);
    void set_sun_direction(const std::array<double, 3> &direction);
    void set_lighting(bool enabled);
    void set_grid_visible(bool enabled);
    void set_tracks_visible(bool enabled);
    void set_labels_visible(bool enabled);
    void set_sun_visible(bool enabled);
    void focus_selection();
    [[nodiscard]] const std::array<double, 3> &sun_direction() const { return sun_direction_; }
    void reset_view();
    void zoom(double factor, QPointF anchor);
    [[nodiscard]] core::Epoch epoch() const { return epoch_; }
    [[nodiscard]] std::size_t track_count() const { return tracks_.size(); }
    [[nodiscard]] QPointF marker_position(std::size_t index) const;
    [[nodiscard]] QPointF marker_coordinates(std::size_t index) const;
    [[nodiscard]] double zoom_factor() const { return zoom_; }
    [[nodiscard]] bool tracks_ready() const;

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

  private:
    struct Point {
        QString name;
        QPointF geographic;
        bool ground_station = false;
        bool operator==(const Point &) const = default;
    };
    QRectF plot_rect() const;
    QRectF map_rect() const;
    QTransform map_transform() const;
    void clamp_pan();
    void rebuild_night();
    void paint_background(QPainter &p, const QRectF &bounds);
    void paint_grid(QPainter &p);
    void paint_axes(QPainter &p);
    void invalidate_tracks(bool geometry_changed = true);
    struct Stroke {
        QPainterPath path;
        QColor color;
        double width;
    };
    struct Raster {
        QImage image;
        QRectF bounds;
        QTransform transform;
        std::uint64_t revision = 0;
        double ratio = 1;
        double milliseconds = 0;
        bool complete = true;
        bool preview = false;
    };
    Raster raster_request(double margin = 160) const;
    Raster preview_request() const;
    QTransform raster_transform(const Raster &raster) const;
    bool covers_view(const Raster &raster, bool exact_scale = true) const;
    void cancel_obsolete_raster();
    static void allocate_raster(Raster &raster, QImage::Format format);
    std::vector<Stroke> strokes() const;
    static Raster rasterize(Raster result, const std::vector<Stroke> &strokes,
                            const std::atomic_bool &cancelled);
    void poll_raster();
    void install_raster(Raster result);
    std::future<Raster> pending_raster_;
    std::shared_ptr<std::atomic_bool> cancel_raster_;
    QTimer raster_timer_;
    std::uint64_t trails_revision_ = 0;
    std::uint64_t geometry_revision_ = 0;
    QImage imagery_;
    QImage night_;
    Raster background_, trails_, preview_, pending_view_;
    bool background_dirty_ = true, trails_dirty_ = true, night_dirty_ = true;
    std::array<double, 3> sun_direction_{1, 0, 0};
    std::optional<QPointF> cursor_position_;
    bool lighting_ = true, grid_ = true, show_tracks_ = true, labels_ = true, show_sun_ = true;
    std::vector<Track> tracks_;
    std::vector<Point> points_;
    std::map<std::string, QString> station_names_;
    std::optional<std::size_t> selected_;
    core::Epoch epoch_{0};
    double zoom_ = 1;
    QPointF pan_, press_, last_mouse_;
    bool dragging_ = false, moved_ = false;
};
} // namespace orbitforge::desktop
