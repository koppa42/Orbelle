#include "../src/viewer/apply_timing.hpp"
#include <QTemporaryDir>
#include <future>
#include <gtest/gtest.h>

namespace orbelle::desktop {
namespace {
class TimingOutput {
  public:
    explicit TimingOutput(const QString &path) : previous_(qgetenv("ORBELLE_APPLY_TIMINGS")) {
        qputenv("ORBELLE_APPLY_TIMINGS", path.toUtf8());
    }
    ~TimingOutput() {
        if (previous_.isNull())
            qunsetenv("ORBELLE_APPLY_TIMINGS");
        else
            qputenv("ORBELLE_APPLY_TIMINGS", previous_);
    }

  private:
    QByteArray previous_;
};
} // namespace
TEST(ApplyTiming, DisabledByDefaultAndUnwritableOutputDoesNotRejectAnEdit) {
    const TimingOutput disabled({});
    EXPECT_FALSE(ApplyTiming::begin("orbit_apply", 1));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const TimingOutput unwritable(directory.path()); // A directory cannot be appended as a file.
    const auto timing = ApplyTiming::begin("orbit_apply", 2);
    ASSERT_TRUE(timing);
    EXPECT_NO_THROW(timing->finish("applied"));
}
TEST(ApplyTiming, WorkerFailureIsTimedAndRecordsAppendAsSeparateJsonLines) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("apply.jsonl");
    const TimingOutput output(path);
    auto unchanged = ApplyTiming::begin("orbit_apply", 1);
    unchanged->requested();
    unchanged->finish("unchanged");
    auto failed = ApplyTiming::begin("orbit_apply", 2);
    failed->asynchronous = true;
    failed->route = "single_orbit";
    failed->requested();
    auto future = std::async(std::launch::async, [failed] {
        const ApplyWorkerTiming worker(failed);
        const StageTiming sampling(&failed->preparation.sampling_ms);
        ++failed->preparation.sample_calls;
        throw std::runtime_error("sampling rejected");
    });
    EXPECT_THROW(future.get(), std::runtime_error);
    failed->ready();
    failed->finish("rejected");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto first = QJsonDocument::fromJson(file.readLine()).object();
    const auto second = QJsonDocument::fromJson(file.readLine()).object();
    EXPECT_EQ(first["outcome"].toString(), "unchanged");
    EXPECT_EQ(first["sample_calls"].toInt(), 0);
    EXPECT_EQ(second["sequence"].toInt(), 2);
    EXPECT_EQ(second["outcome"].toString(), "rejected");
    EXPECT_EQ(second["sample_calls"].toInt(), 1);
    EXPECT_GT(second["stages_ms"].toObject()["propagation_sampling_ms"].toDouble(), 0);
    EXPECT_GE(second["worker_ms"].toDouble(),
              second["stages_ms"].toObject()["propagation_sampling_ms"].toDouble());
    EXPECT_GE(second["total_ms"].toDouble(), second["worker_ms"].toDouble());
    EXPECT_TRUE(file.atEnd());
}
} // namespace orbelle::desktop
