#pragma once
#include "display_settings.hpp"
#include "orbit_scene.hpp"
#include <QCache>
#include <QElapsedTimer>
#include <QImage>
#include <QPainterPath>
#include <QTimer>
#include <QTransform>
#include <QVariantAnimation>
#include <QWidget>
#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <orbelle/visualization/sensor_view.hpp>
#include <span>

namespace orbelle::desktop {
// Display projection only: ITRF Cartesian positions onto the WGS84 reference ellipsoid.
QPointF ground_coordinates(const std::array<double, 3> &position_m);
// Opaque multiplicative day/night display mask; input is an ITRF unit direction.
QImage ground_lighting_mask(const std::array<double, 3> &sun_direction);
QPainterPath ground_path(std::span<const QPointF> longitude_latitude);
QPainterPath ground_footprint_path(std::span<const core::SensorTriangle> triangles);
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
        DisplaySettings display;
    };
    using PreparedTracks = std::vector<Track>;
    explicit GroundMap(const QString &imagery, QWidget *parent = nullptr);
    explicit GroundMap(QImage imagery, QWidget *parent = nullptr);
    [[nodiscard]] const QImage &imagery() const { return imagery_; }
    ~GroundMap() override;
    std::function<void(std::optional<std::size_t>)> select;
    void set_orbits(const std::vector<std::shared_ptr<const OrbitScene>> &orbits);
    static PreparedTracks
    prepare_tracks(const std::vector<std::shared_ptr<const OrbitScene>> &orbits);
    void publish_tracks(PreparedTracks tracks);
    // Same trajectory at higher display detail: retain its visible raster until the
    // replacement is ready, then blend briefly. Ordinary publication cancels this.
    void refine_tracks(PreparedTracks tracks);
    void append_track(Track track);
    void replace_track(std::size_t index, Track track);
    void set_name(std::size_t index, const QString &name);
    void set_station_name(std::size_t index, const std::string &id, const QString &name);
    void set_ground_stations(const std::vector<application::ProjectGroundStation> &stations);
    void set_snapshot(const visualization::RenderSnapshot &snapshot);
    // Continuous playback keeps completed coverage while coalescing newer positions.
    // Configuration changes and time jumps use the default replacement behavior.
    void set_sensor_footprints(std::span<const visualization::SensorMesh> meshes,
                               bool continuous = false);
    void set_selection(std::optional<std::size_t> index);
    void set_access_link(std::optional<std::pair<std::size_t, std::size_t>> pair, bool visible);
    void set_display(std::size_t index, const DisplaySettings &settings, core::Epoch first,
                     core::Epoch last);
    // Continuous playback coalesces lighting updates in one image worker. Explicit
    // time jumps/settings replace immediately; the latest image settles after pause.
    void set_sun_direction(const std::array<double, 3> &direction, bool continuous = false);
    void set_lighting(bool enabled);
    void set_grid_visible(bool enabled);
    void set_tracks_visible(bool enabled);
    void set_labels_visible(bool enabled);
    void set_sun_visible(bool enabled);
    void focus_selection();
    [[nodiscard]] const std::array<double, 3> &sun_direction() const { return sun_direction_; }
    void reset_view();
    void zoom(double factor, QPointF anchor);
    void zoom_centered(double factor);
    [[nodiscard]] core::Epoch epoch() const { return epoch_; }
    [[nodiscard]] std::size_t track_count() const { return tracks_.size(); }
    [[nodiscard]] std::span<const QPointF> track_coordinates(std::size_t index) const {
        return tracks_.at(index).points;
    }
    [[nodiscard]] QPointF marker_position(std::size_t index) const;
    [[nodiscard]] QPointF marker_coordinates(std::size_t index) const;
    [[nodiscard]] double zoom_factor() const { return zoom_; }
    [[nodiscard]] bool tracks_ready() const;
    [[nodiscard]] bool footprints_ready() const;
    [[nodiscard]] bool background_ready() const;
    [[nodiscard]] bool tracks_transitioning() const {
        return refinement_pending_ || refinement_animation_.state() == QAbstractAnimation::Running;
    }

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
    bool event(QEvent *) override;

  private:
    struct Point {
        QString name;
        QPointF geographic;
        bool ground_station = false;
        bool operator==(const Point &) const = default;
    };
    struct Footprint {
        std::vector<core::SensorTriangle> ground;
        std::vector<std::array<core::SensorVector, 2>> edges;
        QColor color;
        double opacity;
        bool filled;
    };
    std::shared_ptr<const std::vector<Footprint>> footprints_;
    QRectF plot_rect() const;
    QRectF map_rect() const;
    QRectF visible_map_rect() const;
    double minimum_zoom() const;
    QTransform map_transform() const;
    void clamp_pan();
    void apply_zoom(double value, QPointF anchor, std::optional<QPointF> location = std::nullopt);
    void request_zoom(double factor, QPointF anchor);
    void pan_by(QPointF delta);
    void drag_to(QPointF position);
    void finish_drag(bool restore = false);
    void rebuild_night();
    void paint_background(QPainter &p, const QRectF &bounds, bool cache_imagery);
    void invalidate_background(bool immediate);
    void prepare_background(bool navigating);
    void poll_background();
    void paint_grid(QPainter &p);
    void paint_axes(QPainter &p);
    void invalidate_tracks(bool geometry_changed = true);
    struct Stroke {
        QPainterPath path;
        QColor color;
        double width;
        application::LineStyle style;
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
    struct BackgroundResult {
        Raster raster;
        QImage night;
        std::uint64_t generation = 0;
    };
    void paint_foreground(QPainter &p, const Raster *display, const Raster *fallback);
    void paint_base(QPainter &p, const Raster *display, const Raster *fallback,
                    bool cached_foreground = false);
    struct BaseKey {
        QTransform transform;
        QSize size;
        double ratio = 1;
        qint64 background = 0, trails = 0, preview = 0;
        bool grid = true;
        QFont font;
        bool operator==(const BaseKey &) const = default;
    };
    BaseKey base_key_;
    QImage base_image_;
    BaseKey foreground_key_;
    QImage foreground_image_;
    QElapsedTimer view_stable_;
    QCache<QString, QImage> labels_cache_{4096}; // KiB; bounded independently of object count.
    Raster raster_request(double margin = 160) const;
    Raster preview_request() const;
    bool preview_usable() const;
    QTransform raster_transform(const Raster &raster) const;
    bool same_scale(const Raster &raster) const;
    QRectF raster_draw_bounds(const Raster &raster) const;
    bool useful_detail(const Raster &raster, bool allow_rescale = false) const;
    bool prefer_detail(const Raster &candidate, const Raster &current,
                       bool allow_rescale = false) const;
    void draw_raster(QPainter &p, const Raster &raster) const;
    void draw_layer(QPainter &p, const Raster *detail, const Raster *fallback) const;
    bool covers_view(const Raster &raster, bool exact_scale = true, double margin = 0) const;
    void cancel_obsolete_raster();
    static void allocate_raster(Raster &raster, QImage::Format format);
    std::vector<Stroke> strokes() const;
    static Raster rasterize(Raster result, const std::vector<Stroke> &strokes,
                            const std::atomic_bool &cancelled);
    void poll_raster();
    static Raster rasterize_footprints(Raster result, const std::vector<Footprint> &footprints,
                                       const std::atomic_bool &cancelled);
    void poll_footprints();
    void prepare_footprints(bool navigating);
    void install_raster(Raster result);
    void cancel_refinement();
    const Raster *refinement_display();
    std::future<Raster> pending_raster_;
    std::shared_ptr<std::atomic_bool> cancel_raster_;
    QTimer raster_timer_;
    std::future<Raster> pending_footprints_;
    std::shared_ptr<std::atomic_bool> cancel_footprints_;
    QTimer footprint_timer_;
    std::uint64_t footprints_revision_ = 0;
    std::uint64_t footprints_reset_revision_ = 0;
    Raster footprint_raster_, footprint_preview_, footprint_request_;
    std::uint64_t trails_revision_ = 0;
    std::uint64_t geometry_revision_ = 0;
    QImage imagery_;
    QImage night_;
    std::future<BackgroundResult> pending_background_;
    QTimer background_timer_;
    std::uint64_t lighting_revision_ = 0, background_generation_ = 0;
    bool background_reset_ = true;
    Raster background_, imagery_raster_, trails_, preview_, pending_view_;
    Raster previous_detail_, blended_detail_;
    QVariantAnimation refinement_animation_;
    bool refinement_pending_ = false;
    bool background_dirty_ = true, trails_dirty_ = true, night_dirty_ = true;
    std::array<double, 3> sun_direction_{1, 0, 0};
    std::optional<QPointF> cursor_position_;
    bool lighting_ = true, grid_ = true, show_tracks_ = true, labels_ = true, show_sun_ = true;
    std::vector<Track> tracks_;
    std::vector<Point> points_;
    std::map<std::string, QString> station_names_;
    std::optional<std::size_t> selected_;
    std::optional<std::pair<std::size_t, std::size_t>> access_pair_;
    bool access_visible_ = false;
    core::Epoch epoch_{0};
    double zoom_ = 1;
    // Fractions of projected map width preserve the geographic center on resize.
    QPointF pan_, press_pan_, press_, last_mouse_;
    QTimer pan_settle_timer_;
    QVariantAnimation zoom_animation_;
    QPointF zoom_anchor_, zoom_location_;
    QSize navigation_size_;
    Qt::MouseButton drag_button_ = Qt::NoButton;
    bool dragging_ = false, moved_ = false;
};
} // namespace orbelle::desktop
