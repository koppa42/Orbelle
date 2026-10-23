#pragma once
#include <QIcon>
#include <QPixmap>

namespace orbelle::desktop {
QIcon application_icon();
// Shared application artwork, sized in logical pixels for the destination screen.
QPixmap brand_mark(int size, qreal device_pixel_ratio = 1.0);
} // namespace orbelle::desktop
