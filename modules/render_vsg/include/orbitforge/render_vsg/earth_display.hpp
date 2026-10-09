#pragma once
#include <array>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/app/WindowTraits.h>
#include <vsg/nodes/Node.h>

namespace orbitforge::render_vsg {
// Renderer-side contract. It provides no terrain heights for physical analysis.
// GUI-thread use. Stop background work before releasing views or compile resources.
class IEarthDisplay {
  public:
    virtual ~IEarthDisplay() = default;
    [[nodiscard]] virtual vsg::ref_ptr<vsg::Node> node() const = 0;
    virtual void prepare_window(vsg::WindowTraits &traits, bool validation) = 0;
    virtual void prepare_view(vsg::View &view) = 0;
    virtual void release_view(const vsg::View &view) = 0;
    // Unit Earth-to-Sun direction in the displayed Earth-fixed frame. Visual only.
    virtual void set_illumination(const std::array<double, 3> &sun_direction, bool enabled) = 0;
    // Final shutdown only. Idempotent; waits for jobs while their dependencies are alive.
    virtual void stop_background_work() = 0;
};
} // namespace orbitforge::render_vsg
