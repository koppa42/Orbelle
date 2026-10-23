#include "application_icon.hpp"
#include <QPixmapCache>
#include <QResource>
#include <stdexcept>

// Keep the resource object when linking this static library.
static void initialize_icon_resource() { Q_INIT_RESOURCE(application_icon); }

namespace orbelle::desktop {
QPixmap brand_mark(int size, qreal device_pixel_ratio) {
    static const bool initialized = (initialize_icon_resource(), true);
    (void)initialized;
    const auto key = QStringLiteral("orbelle.brand.%1.%2").arg(size).arg(device_pixel_ratio);
    QPixmap pixels;
    if (QPixmapCache::find(key, &pixels))
        return pixels;
    const QPixmap source(QStringLiteral(":/orbelle/icons/orbelle.png"));
    if (source.isNull())
        throw std::runtime_error("Application icon is missing from the embedded resources");
    pixels = source.scaled(QSize(size, size) * device_pixel_ratio, Qt::KeepAspectRatio,
                           Qt::SmoothTransformation);
    pixels.setDevicePixelRatio(device_pixel_ratio);
    QPixmapCache::insert(key, pixels);
    return pixels;
}

QIcon application_icon() {
    QIcon icon;
    for (const int size : {16, 20, 24, 32, 40, 48, 64, 128, 256, 512})
        icon.addPixmap(brand_mark(size));
    return icon;
}
} // namespace orbelle::desktop
