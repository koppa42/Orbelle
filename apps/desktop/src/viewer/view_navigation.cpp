#include "view_navigation.hpp"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vsg/app/ProjectionMatrix.h>

namespace orbitforge::desktop {
namespace {
constexpr double earth_radius = 6378137.0;
constexpr double maximum_distance = 1e11;
} // namespace
ViewNavigation::ViewNavigation(QWindow &window, vsg::ref_ptr<vsg::Camera> camera)
    : window_(&window), camera_(std::move(camera)), look_(camera_->viewMatrix.cast<vsg::LookAt>()),
      rotation_(vsg::Trackball::create(camera_)) {
    zoom_animation_.setDuration(120);
    zoom_animation_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&zoom_animation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) { apply_distance(value.toDouble()); });
    // Use VSG's camera rotation math, with Qt handling gestures and their lifecycle.
    // No frame-driven throw or keyboard acceleration continues after input ends.
    rotation_->supportsThrow = false;
    window.installEventFilter(this);
}
ViewNavigation::~ViewNavigation() {
    if (window_) {
        window_->removeEventFilter(this);
        window_->unsetCursor();
    }
}
double ViewNavigation::distance() const { return vsg::length(look_->eye - look_->center); }
double ViewNavigation::minimum_distance() const {
    const auto direction = vsg::normalize(look_->eye - look_->center);
    // Conservative spherical envelope works in both GCRF and ITRF, including a
    // translated orbit target. Stop on the near surface instead of crossing Earth.
    const double radius = earth_radius + 100;
    const auto b = vsg::dot(look_->center, direction);
    const auto c = vsg::dot(look_->center, look_->center) - radius * radius;
    const auto discriminant = b * b - c;
    if (discriminant < 0)
        return 1;
    const auto near = -b - std::sqrt(discriminant);
    const auto far = -b + std::sqrt(discriminant);
    return distance() >= near ? std::max(1.0, far) : 1.0;
}
void ViewNavigation::apply_distance(double value) {
    value = std::clamp(value, minimum_distance(), maximum_distance);
    look_->eye = look_->center + vsg::normalize(look_->eye - look_->center) * value;
    update_projection();
}
void ViewNavigation::update_projection() {
    auto *projection = dynamic_cast<vsg::Perspective *>(camera_->projectionMatrix.get());
    if (!projection)
        return;
    projection->nearDistance = std::clamp(distance() * 1e-4, 0.01, earth_radius * 0.001);
    const auto clearance = vsg::length(look_->eye) - earth_radius;
    if (clearance > 0)
        projection->nearDistance =
            std::min(projection->nearDistance, std::max(0.01, clearance / 4));
    projection->farDistance = std::max(
        {projection->farDistance, distance() * 4, vsg::length(look_->eye) + earth_radius * 24});
}
void ViewNavigation::zoom(double factor, bool animated) {
    if (!std::isfinite(factor) || factor <= 0 || factor == 1)
        return;
    finish_drag();
    double base = distance();
    if (animated && zoom_animation_.state() == QAbstractAnimation::Running) {
        const auto target = zoom_animation_.endValue().toDouble();
        if ((factor > 1) == (target < base))
            base = target;
    }
    zoom_animation_.stop();
    const auto target = std::clamp(base / factor, minimum_distance(), maximum_distance);
    if (!animated) {
        apply_distance(target);
        return;
    }
    if (std::abs(target - distance()) < 1e-9)
        return;
    apply_distance(distance() + (target - distance()) * 0.25);
    {
        const QSignalBlocker blocker(&zoom_animation_);
        zoom_animation_.setStartValue(distance());
        zoom_animation_.setEndValue(target);
        zoom_animation_.setCurrentTime(0);
    }
    zoom_animation_.start();
}
void ViewNavigation::set_viewpoint(vsg::ref_ptr<vsg::LookAt> viewpoint) {
    stop();
    rotation_->setViewpoint(std::move(viewpoint), 0);
    update_projection();
}
void ViewNavigation::stop() {
    zoom_animation_.stop();
    finish_drag();
}
void ViewNavigation::pan(QPointF delta) {
    const auto *projection = dynamic_cast<vsg::Perspective *>(camera_->projectionMatrix.get());
    if (!projection || !window_ || window_->height() <= 0)
        return;
    const auto forward = vsg::normalize(look_->center - look_->eye);
    const auto right = vsg::normalize(vsg::cross(forward, look_->up));
    const auto up = vsg::normalize(vsg::cross(right, forward));
    const auto scale = 2 * distance() *
                       std::tan(projection->fieldOfViewY * std::numbers::pi / 360) /
                       window_->height();
    const auto offset = right * (-delta.x() * scale) + up * (delta.y() * scale);
    look_->eye += offset;
    look_->center += offset;
    apply_distance(distance());
}
void ViewNavigation::drag_to(QPointF point) {
    if (!moved_ && (point - press_).manhattanLength() >= QApplication::startDragDistance()) {
        moved_ = true;
        window_->setCursor(drag_ == Drag::zoom ? Qt::SizeVerCursor : Qt::ClosedHandCursor);
    }
    if (!moved_)
        return;
    const auto delta = point - previous_;
    const auto height = std::max(1, window_->height());
    if (drag_ == Drag::pan)
        pan(delta);
    else if (drag_ == Drag::rotate) {
        // Screen-relative yaw/pitch avoids the edge-dependent roll of a virtual ball.
        const auto target = look_->center;
        const auto radius = distance();
        rotation_->rotate(-2 * delta.x() / height, {0, 1, 0});
        rotation_->rotate(-2 * delta.y() / height, {1, 0, 0});
        look_->center = target;
        const auto forward = vsg::normalize(look_->center - look_->eye);
        const auto right = vsg::normalize(vsg::cross(forward, look_->up));
        look_->up = vsg::normalize(vsg::cross(right, forward));
        apply_distance(radius);
    } else if (drag_ == Drag::zoom)
        apply_distance(distance() * std::exp(std::clamp(-2 * delta.y() / height, -4.0, 4.0)));
    previous_ = point;
}
void ViewNavigation::finish_drag(bool restore) {
    if (drag_ == Drag::none)
        return;
    if (restore && drag_start_)
        rotation_->setViewpoint(drag_start_, 0);
    drag_ = Drag::none;
    button_ = Qt::NoButton;
    drag_start_ = {};
    if (window_)
        window_->unsetCursor();
    update_projection();
}
bool ViewNavigation::eventFilter(QObject *object, QEvent *event) {
    if (object != window_)
        return false;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton && mouse->button() != Qt::MiddleButton &&
            mouse->button() != Qt::RightButton)
            return false;
        stop();
        button_ = mouse->button();
        drag_ = button_ == Qt::RightButton ? Drag::zoom
                : button_ == Qt::MiddleButton || mouse->modifiers().testFlag(Qt::ShiftModifier)
                    ? Drag::pan
                    : Drag::rotate;
        moved_ = false;
        press_ = previous_ = mouse->position();
        drag_start_ = vsg::LookAt::create(*look_);
        return true;
    }
    case QEvent::MouseMove:
        if (drag_ != Drag::none)
            drag_to(static_cast<QMouseEvent *>(event)->position());
        return true;
    case QEvent::MouseButtonRelease: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (drag_ != Drag::none && mouse->button() == button_) {
            drag_to(mouse->position());
            const bool pick = button_ == Qt::LeftButton && drag_ == Drag::rotate && !moved_ &&
                              QRectF(QPointF{}, window_->size()).contains(mouse->position());
            finish_drag();
            if (pick && select)
                select(mouse->position());
        }
        return true;
    }
    case QEvent::Wheel: {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        const bool pixels = !wheel->pixelDelta().isNull();
        const auto delta = pixels ? wheel->pixelDelta().y() : wheel->angleDelta().y();
        if (delta != 0 && drag_ == Drag::none)
            zoom(std::pow(1.2, std::clamp(delta / (pixels ? 60.0 : 120.0), -8.0, 8.0)), !pixels);
        return true;
    }
    case QEvent::KeyPress: {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            zoom_animation_.stop();
            finish_drag(true);
        } else if (key->key() == Qt::Key_Home) {
            stop();
            if (fit)
                fit();
        } else if (key->key() == Qt::Key_Plus || key->key() == Qt::Key_Equal)
            zoom(1.3);
        else if (key->key() == Qt::Key_Minus)
            zoom(1 / 1.3);
        else if (key->key() >= Qt::Key_Left && key->key() <= Qt::Key_Down) {
            stop();
            const double fraction = key->modifiers().testFlag(Qt::ShiftModifier) ? 0.02 : 0.1;
            switch (key->key()) {
            case Qt::Key_Left:
                pan({window_->width() * fraction, 0});
                break;
            case Qt::Key_Right:
                pan({-window_->width() * fraction, 0});
                break;
            case Qt::Key_Up:
                pan({0, window_->height() * fraction});
                break;
            case Qt::Key_Down:
                pan({0, -window_->height() * fraction});
                break;
            }
        } else
            return false;
        return true;
    }
    case QEvent::FocusOut:
    case QEvent::Hide:
    case QEvent::WindowDeactivate:
    case QEvent::Resize:
        stop();
        break;
    case QEvent::UngrabMouse:
        finish_drag();
        break;
    default:
        break;
    }
    return false;
}
} // namespace orbitforge::desktop
