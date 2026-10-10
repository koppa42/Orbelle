#include "welcome_page.hpp"
#include "workbench_style.hpp"
#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace orbitforge::desktop {
namespace {
class WelcomeArtwork final : public QWidget {
  public:
    WelcomeArtwork() : artwork_(":/orbitforge/startup/star-gazing.png") {
        setMinimumHeight(420);
        setMaximumHeight(500);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const auto bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(bounds, 14, 14);
        painter.setClipPath(clip);
        painter.fillRect(rect(), QColor("#151e32"));
        if (!artwork_.isNull()) {
            const auto scale =
                std::max(width() / double(artwork_.width()), height() / double(artwork_.height()));
            const QSizeF source_size(width() / scale, height() / scale);
            const QRectF source((artwork_.width() - source_size.width()) / 2,
                                (artwork_.height() - source_size.height()) / 2, source_size.width(),
                                source_size.height());
            painter.drawPixmap(bounds, artwork_, source);
        }
        QLinearGradient shade(0, 0, width(), 0);
        shade.setColorAt(0, QColor(14, 21, 39, 248));
        shade.setColorAt(0.36, QColor(14, 21, 39, 220));
        shade.setColorAt(0.68, QColor(14, 21, 39, 45));
        shade.setColorAt(1, QColor(14, 21, 39, 10));
        painter.fillRect(rect(), shade);
        painter.setClipping(false);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QColor("#364360"));
        painter.drawRoundedRect(bounds, 14, 14);
    }

  private:
    QPixmap artwork_;
};
} // namespace

QWidget *make_welcome_page(QAction *create, QAction *open, QAction *example) {
    auto *page = new QWidget;
    page->setObjectName("welcome");
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(40, 32, 40, 32);
    outer->addStretch();
    auto *content = new QWidget;
    content->setMaximumWidth(1100);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *stack = new QVBoxLayout(content);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setSpacing(18);
    auto *hero = new WelcomeArtwork;
    auto *hero_layout = new QHBoxLayout(hero);
    hero_layout->setContentsMargins(40, 32, 40, 32);
    auto *copy = new QWidget;
    copy->setMaximumWidth(380);
    auto *layout = new QVBoxLayout(copy);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    layout->addStretch();
    auto *brand = new QLabel("O R B I T F O R G E");
    brand->setObjectName("eyebrow");
    layout->addWidget(brand);
    auto *heading = new QLabel(QStringLiteral("从一个场景开始"));
    heading->setObjectName("welcomeTitle");
    layout->addWidget(heading);
    auto *description = new QLabel(QStringLiteral("创建场景、定义轨道，探索卫星的运动。"));
    description->setObjectName("welcomeDescription");
    description->setWordWrap(true);
    layout->addWidget(description);
    layout->addSpacing(14);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(10);
    auto *new_button = new QPushButton(workbench_icon(WorkbenchIcon::new_scenario, Qt::white),
                                       QStringLiteral("新建场景"));
    new_button->setObjectName("applyOrbit");
    new_button->setMinimumHeight(28);
    new_button->setToolTip(QStringLiteral("新建场景 · Ctrl+N"));
    QObject::connect(new_button, &QPushButton::clicked, create, &QAction::trigger);
    auto *open_button =
        new QPushButton(workbench_icon(WorkbenchIcon::folder), QStringLiteral("打开项目…"));
    open_button->setMinimumHeight(28);
    open_button->setToolTip(QStringLiteral("打开项目 · Ctrl+O"));
    QObject::connect(open_button, &QPushButton::clicked, open, &QAction::trigger);
    actions->addWidget(new_button);
    actions->addWidget(open_button);
    actions->addStretch();
    layout->addLayout(actions);
    auto *example_button = new QPushButton(QStringLiteral("先试试二体示例  →"));
    example_button->setObjectName("welcomeExample");
    QObject::connect(example_button, &QPushButton::clicked, example, &QAction::trigger);
    layout->addWidget(example_button, 0, Qt::AlignLeft);
    layout->addStretch();
    hero_layout->addWidget(copy, 1);
    hero_layout->addStretch(1);
    stack->addWidget(hero);
    auto *footer = new QHBoxLayout;
    auto *caption = new QLabel(QStringLiteral("轨道仿真工作台"));
    caption->setObjectName("welcomeCaption");
    footer->addWidget(caption);
    footer->addStretch();
    auto *shortcuts = new QLabel(QStringLiteral("Ctrl+N  新建场景     /     Ctrl+O  打开项目"));
    shortcuts->setObjectName("welcomeCaption");
    footer->addWidget(shortcuts);
    stack->addLayout(footer);
    auto *center = new QHBoxLayout;
    center->addStretch();
    center->addWidget(content, 100);
    center->addStretch();
    outer->addLayout(center);
    outer->addStretch();
    return page;
}
} // namespace orbitforge::desktop
