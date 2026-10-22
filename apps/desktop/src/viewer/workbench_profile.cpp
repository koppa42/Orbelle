#include "workbench_profile.hpp"
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace orbelle::desktop {
WorkbenchProfile *WorkbenchProfile::active = nullptr;
WorkbenchProfile::WorkbenchProfile(QString output) : output_(std::move(output)) {
    if (QFile::exists(output_) || !QDir().mkpath(output_))
        throw std::invalid_argument("Workbench profile requires a new output directory");
    active = this;
}
WorkbenchProfile::~WorkbenchProfile() { active = nullptr; }
void WorkbenchProfile::phase(const QString &name, bool recording) {
    if (name != phase_ || recording != recording_)
        cadence_.invalidate();
    phase_ = name;
    recording_ = recording;
}
void WorkbenchProfile::tick() {
    if (cadence_.isValid())
        record("frame_interval_ms", cadence_.nsecsElapsed() * 1e-6);
    cadence_.start();
}
void WorkbenchProfile::record(const char *name, double milliseconds) {
    if (recording_)
        samples_[phase_][QString::fromLatin1(name)].push_back(milliseconds);
    else if (QString::fromLatin1(name) == "map_tracks_ms")
        samples_["warmup-" + phase_][QString::fromLatin1(name)].push_back(milliseconds);
}
void WorkbenchProfile::finish(const QJsonObject &metadata) {
    QJsonObject phases;
    for (auto &[phase, metrics] : samples_) {
        QJsonObject summary;
        for (auto &[metric, values] : metrics) {
            std::sort(values.begin(), values.end());
            summary[metric] = QJsonObject{
                {"count", static_cast<qint64>(values.size())},
                {"mean", std::accumulate(values.begin(), values.end(), 0.0) / values.size()},
                {"p50", values[(values.size() - 1) / 2]},
                {"p95", values[(values.size() - 1) * 95 / 100]},
                {"max", values.back()}};
        }
        phases[phase] = summary;
    }
    QFile file(output_ + "/profile.json");
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(
            QJsonDocument(QJsonObject{{"metadata", metadata}, {"phases", phases}}).toJson()) < 0)
        throw std::runtime_error("Cannot write workbench profile");
    recording_ = false;
}
void WorkbenchProfile::capture(const QString &name, const QImage &image) const {
    if (!image.save(output_ + "/" + name + ".png"))
        throw std::runtime_error("Cannot save workbench profile image");
}
} // namespace orbelle::desktop
