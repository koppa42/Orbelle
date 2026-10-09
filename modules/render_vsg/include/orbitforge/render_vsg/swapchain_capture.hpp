#pragma once
#include <cstdint>
#include <vector>
#include <vsg/app/Window.h>
#include <vsg/commands/Command.h>
#include <vsg/state/Buffer.h>
#include <vsg/vk/Fence.h>

namespace orbitforge::render_vsg {
struct CapturedImage {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

// Diagnostic readback of a real acquired swapchain image. Place after the window's
// RenderGraph in the same CommandGraph; the normal present semaphore then covers
// both the copy and the return to PRESENT_SRC. All methods use the render thread.
class SwapchainCapture : public vsg::Inherit<vsg::Command, SwapchainCapture> {
  public:
    // Window traits must request TRANSFER_SRC before swapchain creation.
    explicit SwapchainCapture(vsg::ref_ptr<vsg::Window> window);
    void request();
    void record(vsg::CommandBuffer &commands) const override;
    // Pass the current fence of the task that just submitted this CommandGraph.
    // Resources remain owned here if waiting fails, until normal device shutdown.
    [[nodiscard]] CapturedImage read_after_submit(vsg::Fence &fence);

  private:
    vsg::ref_ptr<vsg::Window> window_;
    vsg::ref_ptr<vsg::Buffer> buffer_;
    VkExtent2D extent_{};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    bool requested_ = false;
    mutable bool recorded_ = false;
};
} // namespace orbitforge::render_vsg
