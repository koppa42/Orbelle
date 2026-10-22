#include "benchmark_run.hpp"
#include "benchmark_memory.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <Psapi.h>
#endif

namespace orbelle::desktop {
namespace {
const char *phase_name(BenchmarkRun::Phase phase) {
    switch (phase) {
    case BenchmarkRun::Phase::waiting:
        return "waiting";
    case BenchmarkRun::Phase::calibration:
        return "calibration";
    case BenchmarkRun::Phase::warmup:
        return "warmup";
    case BenchmarkRun::Phase::stable:
        return "stable";
    case BenchmarkRun::Phase::inputs:
        return "inputs";
    case BenchmarkRun::Phase::drain:
        return "drain";
    case BenchmarkRun::Phase::done:
        return "done";
    }
    throw std::logic_error("Unknown benchmark phase");
}
} // namespace
QEvent::Type BenchmarkInputEvent::event_type() {
    static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
    return type;
}
BenchmarkInputEvent::BenchmarkInputEvent(std::uint64_t value, bool seek_value, std::uint64_t posted)
    : QEvent(event_type()), id(value), posted_qpc(posted), seek(seek_value) {}
BenchmarkRun::BenchmarkRun(QString directory, bool probe, bool output_only_probe)
    : directory_(std::move(directory)), probe_(probe), output_only_probe_(output_only_probe),
      calibration_seconds_(probe ? 1 : 2), warmup_seconds_(probe ? 1 : 10),
      stable_seconds_(probe ? 3 : 60), input_period_seconds_(probe ? 0.125 : 0.5) {
    (void)qpc_frequency();
    if (output_only_probe_ && !probe_)
        throw std::invalid_argument("Output-only timing is restricted to diagnostic probes");
    if (QFileInfo::exists(directory_) || !QDir().mkpath(directory_))
        throw std::runtime_error("Benchmark output directory must be new and writable");
    frames_.reserve(30000);
    presents_.reserve(40000);
    feedback_.reserve(40000);
    feedback_queries_.reserve(40000);
    calibrations_.reserve(120000);
    gpu_.reserve(40000);
}
BenchmarkRun::~BenchmarkRun() { stop_inputs(); }
std::uint64_t BenchmarkRun::qpc() {
#ifdef _WIN32
    LARGE_INTEGER value{};
    if (!QueryPerformanceCounter(&value))
        throw std::runtime_error("QueryPerformanceCounter failed");
    return static_cast<std::uint64_t>(value.QuadPart);
#else
    throw std::runtime_error("Presentation trace timing currently requires Windows QPC");
#endif
}
std::uint64_t BenchmarkRun::qpc_frequency() {
#ifdef _WIN32
    static const auto frequency = [] {
        LARGE_INTEGER value{};
        if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0)
            throw std::runtime_error("QueryPerformanceFrequency failed");
        return static_cast<std::uint64_t>(value.QuadPart);
    }();
    return frequency;
#else
    throw std::runtime_error("Presentation trace timing currently requires Windows QPC");
#endif
}
void BenchmarkRun::write_json(const QString &name, const QJsonObject &object) const {
    QSaveFile file(directory_ + '/' + name);
    const auto bytes = QJsonDocument(object).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot write benchmark report");
}
void BenchmarkRun::ready(const QJsonObject &metadata) {
    auto ready = metadata;
    ready.insert("process_id", QCoreApplication::applicationPid());
    ready.insert("qpc_frequency", static_cast<qint64>(qpc_frequency()));
    ready.insert("ready_qpc", static_cast<qint64>(qpc()));
    ready.insert("profile", probe_ ? "trace-probe" : "timing-characterization-v1");
    if (video_memory_)
        ready.insert("video_memory", video_memory_->metadata());
    write_json("ready.json", ready);
}
BenchmarkRun::Phase BenchmarkRun::phase() {
    if (phase_ == Phase::done)
        return phase_;
    const auto now = qpc();
    if (!start_qpc_) {
        if (!QFileInfo::exists(directory_ + "/go"))
            return Phase::waiting;
        start_qpc_ = now;
    }
    const double elapsed =
        static_cast<double>(now - start_qpc_) / static_cast<double>(qpc_frequency());
    const double stable_end = calibration_seconds_ + warmup_seconds_ + stable_seconds_;
    const double inputs_end = stable_end + trials_per_kind() * 2 * input_period_seconds_ + 0.1;
    const auto next = elapsed < calibration_seconds_                     ? Phase::calibration
                      : elapsed < calibration_seconds_ + warmup_seconds_ ? Phase::warmup
                      : elapsed < stable_end                             ? Phase::stable
                      : elapsed < inputs_end                             ? Phase::inputs
                                                                         : Phase::drain;
    if (next != phase_) {
        phase_ = next;
        std::cout << "benchmark phase: " << phase_name(phase_) << std::endl;
    }
    return phase_;
}
void BenchmarkRun::begin_drain() {
    if (drain_begin_qpc_)
        return;
    const auto count = static_cast<std::uint64_t>(trials_per_kind() * 2);
    if (inputs_.size() != count || active_input_ != count || frames_.empty() ||
        frames_.back().input_id != count)
        throw std::runtime_error("Cannot drain before all inputs have reached a rendered frame");
    for (int view = 0; view < 2; ++view)
        if (std::none_of(presents_.rbegin(), presents_.rend(), [&](const Present &p) {
                return p.frame == frames_.back().id && p.view == view && p.result == 0;
            }))
            throw std::runtime_error(
                "Cannot drain before the last input is presented in both views");
    stop_inputs();
    drain_begin_qpc_ = qpc();
    drain_deadline_qpc_ = drain_begin_qpc_ + 2 * qpc_frequency();
}
bool BenchmarkRun::drain_expired() const {
    return drain_deadline_qpc_ && qpc() >= drain_deadline_qpc_;
}
void BenchmarkRun::end_drain(bool complete) {
    if (!drain_begin_qpc_ || drain_end_qpc_)
        throw std::logic_error("Invalid feedback drain lifecycle");
    drain_end_qpc_ = qpc();
    drain_complete_ = complete;
    phase_ = Phase::done;
    std::cout << "benchmark phase: done" << std::endl;
}
double BenchmarkRun::animation_seconds() const {
    if (!start_qpc_)
        return 0;
    double elapsed = static_cast<double>(qpc() - start_qpc_) / static_cast<double>(qpc_frequency());
    if (phase_ == Phase::stable)
        elapsed -= calibration_seconds_ + warmup_seconds_;
    return std::fmod(elapsed * 10.0, 600.0);
}
void BenchmarkRun::start_inputs(QObject *target) {
    if (input_thread_.joinable())
        return;
    (void)BenchmarkInputEvent::event_type();
    const int count = trials_per_kind();
    const double period = input_period_seconds_;
    input_thread_ = std::jthread([target, count, period](std::stop_token stop) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < count * 2; ++i) {
            // Fixed phase offsets avoid systematically injecting just after a Qt frame tick.
            const auto delay =
                std::chrono::duration<double>(0.05 + i * period + (i * 17 % 47) * 0.001);
            std::this_thread::sleep_until(start + delay);
            if (stop.stop_requested())
                return;
            QCoreApplication::postEvent(
                target,
                new BenchmarkInputEvent(static_cast<std::uint64_t>(i + 1), i < count, qpc()));
        }
    });
}
void BenchmarkRun::stop_inputs() {
    if (input_thread_.joinable()) {
        input_thread_.request_stop();
        input_thread_.join();
    }
}
void BenchmarkRun::input(const BenchmarkInputEvent &event, const QJsonObject &result) {
    auto record = result;
    record.insert("id", static_cast<qint64>(event.id));
    record.insert("kind", event.seek ? "cached-seek" : "selection");
    record.insert("posted_qpc", static_cast<qint64>(event.posted_qpc));
    record.insert("handled_qpc", static_cast<qint64>(qpc()));
    inputs_.push_back(record);
    active_input_ = event.id;
}
void BenchmarkRun::sample_resources(std::uint64_t now) {
    if (last_resource_qpc_ && now - last_resource_qpc_ < qpc_frequency())
        return;
#ifdef _WIN32
    const auto sample_begin = qpc();
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)))
        throw std::runtime_error("Cannot query process memory");
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        throw std::runtime_error("Cannot query process CPU time");
    const auto ticks = [](const FILETIME &time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    const auto cpu = ticks(kernel) + ticks(user);
    QJsonObject item{{"qpc", static_cast<qint64>(now)},
                     {"sample_begin_qpc", static_cast<qint64>(sample_begin)},
                     {"phase", phase_name(phase_)},
                     {"working_set_bytes", static_cast<qint64>(memory.WorkingSetSize)},
                     {"private_bytes", static_cast<qint64>(memory.PrivateUsage)},
                     {"peak_working_set_bytes", static_cast<qint64>(memory.PeakWorkingSetSize)}};
    if (last_resource_qpc_)
        item.insert("cpu_percent_of_one_core", static_cast<double>(cpu - last_cpu_ticks_) / 1e7 /
                                                   (static_cast<double>(now - last_resource_qpc_) /
                                                    static_cast<double>(qpc_frequency())) *
                                                   100);
    if (video_memory_)
        item.insert("video_memory", video_memory_->sample());
    item.insert("sample_end_qpc", static_cast<qint64>(qpc()));
    resources_.push_back(item);
    last_cpu_ticks_ = cpu;
#endif
    last_resource_qpc_ = now;
}
void BenchmarkRun::frame(const Frame &value) {
    frames_.push_back(value);
    sample_resources(value.end_qpc);
}
void BenchmarkRun::present(const Present &value) { presents_.push_back(value); }
void BenchmarkRun::feedback(const Feedback &value) { feedback_.push_back(value); }
void BenchmarkRun::feedback_query(FeedbackQuery value) {
    value.phase = phase_;
    feedback_queries_.push_back(value);
}
void BenchmarkRun::associate_calibrations(std::uint64_t query_id,
                                          std::array<std::uint64_t, 2> ids) {
    for (auto value = feedback_.rbegin(); value != feedback_.rend() && value->query_id == query_id;
         ++value) {
        value->queue_calibration_id = ids[0];
        value->output_calibration_id = ids[1];
    }
}
void BenchmarkRun::timing_properties(const TimingProperties &value) {
    timing_properties_.push_back(value);
}
void BenchmarkRun::calibration(const Calibration &value) { calibrations_.push_back(value); }
void BenchmarkRun::gpu(const Gpu &value) { gpu_.push_back(value); }
void BenchmarkRun::configure_video_memory(const vsg::PhysicalDevice &physical) {
    if (video_memory_)
        throw std::logic_error("Process video memory adapter was already configured");
    video_memory_ = std::make_unique<BenchmarkMemory>(physical);
}
void BenchmarkRun::finish(const QJsonObject &metadata) {
    stop_inputs();
    QFile frames(directory_ + "/frames.csv"), presents(directory_ + "/presents.csv"),
        feedback(directory_ + "/feedback.csv"), calibrations(directory_ + "/calibrations.csv"),
        gpu(directory_ + "/gpu.csv"), queries(directory_ + "/feedback_queries.csv"),
        stages(directory_ + "/feedback_stages.csv"),
        properties(directory_ + "/timing_properties.csv");
    if (!frames.open(QIODevice::WriteOnly) || !presents.open(QIODevice::WriteOnly) ||
        !feedback.open(QIODevice::WriteOnly) || !calibrations.open(QIODevice::WriteOnly) ||
        !gpu.open(QIODevice::WriteOnly) || !queries.open(QIODevice::WriteOnly) ||
        !stages.open(QIODevice::WriteOnly) || !properties.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot write frame timing records");
    QTextStream f(&frames), p(&presents);
    f << "frame,phase,begin_qpc,update_qpc,submit_qpc,end_qpc,epoch_ns,input_id,selected_index\n";
    for (const auto &value : frames_)
        f << value.id << ',' << phase_name(value.phase) << ',' << value.begin_qpc << ','
          << value.update_qpc << ',' << value.submit_qpc << ',' << value.end_qpc << ','
          << value.epoch_ns << ',' << value.input_id << ',' << value.selected_index << '\n';
    p << "frame,view,generation,present_id,enter_qpc,leave_qpc,result\n";
    for (const auto &value : presents_)
        p << value.frame << ',' << value.view << ',' << value.generation << ',' << value.present_id
          << ',' << value.enter_qpc << ',' << value.leave_qpc << ',' << value.result << '\n';
    QTextStream b(&feedback);
    b << "view,generation,present_id,queried_qpc,time_domain,time_domain_id,complete,queue_end_ns,"
         "first_pixel_out_ns,queue_calibration_id,output_calibration_id,query_id,record_index,"
         "target_time,stage_count\n";
    for (const auto &value : feedback_)
        b << value.view << ',' << value.generation << ',' << value.present_id << ','
          << value.queried_qpc << ',' << value.time_domain << ',' << value.time_domain_id << ','
          << value.complete << ',' << value.queue_end_ns << ',' << value.first_pixel_out_ns << ','
          << value.queue_calibration_id << ',' << value.output_calibration_id << ','
          << value.query_id << ',' << value.record_index << ',' << value.target_time << ','
          << value.stage_count << '\n';
    QTextStream q(&queries), s(&stages), t(&properties);
    q << "id,view,generation,phase,before_qpc,after_qpc,result,output_valid,capacity,returned_"
         "count,"
         "flags,time_domains_counter,timing_properties_counter\n";
    for (const auto &value : feedback_queries_)
        q << value.id << ',' << value.view << ',' << value.generation << ','
          << phase_name(value.phase) << ',' << value.before_qpc << ',' << value.after_qpc << ','
          << value.result << ',' << (value.output_valid ? 1 : 0) << ',' << value.capacity << ','
          << value.returned_count << ',' << value.flags << ',' << value.time_domains_counter << ','
          << value.timing_properties_counter << '\n';
    s << "query_id,record_index,stage_index,stage,time_ns\n";
    for (const auto &value : feedback_)
        for (std::uint32_t index = 0; index < std::min(value.stage_count, feedback_stage_capacity);
             ++index)
            s << value.query_id << ',' << value.record_index << ',' << index << ','
              << value.stages[index].stage << ',' << value.stages[index].time << '\n';
    t << "query_id,view,generation,before_qpc,after_qpc,result,output_valid,counter,refresh_"
         "duration,"
         "refresh_interval\n";
    for (const auto &value : timing_properties_)
        t << value.query_id << ',' << value.view << ',' << value.generation << ','
          << value.before_qpc << ',' << value.after_qpc << ',' << value.result << ','
          << (value.output_valid ? 1 : 0) << ',' << value.counter << ',' << value.refresh_duration
          << ',' << value.refresh_interval << '\n';
    QTextStream c(&calibrations);
    c << "id,view,generation,time_domain,time_domain_id,stage,before_qpc,after_qpc,local_ns,"
         "calibrated_qpc,max_deviation_ns\n";
    for (const auto &value : calibrations_)
        c << value.id << ',' << value.view << ',' << value.generation << ',' << value.time_domain
          << ',' << value.time_domain_id << ',' << value.stage << ',' << value.before_qpc << ','
          << value.after_qpc << ',' << value.local_ns << ',' << value.calibrated_qpc << ','
          << value.max_deviation_ns << '\n';
    QTextStream g(&gpu);
    g << "frame,view,generation,slot,slot_use,prepared_qpc,read_qpc,begin_tick,end_tick,"
         "begin_available,end_available,result\n";
    for (const auto &value : gpu_)
        g << value.frame << ',' << value.view << ',' << value.generation << ',' << value.slot << ','
          << value.slot_use << ',' << value.prepared_qpc << ',' << value.read_qpc << ','
          << value.begin_tick << ',' << value.end_tick << ',' << value.begin_available << ','
          << value.end_available << ',' << value.result << '\n';
    f.flush();
    p.flush();
    b.flush();
    c.flush();
    g.flush();
    q.flush();
    s.flush();
    t.flush();
    if (frames.error() != QFile::NoError || presents.error() != QFile::NoError ||
        feedback.error() != QFile::NoError || calibrations.error() != QFile::NoError ||
        gpu.error() != QFile::NoError || queries.error() != QFile::NoError ||
        stages.error() != QFile::NoError || properties.error() != QFile::NoError)
        throw std::runtime_error("Cannot finish timing records");
    QJsonArray inputs, resources;
    for (const auto &value : inputs_)
        inputs.append(value);
    for (const auto &value : resources_)
        resources.append(value);
    auto report = metadata;
    report.insert("profile", probe_ ? "trace-probe" : "timing-characterization-v1");
    report.insert("qpc_frequency", static_cast<qint64>(qpc_frequency()));
    report.insert("start_qpc", static_cast<qint64>(start_qpc_));
    report.insert("finished_qpc", static_cast<qint64>(qpc()));
    report.insert("calibration_seconds", calibration_seconds_);
    report.insert("warmup_seconds", warmup_seconds_);
    report.insert("stable_seconds", stable_seconds_);
    report.insert("trials_per_kind", trials_per_kind());
    report.insert("input_period_seconds", input_period_seconds_);
    report.insert("inputs", inputs);
    report.insert("resources", resources);
    if (video_memory_)
        report.insert("video_memory", video_memory_->metadata());
    report.insert("schema_version", 4);
    report.insert(
        "feedback_drain",
        QJsonObject{{"begin_qpc", static_cast<qint64>(drain_begin_qpc_)},
                    {"end_qpc", static_cast<qint64>(drain_end_qpc_)},
                    {"deadline_qpc", static_cast<qint64>(drain_deadline_qpc_)},
                    {"deadline_policy",
                     "no new feedback query at/after deadline; in-flight calls may finish"},
                    {"timeout_seconds", 2},
                    {"complete", drain_complete_},
                    {"policy", "stop acquisition/submission/presentation, then poll"}});
    report.insert("input_boundary",
                  "Qt posted synthetic action -> VK_EXT_present_timing first pixel output; "
                  "excludes hardware/OS input delivery and physical pixel visibility");
    write_json("timing.json", report);
}
} // namespace orbelle::desktop
