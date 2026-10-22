#include "benchmark_gpu.hpp"
#include "benchmark_run.hpp"
#include <QByteArray>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vsg/vk/Queue.h>

namespace orbelle::desktop {
namespace {
class TimestampCommand : public vsg::Inherit<vsg::Command, TimestampCommand> {
  public:
    TimestampCommand(std::shared_ptr<BenchmarkGpu> owner, bool begin)
        : owner_(std::move(owner)), begin_(begin) {}
    void record(vsg::CommandBuffer &commands) const override { owner_->record(commands, begin_); }

  private:
    std::shared_ptr<BenchmarkGpu> owner_;
    bool begin_;
};
} // namespace

BenchmarkGpu::BenchmarkGpu(vsg::ref_ptr<vsg::Device> device, std::uint32_t family, int view,
                           BenchmarkRun &run)
    : device_(std::move(device)), family_(family), view_(view), run_(run) {
    const auto physical = device_->getPhysicalDevice();
    const auto &families = physical->getQueueFamilyProperties();
    if (family_ >= families.size() || !(families[family_].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        throw std::runtime_error("GPU timing requires the view's graphics queue family");
    valid_bits_ = families[family_].timestampValidBits;
    period_ns_ = physical->getProperties().limits.timestampPeriod;
    if (valid_bits_ < 36 || valid_bits_ > 64 || !std::isfinite(period_ns_) || period_ns_ <= 0)
        throw std::runtime_error("This graphics queue cannot provide GPU timestamps");
    pool_ = vsg::QueryPool::create(device_.get(), 0, VK_QUERY_TYPE_TIMESTAMP, slot_count * 2, 0);
}
vsg::ref_ptr<vsg::Command> BenchmarkGpu::command(bool begin) {
    return TimestampCommand::create(shared_from_this(), begin);
}
void BenchmarkGpu::prepare(std::uint64_t frame, vsg::RecordAndSubmitTask &task) {
    if (state_ != State::idle || task.device != device_ || !task.queue ||
        task.queue->queueFamilyIndex() != family_ || !task.fence())
        throw std::logic_error("GPU timing task binding changed or was not submitted");
    const auto fence = task.fence();
    const auto found = std::find_if(slots_.begin(), slots_.end(),
                                    [&](const auto &slot) { return slot.fence == fence; });
    const auto free =
        std::find_if(slots_.begin(), slots_.end(), [](const auto &slot) { return !slot.fence; });
    const auto selected = found != slots_.end() ? found : free;
    if (selected == slots_.end())
        throw std::runtime_error("GPU timing exhausted its fence bindings; run is invalid");
    current_ = static_cast<std::uint32_t>(std::distance(slots_.begin(), selected));
    selected->fence = fence; // Retain identity across task destruction/address reuse.
    next_frame_ = frame;     // Do not overwrite the previous submitted frame before readback.
    prepared_qpc_ = BenchmarkRun::qpc();
    state_ = State::prepared;
}
void BenchmarkGpu::record(vsg::CommandBuffer &commands, bool begin) {
    if (commands.getDevice() != device_.get() ||
        commands.getCommandPool()->queueFamilyIndex != family_)
        throw std::logic_error("GPU timestamps recorded on a different queue/device");
    auto &slot = slots_[current_];
    if (begin) {
        // This callback runs inside task.submit(), after VSG start() completed the
        // previous use of this exact fence. Query availability alone cannot prove
        // reset executed: it may still describe the previous generation.
        if (state_ != State::prepared || slot.fence->hasDependencies())
            throw std::logic_error("GPU timing must record after VSG recycled the task fence");
        if (slot.pending)
            read_slot(current_);
        vkCmdResetQueryPool(commands, pool_->vk(), current_ * 2, 2);
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_->vk(), current_ * 2);
        recording_ = commands.vk();
        state_ = State::begun;
    } else {
        if (state_ != State::begun || recording_ != commands.vk())
            throw std::logic_error("GPU timing needs exactly one begin/end in the same buffer");
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_->vk(),
                            current_ * 2 + 1);
        state_ = State::ended;
    }
}
void BenchmarkGpu::submitted() {
    auto &slot = slots_[current_];
    if (state_ != State::ended || !slot.fence->hasDependencies())
        throw std::runtime_error("The timed view was not recorded/submitted exactly once");
    slot.frame = next_frame_;
    slot.prepared_qpc = prepared_qpc_;
    ++slot.use;
    slot.pending = true;
    state_ = State::idle;
}
void BenchmarkGpu::read_slot(std::uint32_t index) {
    auto &slot = slots_[index];
    struct Result {
        std::uint64_t tick = 0, available = 0;
    };
    std::array<Result, 2> results{};
    // The VSG vector convenience method does not support this 16-byte stride.
    // No WAIT/PARTIAL flags, queue idle, or additional fence wait in measurement.
    const auto result = vkGetQueryPoolResults(
        *device_, pool_->vk(), index * 2, 2, sizeof(results), results.data(), sizeof(Result),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    run_.gpu({slot.frame, slot.prepared_qpc, BenchmarkRun::qpc(), slot.use, results[0].tick,
              results[1].tick, results[0].available, results[1].available, view_, 1,
              static_cast<int>(index), static_cast<int>(result)});
    slot.pending = false;
    if (result != VK_SUCCESS || !results[0].available || !results[1].available)
        throw std::runtime_error(
            "GPU timestamp results unavailable after their submission completed");
}
void BenchmarkGpu::drain_after_device_idle() {
    for (std::uint32_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].pending)
            read_slot(i);
}
QJsonObject BenchmarkGpu::metadata() const {
    const auto id = device_->getPhysicalDevice()
                        ->getProperties<VkPhysicalDeviceIDProperties,
                                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES>();
    return {
        {"view", view_},
        {"generation", 1},
        {"queue_family", static_cast<int>(family_)},
        {"timestamp_valid_bits", static_cast<int>(valid_bits_)},
        {"timestamp_period_ns", period_ns_},
        {"query_slots", static_cast<int>(slot_count)},
        {"device_uuid_hex",
         QString::fromLatin1(
             QByteArray(reinterpret_cast<const char *>(id.deviceUUID), VK_UUID_SIZE).toHex())},
        {"device_luid_hex",
         id.deviceLUIDValid
             ? QString::fromLatin1(
                   QByteArray(reinterpret_cast<const char *>(id.deviceLUID), VK_LUID_SIZE).toHex())
             : QString{}},
        {"scope", "TOP_OF_PIPE before RenderGraph -> BOTTOM_OF_PIPE after RenderGraph; "
                  "GPU elapsed span includes pipeline/queue waits and may overlap other views; "
                  "not exclusive GPU busy time or separate transfer/presentation timing"},
        {"readback",
         "after existing VSG task fence reuse, then final device-idle drain; no new waits"}};
}
} // namespace orbelle::desktop
