#pragma once
#include <QString>
#include <cstddef>

namespace orbitforge::desktop {
struct ReportCurveField {
    std::size_t column;
    QString id, label, unit;
};
} // namespace orbitforge::desktop
