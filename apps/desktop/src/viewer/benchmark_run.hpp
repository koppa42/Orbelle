#pragma once
#include <QEvent>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <array>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace vsg {
class PhysicalDevice;
}
namespace orbelle::desktop {
class BenchmarkMemory;
class BenchmarkInputEvent final : public QEvent {
  public:
    static QEvent::Type event_type();
    BenchmarkInputEvent(std::uint64_t id, bool seek, std::uint64_t posted);
    const std::uint64_t id, posted_qpc;
    const bool seek;
};
class BenchmarkRun {
  public:
    enum class Phase { waiting, calibration, warmup, stable, inputs, drain, done };
    static constexpr std::uint32_t feedback_stage_capacity = 4;
    struct Frame {
        std::uint64_t id, begin_qpc, update_qpc, submit_qpc, end_qpc, input_id;
        std::int64_t epoch_ns, selected_index;
        Phase phase;
    };
    struct Present {
        std::uint64_t frame, enter_qpc, leave_qpc;
        std::uint64_t present_id;
        int view, generation, result;
    };
    struct Feedback {
        std::uint64_t present_id, queried_qpc, queue_end_ns, first_pixel_out_ns, time_domain_id;
        int view, generation, time_domain;
        std::uint32_t complete;
        std::uint64_t queue_calibration_id = 0, output_calibration_id = 0;
        std::uint64_t query_id = 0, target_time = 0;
        std::uint32_t record_index = 0, stage_count = 0;
        struct Stage {
            std::uint32_t stage = 0;
            std::uint64_t time = 0;
        };
        std::array<Stage, feedback_stage_capacity> stages{};
    };
    struct FeedbackQuery {
        std::uint64_t id, before_qpc, after_qpc, time_domains_counter, timing_properties_counter;
        std::uint32_t capacity, returned_count, flags;
        int view, generation, result;
        bool output_valid;
        Phase phase = Phase::waiting;
    };
    struct TimingProperties {
        std::uint64_t query_id, before_qpc, after_qpc, counter, refresh_duration, refresh_interval;
        int view, generation, result;
        bool output_valid;
    };
    struct Calibration {
        std::uint64_t id, time_domain_id, before_qpc, after_qpc, local_ns, calibrated_qpc,
            max_deviation_ns;
        int view, generation, time_domain, stage;
    };
    struct Gpu {
        std::uint64_t frame, prepared_qpc, read_qpc, slot_use, begin_tick, end_tick,
            begin_available, end_available;
        int view, generation, slot, result;
    };
    BenchmarkRun(QString directory, bool probe, bool output_only_probe = false);
    ~BenchmarkRun();
    [[nodiscard]] static std::uint64_t qpc();
    [[nodiscard]] static std::uint64_t qpc_frequency();
    [[nodiscard]] Phase phase();
    [[nodiscard]] double animation_seconds() const;
    void ready(const QJsonObject &metadata);
    void start_inputs(QObject *target);
    void stop_inputs();
    void input(const BenchmarkInputEvent &event, const QJsonObject &result);
    void frame(const Frame &value);
    void present(const Present &value);
    void feedback(const Feedback &value);
    void feedback_query(FeedbackQuery value);
    void associate_calibrations(std::uint64_t query_id, std::array<std::uint64_t, 2> ids);
    void timing_properties(const TimingProperties &value);
    void begin_drain();
    [[nodiscard]] bool drain_expired() const;
    [[nodiscard]] std::uint64_t drain_deadline_qpc() const { return drain_deadline_qpc_; }
    void end_drain(bool complete);
    void calibration(const Calibration &value);
    void gpu(const Gpu &value);
    void configure_video_memory(const vsg::PhysicalDevice &physical);
    void finish(const QJsonObject &metadata);
    [[nodiscard]] std::uint64_t active_input_id() const { return active_input_; }
    [[nodiscard]] bool is_probe() const { return probe_; }
    [[nodiscard]] bool output_only_probe() const { return output_only_probe_; }
    [[nodiscard]] int trials_per_kind() const { return probe_ ? 4 : 100; }

  private:
    void sample_resources(std::uint64_t now);
    void write_json(const QString &filename, const QJsonObject &object) const;
    QString directory_;
    bool probe_, output_only_probe_;
    Phase phase_ = Phase::waiting;
    std::uint64_t start_qpc_ = 0, active_input_ = 0, last_resource_qpc_ = 0;
    std::uint64_t last_cpu_ticks_ = 0;
    std::uint64_t drain_begin_qpc_ = 0, drain_end_qpc_ = 0, drain_deadline_qpc_ = 0;
    bool drain_complete_ = false;
    double calibration_seconds_, warmup_seconds_, stable_seconds_, input_period_seconds_;
    std::vector<Frame> frames_;
    std::vector<Present> presents_;
    std::vector<Feedback> feedback_;
    std::vector<FeedbackQuery> feedback_queries_;
    std::vector<TimingProperties> timing_properties_;
    std::vector<Calibration> calibrations_;
    std::vector<Gpu> gpu_;
    std::vector<QJsonObject> inputs_, resources_;
    std::jthread input_thread_;
    std::unique_ptr<BenchmarkMemory> video_memory_;
};
} // namespace orbelle::desktop
