#include "benchmark_scene.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <stdexcept>
#include <vsgXchange/freetype.h>

namespace orbitforge::desktop {
namespace {
QByteArray checked_file(const QString &path, const QByteArray &expected) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Benchmark resource missing: " + path.toStdString());
    const auto bytes = file.readAll();
    if (file.error() != QFile::NoError ||
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != expected)
        throw std::runtime_error("Benchmark resource SHA256 mismatch: " + path.toStdString());
    return bytes;
}
} // namespace
BenchmarkScene::BenchmarkScene(const QString &manifest, const core::IFrameProvider &frames,
                               const core::IEarthOrientationProvider &eop) {
    // This fixture implements one frozen workload, not arbitrary JSON parameters.
    // Changes require a new reviewed fixture/hash, preventing silently ignored options.
    const QByteArray scenario_hash =
        "785a954e4cf4498d07100f678c2db809e41ed77b76a161610aaa692a5e6b9f36";
    const auto scenario = QJsonDocument::fromJson(checked_file(manifest, scenario_hash)).object();
    const auto font_path =
        QCoreApplication::applicationDirPath() + "/resources/font/RobotoMono-Regular.ttf";
    const auto font_hash =
        scenario["workload"].toObject()["labels"].toObject()["fontSha256"].toString();
    (void)checked_file(font_path, font_hash.toLatin1());
    font =
        vsgXchange::freetype::create()->read(vsg::Path(font_path.toStdWString())).cast<vsg::Font>();
    if (!font)
        throw std::runtime_error("Cannot load the fixed benchmark font");
    QElapsedTimer timer;
    timer.start();
    cache = std::make_unique<benchmarks::DisplayCache>(frames, eop);
    report = {{"scenario_sha256", QString::fromLatin1(scenario_hash)},
              {"fixture", "circular-100x100-v1"},
              {"satellites", static_cast<qint64>(benchmarks::Constellation::count)},
              {"tracks_submitted_per_view", 100},
              {"vertices_per_track", 121},
              {"labels_submitted_per_view", 200},
              {"cache_build_ms", static_cast<double>(timer.nsecsElapsed()) / 1e6},
              {"cache_position_bytes", static_cast<qint64>(cache->position_bytes())},
              {"font_sha256", font_hash},
              {"scope", "fixed-workload integration preflight; performance unmeasured"}};
}
} // namespace orbitforge::desktop
