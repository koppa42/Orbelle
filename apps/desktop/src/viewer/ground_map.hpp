#pragma once
#include "display_settings.hpp"
#include "orbit_scene.hpp"
#include <QImage>
#include <QPainterPath>
#include <QTransform>
#include <QWidget>
#include <functional>
#include <optional>

namespace orbitforge::desktop {
// Display projection only: ITRF Cartesian positions onto the WGS84 reference ellipsoid.
QPointF ground_coordinates(const std::array<double, 3> &position_m);
QPainterPath ground_path(const std::vector<QPointF> &longitude_latitude);

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
    std::function<void(std::optional<std::size_t>)> select;
    void set_orbits(const std::vector<std::shared_ptr<const OrbitScene>> &orbits);
    static PreparedTracks
    prepare_tracks(const std::vector<std::shared_ptr<const OrbitScene>> &orbits);
    void publish_tracks(PreparedTracks tracks);
    void replace_track(std::size_t index, Track track);
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
    };
    QRectF plot_rect() const;
    QRectF map_rect() const;
    QTransform map_transform() const;
    void clamp_pan();
    QImage imagery_;
    QImage night_;
    std::array<double, 3> sun_direction_{1, 0, 0};
    std::optional<QPointF> cursor_position_;
    bool lighting_ = true, grid_ = true, show_tracks_ = true, labels_ = true, show_sun_ = true;
    std::vector<Track> tracks_;
    std::vector<Point> points_;
    std::optional<std::size_t> selected_;
    core::Epoch epoch_{0};
    double zoom_ = 1;
    QPointF pan_, press_, last_mouse_;
    bool dragging_ = false, moved_ = false;
};
} // namespace orbitforge::desktop
