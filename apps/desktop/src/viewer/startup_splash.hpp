#pragma once
#include <QSplashScreen>
#include <atomic>
#include <functional>
#include <vector>
class QProgressBar;
class QPushButton;

namespace orbelle::desktop {
class StartupSplash final : public QSplashScreen {
  public:
    explicit StartupSplash(QScreen *screen);
    struct Step {
        QString message;
        std::function<void(const std::function<void()> &checkpoint)> prepare;
    };
    // Runs off the GUI thread while this window stays interactive. Returns false
    // only for cancellation; failures propagate after the worker has been joined.
    bool prepare(const std::vector<Step> &steps);

  protected:
    void drawContents(QPainter *painter) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *) override {} // QSplashScreen normally hides on click.

  private:
    void cancel();
    QProgressBar *progress_ = nullptr;
    QPushButton *cancel_ = nullptr;
    std::atomic_bool cancelled_{false};
    bool preparing_ = false;
};
} // namespace orbelle::desktop
