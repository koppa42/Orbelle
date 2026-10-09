#include <algorithm>
#include <cstring>
#include <limits>
#include <orbitforge/render_vsg/swapchain_capture.hpp>
#include <stdexcept>
#include <string>

namespace orbitforge::render_vsg {
namespace {
void check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
} // namespace

SwapchainCapture::SwapchainCapture(vsg::ref_ptr<vsg::Window> window) : window_(std::move(window)) {
    if (!window_ ||
        !(window_->traits()->swapchainPreferences.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        throw std::invalid_argument("Swapchain capture requires TRANSFER_SRC image usage");
    VkSurfaceCapabilitiesKHR capabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(window_->getOrCreatePhysicalDevice()->vk(),
                                                    window_->getOrCreateSurface()->vk(),
                                                    &capabilities),
          "Query swapchain capture support");
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        throw std::runtime_error("This surface does not support swapchain image readback");
}

void SwapchainCapture::request() {
    if (requested_)
        throw std::logic_error("A swapchain capture is already pending");
    if (!window_->visible() || window_->imageIndex() >= window_->numFrames())
        throw std::runtime_error("Swapchain capture requires an acquired visible image");
    extent_ = window_->extent2D();
    format_ = window_->getSwapchain()->getImageFormat();
    if (format_ != VK_FORMAT_R8G8B8A8_UNORM && format_ != VK_FORMAT_R8G8B8A8_SRGB &&
        format_ != VK_FORMAT_B8G8R8A8_UNORM && format_ != VK_FORMAT_B8G8R8A8_SRGB)
        throw std::runtime_error("Swapchain capture only supports 8-bit RGBA/BGRA surfaces");
    const VkDeviceSize size = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
    if (!size || size > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("Invalid swapchain capture dimensions");
    const auto device = window_->getDevice();
    buffer_ =
        vsg::Buffer::create(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE);
    buffer_->compile(device);
    const auto memory = vsg::DeviceMemory::create(
        device, buffer_->getMemoryRequirements(device->deviceID),
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(buffer_->bind(memory, 0), "Bind swapchain readback buffer");
    recorded_ = false;
    requested_ = true;
}

void SwapchainCapture::record(vsg::CommandBuffer &commands) const {
    if (!requested_)
        return;
    if (recorded_ || commands.deviceID != window_->getDevice()->deviceID ||
        window_->imageIndex() >= window_->numFrames() ||
        window_->extent2D().width != extent_.width || window_->extent2D().height != extent_.height)
        throw std::runtime_error("Swapchain changed during image capture");
    const auto image = window_->imageView(window_->imageIndex())->image->vk(commands.deviceID);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands.vk(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(commands.vk(), image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer_->vk(commands.deviceID), 1, &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier host{};
    host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = buffer_->vk(commands.deviceID);
    host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commands.vk(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 1, &host, 1, &barrier);
    recorded_ = true;
}

CapturedImage SwapchainCapture::read_after_submit(vsg::Fence &fence) {
    if (!requested_ || !recorded_ || fence.getDevice() != window_->getDevice())
        throw std::logic_error("Swapchain capture was not submitted on this device");
    check(fence.wait(10'000'000'000ull), "Wait for swapchain image readback");
    CapturedImage result{extent_.width, extent_.height,
                         std::vector<std::uint8_t>(static_cast<std::size_t>(buffer_->size))};
    auto *memory = buffer_->getDeviceMemory(window_->getDevice()->deviceID);
    void *mapped = nullptr;
    check(memory->map(0, buffer_->size, 0, &mapped), "Map swapchain readback buffer");
    std::memcpy(result.rgba.data(), mapped, result.rgba.size());
    memory->unmap();
    if (format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB)
        for (std::size_t i = 0; i < result.rgba.size(); i += 4)
            std::swap(result.rgba[i], result.rgba[i + 2]);
    requested_ = false;
    return result;
}
} // namespace orbitforge::render_vsg
