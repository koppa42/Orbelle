#include "../src/viewer/view_navigation.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QThread>
#include <QWheelEvent>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <vsg/app/ProjectionMatrix.h>

namespace orbitforge::desktop {
class ViewNavigationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        if (!QApplication::instance()) {
            static int argc = 1;
            static char executable[] = "navigation_test";
            static char *argv[] = {executable, nullptr};
            static QApplication app(argc, argv);
        }
        window_ = std::make_unique<QWindow>();
        window_->resize(800, 600);
        look_ = vsg::LookAt::create(vsg::dvec3(0, 0, 30e6), vsg::dvec3{}, vsg::dvec3(0, 1, 0));
        camera_ = vsg::Camera::create(vsg::Perspective::create(35, 4.0 / 3, 1000, 1e9), look_,
                                      vsg::ViewportState::create(0, 0, 800, 600));
        navigation_ = std::make_unique<ViewNavigation>(*window_, camera_);
    }
    static void process_for(int ms) {
        QElapsedTimer timer;
        timer.start();
        do {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        } while (timer.elapsed() < ms);
    }
    double distance() const { return vsg::length(look_->eye - look_->center); }
    QPointF project(vsg::dvec3 point) const {
        const auto clip = camera_->projectionMatrix->transform() * look_->transform() *
                          vsg::dvec4(point.x, point.y, point.z, 1);
        return {(clip.x / clip.w + 1) * window_->width() / 2,
                (clip.y / clip.w + 1) * window_->height() / 2};
    }
    void mouse(QEvent::Type type, QPointF point, Qt::MouseButton button = Qt::LeftButton,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, point, point, type == QEvent::MouseMove ? Qt::NoButton : button,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : button, modifiers);
        QCoreApplication::sendEvent(window_.get(), &event);
    }
    void drag(QPointF delta, Qt::MouseButton button = Qt::LeftButton,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const QPointF start(400, 300);
        mouse(QEvent::MouseButtonPress, start, button, modifiers);
        mouse(QEvent::MouseMove, start + delta, button, modifiers);
        mouse(QEvent::MouseButtonRelease, start + delta, button, modifiers);
    }
    void wheel(QPoint angle, QPoint pixel = {}) {
        QWheelEvent event({400, 300}, {400, 300}, pixel, angle, Qt::NoButton, Qt::NoModifier,
                          pixel.isNull() ? Qt::NoScrollPhase : Qt::ScrollUpdate, false);
        QCoreApplication::sendEvent(window_.get(), &event);
    }
    void key(int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent event(QEvent::KeyPress, code, modifiers);
        QCoreApplication::sendEvent(window_.get(), &event);
    }
    std::unique_ptr<QWindow> window_;
    vsg::ref_ptr<vsg::LookAt> look_;
    vsg::ref_ptr<vsg::Camera> camera_;
    std::unique_ptr<ViewNavigation> navigation_;
};
TEST_F(ViewNavigationTest, WheelBurstsPreserveMagnitudeAndReverseImmediately) {
    for (int i = 0; i < 4; ++i)
        wheel({0, 120});
    EXPECT_LT(distance(), 30e6);
    process_for(170);
    EXPECT_NEAR(distance(), 30e6 / std::pow(1.2, 4), 1e-6);
    wheel({0, 60});
    const auto before = distance();
    wheel({0, -60});
    EXPECT_GT(distance(), before);
    process_for(170);
    EXPECT_NEAR(distance(), before * std::sqrt(1.2), 1e-6);
}
TEST_F(ViewNavigationTest, PixelScrollIsProportionalAndHorizontalOrZeroScrollDoesNothing) {
    wheel({0, 120});
    const auto before = distance();
    wheel({0, 120}, {0, 3});
    const auto expected = before / std::pow(1.2, 3.0 / 60);
    EXPECT_NEAR(distance(), expected, 1e-6);
    process_for(170);
    EXPECT_NEAR(distance(), expected, 1e-6);
    wheel({120, 0});
    wheel({});
    wheel({}, {4, 0});
    EXPECT_NEAR(distance(), expected, 1e-6);
}
TEST_F(ViewNavigationTest, RotationStopsOnReleaseAndPreservesTargetAndCameraBasis) {
    const auto start = look_->eye;
    drag({80, 45});
    EXPECT_GT(vsg::length(look_->eye - start), 1e6);
    EXPECT_NEAR(distance(), 30e6, 1e-6);
    EXPECT_NEAR(vsg::length(look_->center), 0, 1e-6);
    EXPECT_NEAR(vsg::length(look_->up), 1, 1e-12);
    EXPECT_NEAR(vsg::dot(vsg::normalize(look_->center - look_->eye), look_->up), 0, 1e-12);
    const auto released = look_->eye;
    process_for(170);
    EXPECT_EQ(look_->eye, released);
}
TEST_F(ViewNavigationTest, PanMovesTheProjectedTargetWithThePointerAtAnyDistance) {
    for (const auto eye_distance : {30e6, 15e6}) {
        navigation_->set_viewpoint(
            vsg::LookAt::create(vsg::dvec3(0, 0, eye_distance), vsg::dvec3{}, vsg::dvec3(0, 1, 0)));
        const auto before = project({});
        drag({55, 35}, Qt::MiddleButton);
        const auto after = project({});
        EXPECT_NEAR(after.x() - before.x(), 55, 1e-9);
        EXPECT_NEAR(after.y() - before.y(), 35, 1e-9);
        EXPECT_NEAR(distance(), eye_distance, 1e-6);
    }
}
TEST_F(ViewNavigationTest, ClickSelectsOnReleaseButDraggingAndShiftClickDoNotSelect) {
    int calls = 0;
    navigation_->select = [&](QPointF) { ++calls; };
    mouse(QEvent::MouseButtonPress, {400, 300});
    EXPECT_EQ(calls, 0);
    mouse(QEvent::MouseMove, {401, 301});
    mouse(QEvent::MouseButtonRelease, {401, 301});
    EXPECT_EQ(calls, 1);
    drag({60, 20});
    drag({60, 20}, Qt::MiddleButton);
    drag({0, 0}, Qt::LeftButton, Qt::ShiftModifier);
    EXPECT_EQ(calls, 1);
}
TEST_F(ViewNavigationTest, EscapeRestoresEachDragAndReleaseDoesNotRestartIt) {
    for (const auto button : {Qt::LeftButton, Qt::MiddleButton, Qt::RightButton}) {
        const auto eye = look_->eye, center = look_->center, up = look_->up;
        mouse(QEvent::MouseButtonPress, {400, 300}, button);
        mouse(QEvent::MouseMove, {450, 350}, button);
        EXPECT_NE(look_->eye, eye);
        key(Qt::Key_Escape);
        mouse(QEvent::MouseButtonRelease, {470, 370}, button);
        EXPECT_EQ(look_->eye, eye);
        EXPECT_EQ(look_->center, center);
        EXPECT_EQ(look_->up, up);
    }
}
TEST_F(ViewNavigationTest, EarthAndFocusedObjectZoomRespectDistanceAndClippingBounds) {
    navigation_->zoom(1e12, false);
    EXPECT_NEAR(distance(), 6378137.0 + 100, 1e-6);
    const auto *projection = dynamic_cast<vsg::Perspective *>(camera_->projectionMatrix.get());
    ASSERT_NE(projection, nullptr);
    EXPECT_LT(projection->nearDistance, 100); // The near plane must not cut into the globe.
    navigation_->zoom(1e-12, false);
    EXPECT_NEAR(distance(), 1e11, 1e-3);
    // An off-center target on the far side must not allow zooming through Earth.
    navigation_->set_viewpoint(
        vsg::LookAt::create(vsg::dvec3(-10e6, 0, 0), vsg::dvec3(7e6, 0, 0), vsg::dvec3(0, 0, 1)));
    navigation_->zoom(1e12, false);
    EXPECT_NEAR(look_->eye.x, -6378237.0, 1e-6);
    navigation_->set_viewpoint(
        vsg::LookAt::create(vsg::dvec3(8e6, 0, 0), vsg::dvec3(7e6, 0, 0), vsg::dvec3(0, 0, 1)));
    navigation_->zoom(1e12, false);
    EXPECT_NEAR(distance(), 1, 1e-6);
    EXPECT_LT(projection->nearDistance, distance());
    EXPECT_GT(vsg::length(look_->eye), 6378137);
    const auto eye = look_->eye;
    for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()})
        navigation_->zoom(invalid);
    EXPECT_EQ(look_->eye, eye);
}
TEST_F(ViewNavigationTest, HomeViewpointFocusLossAndReattachmentCancelPendingNavigation) {
    const auto initial = vsg::LookAt::create(*look_);
    navigation_->fit = [&] { navigation_->set_viewpoint(initial); };
    wheel({0, 120});
    key(Qt::Key_Home);
    process_for(170);
    EXPECT_EQ(look_->eye, initial->eye);
    wheel({0, 120});
    QFocusEvent focus_out(QEvent::FocusOut);
    QCoreApplication::sendEvent(window_.get(), &focus_out);
    const auto stopped = look_->eye;
    process_for(170);
    EXPECT_EQ(look_->eye, stopped);
    navigation_.reset();
    navigation_ = std::make_unique<ViewNavigation>(*window_, camera_);
    EXPECT_EQ(look_->eye, stopped);
    key(Qt::Key_Plus);
    process_for(170);
    EXPECT_LT(distance(), vsg::length(stopped - look_->center));
}
TEST_F(ViewNavigationTest, KeyboardPanHasAPreciseStepAndResizePreservesTheCamera) {
    const auto before = project({});
    key(Qt::Key_Right);
    EXPECT_NEAR(project({}).x() - before.x(), -80, 1e-9);
    key(Qt::Key_Left);
    key(Qt::Key_Down, Qt::ShiftModifier);
    EXPECT_NEAR(project({}).y() - before.y(), -12, 1e-9);
    const auto eye = look_->eye, center = look_->center, up = look_->up;
    window_->resize(500, 400);
    EXPECT_EQ(look_->eye, eye);
    EXPECT_EQ(look_->center, center);
    EXPECT_EQ(look_->up, up);
}
} // namespace orbitforge::desktop
