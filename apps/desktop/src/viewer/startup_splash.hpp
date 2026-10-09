#pragma once
#include <QSplashScreen>

namespace orbitforge::desktop {
class StartupSplash final : public QSplashScreen {
  public:
    explicit StartupSplash(QScreen *screen);

  protected:
    void drawContents(QPainter *painter) override;
};
} // namespace orbitforge::desktop
