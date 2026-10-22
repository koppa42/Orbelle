#include "startup_splash.hpp"
#include "../ui_text.hpp"
#include "workbench_style.hpp"
#include <QCloseEvent>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QTimer>
#include <algorithm>
#include <future>
#include <stdexcept>

namespace orbelle::desktop {
namespace {
QPixmap splash_picture(QScreen *screen) {
    const QDir directory(QStringLiteral(":/orbelle/startup"));
    const auto pictures = directory.entryList({QStringLiteral("*.png")}, QDir::Files, QDir::Name);
    if (pictures.isEmpty())
        throw std::runtime_error("Startup artwork is missing from the application resources");
    const auto index = QRandomGenerator::global()->bounded(static_cast<int>(pictures.size()));
    const QPixmap artwork(directory.filePath(pictures[index]));
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
    setWindowTitle(ui::text("startup_splash.label"));
    setWindowIcon(workbench_icon(WorkbenchIcon::orbit, theme::color(theme::accent)));
    if (screen)
        move(screen->availableGeometry().center() - rect().center());
    progress_ = new QProgressBar(this);
    progress_->setObjectName("startupProgress");
    progress_->setTextVisible(false);
    progress_->setAccessibleName(ui::text("startup_splash.progress"));
    progress_->hide();
    cancel_ = new QPushButton(ui::text("startup_splash.cancel"), this);
    cancel_->setObjectName("startupCancel");
    cancel_->hide();
    connect(cancel_, &QPushButton::clicked, this, &StartupSplash::cancel);
}
void StartupSplash::resizeEvent(QResizeEvent *event) {
    QSplashScreen::resizeEvent(event);
    const double scale = width() / 960.0;
    if (progress_)
        progress_->setGeometry(qRound(48 * scale), qRound(518 * scale), qRound(864 * scale),
                               std::max(4, qRound(6 * scale)));
    if (cancel_)
        cancel_->setGeometry(qRound(814 * scale), qRound(480 * scale), qRound(98 * scale),
                             qRound(30 * scale));
}
void StartupSplash::cancel() {
    if (!preparing_)
        return;
    cancelled_ = true;
    cancel_->setEnabled(false);
    showMessage(ui::text("startup_splash.cancelling"));
}
void StartupSplash::closeEvent(QCloseEvent *event) {
    if (preparing_) {
        cancel();
        event->ignore();
    } else {
        QSplashScreen::closeEvent(event);
    }
}
void StartupSplash::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape && preparing_) {
        cancel();
        event->accept();
    } else {
        QSplashScreen::keyPressEvent(event);
    }
}
bool StartupSplash::prepare(const std::vector<Step> &steps) {
    struct Cancelled {};
    cancelled_ = false;
    preparing_ = true;
    progress_->setRange(0, std::max(1, static_cast<int>(steps.size())));
    progress_->setValue(0);
    progress_->show();
    cancel_->setEnabled(true);
    cancel_->show();
    if (!isVisible())
        show();
    std::atomic_size_t completed{0};
    auto worker = std::async(std::launch::async, [&] {
        const std::function<void()> checkpoint = [&] {
            if (cancelled_.load())
                throw Cancelled{};
        };
        for (const auto &step : steps) {
            checkpoint();
            step.prepare(checkpoint);
            checkpoint();
            ++completed;
        }
    });
    QEventLoop loop;
    QTimer poll;
    connect(&poll, &QTimer::timeout, &loop, [&] {
        const auto count = completed.load();
        progress_->setValue(static_cast<int>(count));
        if (!cancelled_ && count < steps.size()) {
            const auto status = ui::text("startup_splash.stage")
                                    .arg(count + 1)
                                    .arg(steps.size())
                                    .arg(steps[count].message);
            if (message() != status)
                showMessage(status);
        }
        if (worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            loop.quit();
    });
    if (!steps.empty())
        showMessage(
            ui::text("startup_splash.stage").arg(1).arg(steps.size()).arg(steps[0].message));
    poll.start(30);
    loop.exec();
    // Quit events can end a nested loop before the worker. Cancel and join before
    // destroying anything captured by reference, including on application exit.
    if (worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        cancelled_ = true;
    preparing_ = false;
    cancel_->hide();
    try {
        worker.get();
    } catch (const Cancelled &) {
        return false;
    }
    if (cancelled_)
        return false;
    progress_->setValue(progress_->maximum());
    showMessage(ui::text("startup_splash.ready"));
    return true;
}
void StartupSplash::drawContents(QPainter *painter) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->scale(width() / 960.0, height() / 540.0);
    QLinearGradient left(0, 0, 580, 0);
    left.setColorAt(0, theme::color(theme::canvas, 150));
    left.setColorAt(1, theme::color(theme::canvas, 0));
    painter->fillRect(QRectF(0, 0, 960, 540), left);
    QLinearGradient footer(0, 434, 0, 540);
    footer.setColorAt(0, theme::color(theme::canvas, 0));
    footer.setColorAt(1, theme::color(theme::canvas, 170));
    painter->fillRect(QRectF(0, 434, 960, 106), footer);

    painter->drawPixmap(QPointF(44, 46), brand_mark(40, painter->device()->devicePixelRatioF()));
    QFont font;
    font.setFamilies({"Microsoft YaHei UI", "Segoe UI"});
    font.setPixelSize(40);
    font.setWeight(QFont::DemiBold);
    painter->setFont(font);
    painter->setPen(theme::color(theme::text));
    painter->drawText(QRectF(98, 37, 520, 58), Qt::AlignLeft | Qt::AlignVCenter,
                      ui::text("brand.name"));
    paint_sakura_blossom(*painter, QPointF(906, 48), 11, theme::color(theme::sakura, 200));
    paint_sakura_blossom(*painter, QPointF(878, 34), 5, theme::color(theme::sakura, 120));

    // Ice-to-sakura hairline above the status message.
    QLinearGradient rule(48, 0, 368, 0);
    rule.setColorAt(0, theme::color(theme::accent, 220));
    rule.setColorAt(1, theme::color(theme::sakura, 0));
    painter->setPen(QPen(QBrush(rule), 1));
    painter->drawLine(QPointF(48, 474), QPointF(368, 474));
    painter->setPen(Qt::NoPen);
    painter->setBrush(theme::color(theme::sakura));
    painter->drawEllipse(QPointF(50, 496), 3, 3);
    font.setPixelSize(13);
    font.setWeight(QFont::Normal);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    painter->setFont(font);
    painter->setPen(theme::color(theme::text_soft));
    painter->drawText(QRectF(62, 480, 740, 32), Qt::AlignLeft | Qt::AlignVCenter, message());
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(theme::color(theme::accent, 50), 1));
    painter->drawRect(QRectF(0.5, 0.5, 959, 539));
    painter->restore();
}
} // namespace orbelle::desktop
