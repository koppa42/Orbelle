#pragma once
#include <functional>
#include <vsg/app/CompileManager.h>

namespace orbitforge::render_vsg {
// A stable manager for Rocky's asynchronous compilation. GUI-thread changes borrow
// every traversal, so no worker can use a window context while it is being replaced.
class CompileManager final : public vsg::Inherit<vsg::CompileManager, CompileManager> {
  public:
    CompileManager(vsg::Viewer &viewer, vsg::ref_ptr<vsg::ResourceHints> hints);
    void begin_change(vsg::Viewer &viewer);
    // Leave the manager paused when no view exists. Reopening a view resumes it.
    void end_change(vsg::Viewer &viewer);
    // GUI-thread operations that may call vkDeviceWaitIdle exclude worker submissions.
    // Nestable, including around structural changes; do not run scene update inside it.
    void run_exclusive(const std::function<void()> &operation);

  private:
    void borrow_traversals();
    void return_traversals_if_ready();
    vsg::ref_ptr<vsg::ResourceHints> hints_;
    CompileTraversals::container_type borrowed_;
    bool paused_ = false;
    std::size_t exclusive_depth_ = 0;
};
} // namespace orbitforge::render_vsg
