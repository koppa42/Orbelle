#pragma once

#include "../../src/viewer/eop_catalog.hpp"
#include "property_editor_fixture.hpp"
#include <QEventLoop>
#include <QTimer>

namespace orbitforge::desktop {
inline void settle_hpop_catalog(EopCatalog &catalog) {
    EXPECT_TRUE(wait_for_ui([&] { return !catalog.downloading(); }))
        << catalog.status().toStdString();
    QEventLoop settled;
    QTimer::singleShot(150, &settled, &QEventLoop::quit);
    settled.exec();
}
} // namespace orbitforge::desktop
