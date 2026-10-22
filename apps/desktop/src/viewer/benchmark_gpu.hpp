#pragma once
#include <QJsonObject>
#include <array>
#include <cstdint>
#include <memory>
#include <vsg/app/RecordAndSubmitTask.h>
#include <vsg/commands/Command.h>
#include <vsg/state/QueryPool.h>
#include <vsg/vk/Fence.h>

namespace orbelle::desktop {
class BenchmarkRun;

// Benchmark-only, GUI-thread recorder. Bracket one RenderGraph with command(true)
// and command(false). prepare() precedes its task.submit(); submitted() follows a
// successful submit. VSG start() waits/recycles that task's fence before record().
class BenchmarkGpu : public std::enable_shared_from_this<BenchmarkGpu> {
  public:
    BenchmarkGpu(vsg::ref_ptr<vsg::Device> device, std::uint32_t family, int view,
                 BenchmarkRun &run);
    [[nodiscard]] vsg::ref_ptr<vsg::Command> command(bool begin);
    void prepare(std::uint64_t frame, vsg::RecordAndSubmitTask &task);
    void record(vsg::CommandBuffer &commands, bool begin);
    void submitted();
    // Only after the existing viewer.deviceWaitIdle(), before destroying graphs.
    void drain_after_device_idle();
    [[nodiscard]] QJsonObject metadata() const;

  private:
    static constexpr std::uint32_t slot_count = 8;
    enum class State { idle, prepared, begun, ended };
    struct Slot {
        vsg::ref_ptr<vsg::Fence> fence;
        std::uint64_t frame = 0, prepared_qpc = 0, use = 0;
        bool pending = false;
    };
    void read_slot(std::uint32_t index);
    vsg::ref_ptr<vsg::Device> device_;
    vsg::ref_ptr<vsg::QueryPool> pool_;
    std::array<Slot, slot_count> slots_;
    std::uint32_t family_, valid_bits_, current_ = 0;
    int view_;
    double period_ns_;
    BenchmarkRun &run_;
    State state_ = State::idle;
    std::uint64_t next_frame_ = 0, prepared_qpc_ = 0;
    VkCommandBuffer recording_ = VK_NULL_HANDLE;
};
} // namespace orbelle::desktop
