#pragma once
#include "workload.hpp"
#include <QJsonObject>
#include <QString>
#include <vsg/text/Font.h>

namespace orbelle::desktop {
// Optional developer fixture. It does not add a propagator or a desktop-control API.
class BenchmarkScene {
  public:
    BenchmarkScene(const QString &manifest, const core::IFrameProvider &frames,
                   const core::IEarthOrientationProvider &eop);
    std::unique_ptr<benchmarks::DisplayCache> cache;
    vsg::ref_ptr<vsg::Font> font;
    QJsonObject report;
};
} // namespace orbelle::desktop
