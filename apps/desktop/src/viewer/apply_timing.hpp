#pragma once
#include "preparation_timing.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <memory>

namespace orbelle::desktop {
// Opt-in, local JSONL output; diagnostics never prevent applying a valid edit.
struct ApplyTiming {
    using Clock = StageTiming::Clock;
    QString path, operation, route = "inline";
    qint64 sequence = 0;
    Clock::time_point started = Clock::now(), worker_finished{};
    PreparationTiming preparation;
    double request_ms = 0, resources_ms = 0, worker_ms = 0, publish_ms = 0, dispatch_ms = 0;
    bool asynchronous = false;

    static std::shared_ptr<ApplyTiming> begin(const QString &operation, qint64 sequence) {
        const auto path = qEnvironmentVariable("ORBELLE_APPLY_TIMINGS");
        if (path.isEmpty())
            return {};
        auto timing = std::make_shared<ApplyTiming>();
        timing->path = path;
        timing->operation = operation;
        timing->sequence = sequence;
        return timing;
    }
    void requested() {
        request_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    }
    void ready() {
        if (worker_finished != Clock::time_point{})
            dispatch_ms =
                std::chrono::duration<double, std::milli>(Clock::now() - worker_finished).count();
    }
    void finish(const QString &outcome) const {
        const QJsonObject stages{{"request_ms", request_ms},
                                 {"resources_ms", resources_ms},
                                 {"session_ms", preparation.session_ms},
                                 {"propagation_sampling_ms", preparation.sampling_ms},
                                 {"display_ms", preparation.display_ms},
                                 {"models_ms", preparation.models_ms},
                                 {"dispatch_ms", dispatch_ms},
                                 {"publish_ms", publish_ms}};
        const QJsonObject record{
            {"schema", 1},
            {"process_id", QCoreApplication::applicationPid()},
            {"sequence", sequence},
            {"operation", operation},
            {"route", route},
            {"finished_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"outcome", outcome},
            {"asynchronous", asynchronous},
            {"stages_ms", stages},
            {"worker_ms", worker_ms},
            {"total_ms", std::chrono::duration<double, std::milli>(Clock::now() - started).count()},
            {"sessions_acquired", static_cast<qint64>(preparation.sessions_acquired)},
            {"sessions_reused", static_cast<qint64>(preparation.sessions_reused)},
            {"sample_calls", static_cast<qint64>(preparation.sample_calls)}};
        QFile output(path);
        const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
        if (!output.open(QIODevice::WriteOnly | QIODevice::Append) ||
            output.write(bytes) != bytes.size() || !output.flush())
            std::fprintf(stderr, "Cannot write ORBELLE_APPLY_TIMINGS: %s\n",
                         qPrintable(output.errorString()));
    }
};
// Record completion on success, cancellation and exceptions, before the future becomes ready.
class ApplyWorkerTiming {
  public:
    explicit ApplyWorkerTiming(const std::shared_ptr<ApplyTiming> &timing)
        : timing_(timing), stage_(timing ? &timing->worker_ms : nullptr) {}
    ~ApplyWorkerTiming() {
        stage_.stop();
        if (timing_)
            timing_->worker_finished = ApplyTiming::Clock::now();
    }

  private:
    std::shared_ptr<ApplyTiming> timing_;
    StageTiming stage_;
};
} // namespace orbelle::desktop
