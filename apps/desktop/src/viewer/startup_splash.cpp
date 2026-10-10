#include "startup_splash.hpp"
#include "workbench_style.hpp"
#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QScreen>
#include <algorithm>
#include <stdexcept>

namespace orbitforge::desktop {
namespace {
QPixmap splash_picture(QScreen *screen) {
    const QPixmap artwork(QStringLiteral(":/orbitforge/startup/star-gazing.png"));
    if (artwork.isNull())
        throw std::runtime_error("Startup artwork is missing from the application resources");
    const auto available = screen ? screen->availableGeometry().size() : QSize(1280, 800);
    const auto scale =
        std::min({1.0, available.width() * 0.78 / 960, available.height() * 0.78 / 540});
    const auto ratio = screen ? screen->devicePixelRatio() : 1.0;
    const QSize pixels(qRound(960 * scale * ratio), qRound(540 * scale * ratio));
    auto result = artwork.scaled(pixels, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    result = result.copy((result.width() - pixels.width()) / 2,
                         (result.height() - pixels.height()) / 2, pixels.width(), pixels.height());
    result.setDevicePixelRatio(ratio);
    return result;
}
} // namespace
StartupSplash::StartupSplash(QScreen *screen) : QSplashScreen(screen, splash_picture(screen)) {
    setObjectName("startupSplash");
    setWindowTitle(QStringLiteral("OrbitForge · 启动"));
    setWindowIcon(workbench_icon(WorkbenchIcon::orbit));
    if (screen)
        move(screen->availableGeometry().center() - rect().center());
}
void StartupSplash::drawContents(QPainter *painter) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->scale(width() / 960.0, height() / 540.0);
    QLinearGradient left(0, 0, 580, 0);
    left.setColorAt(0, QColor(6, 12, 24, 100));
    left.setColorAt(1, QColor(6, 12, 24, 0));
    painter->fillRect(QRectF(0, 0, 960, 540), left);
    QLinearGradient footer(0, 434, 0, 540);
    footer.setColorAt(0, QColor(6, 12, 24, 0));
    footer.setColorAt(1, QColor(6, 12, 24, 140));
    painter->fillRect(QRectF(0, 434, 960, 106), footer);

    QFont font;
    font.setFamilies({"Microsoft YaHei UI", "Segoe UI"});
    font.setPixelSize(42);
    font.setWeight(QFont::DemiBold);
    painter->setFont(font);
    painter->setPen(QColor("#f0f6ff"));
    painter->drawText(QRectF(44, 37, 540, 64), Qt::AlignLeft | Qt::AlignVCenter, "OrbitForge");
    font.setPixelSize(15);
    font.setWeight(QFont::Normal);
    painter->setFont(font);
    painter->setPen(QColor("#bdd0e8"));
    painter->drawText(QRectF(46, 99, 420, 28), Qt::AlignLeft | Qt::AlignVCenter,
                      QStringLiteral("轨道仿真工作台"));

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor("#94c9ec"));
    painter->drawEllipse(QPointF(48, 496), 2.5, 2.5);
    font.setPixelSize(13);
    painter->setFont(font);
    painter->setPen(QColor("#b4c7dd"));
    painter->drawText(QRectF(60, 480, 800, 32), Qt::AlignLeft | Qt::AlignVCenter, message());
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(QColor(200, 220, 240, 45), 1));
    painter->drawRect(QRectF(0.5, 0.5, 959, 539));
    painter->restore();
}
} // namespace orbitforge::desktop
