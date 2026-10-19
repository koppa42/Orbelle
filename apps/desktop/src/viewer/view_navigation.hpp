#pragma once
#include <QObject>
#include <QPointer>
#include <QVariantAnimation>
#include <QWindow>
#include <functional>
#include <vsg/app/Trackball.h>

namespace orbitforge::desktop {
// Qt logical-pixel input for an existing camera; owns no rendering or orbit resources.
class ViewNavigation final : public QObject {
  public:
    ViewNavigation(QWindow &window, vsg::ref_ptr<vsg::Camera> camera);
    ~ViewNavigation() override;
    std::function<void(QPointF)> select; // Logical pixels, after a completed left click.
    std::function<void()> fit;
    void set_viewpoint(vsg::ref_ptr<vsg::LookAt> viewpoint);
    void zoom(double factor, bool animated = true); // >1 moves closer to the current target.
    void stop();

  protected:
    bool eventFilter(QObject *, QEvent *) override;

  private:
    enum class Drag { none, rotate, pan, zoom };
    void drag_to(QPointF point);
    void finish_drag(bool restore = false);
    void pan(QPointF delta);
    void apply_distance(double distance);
    void update_projection();
    [[nodiscard]] double distance() const;
    [[nodiscard]] double minimum_distance() const;
    QPointer<QWindow> window_;
    vsg::ref_ptr<vsg::Camera> camera_;
    vsg::ref_ptr<vsg::LookAt> look_, drag_start_;
    vsg::ref_ptr<vsg::Trackball> rotation_;
    QVariantAnimation zoom_animation_;
    QPointF press_, previous_;
    Qt::MouseButton button_ = Qt::NoButton;
    Drag drag_ = Drag::none;
    bool moved_ = false;
};
} // namespace orbitforge::desktop
