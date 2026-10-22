#pragma once
#include <QString>
#include <cstddef>

namespace orbelle::desktop {
struct ReportCurveField {
    std::size_t column;
    QString id, label, unit;
};
} // namespace orbelle::desktop
