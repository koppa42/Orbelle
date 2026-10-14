#include "welcome_page.hpp"
#include "workbench_style.hpp"
#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>

namespace orbitforge::desktop {
namespace {
class WelcomeArtwork final : public QWidget {
  public:
    WelcomeArtwork() : artwork_(":/orbitforge/artwork/orbital-welcome.png") {
        setMinimumHeight(460);
        setMaximumHeight(540);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    QBoxLayout *actions = nullptr;
    QHBoxLayout *content = nullptr;

  protected:
    void resizeEvent(QResizeEvent *) override {
        const bool narrow = width() < 620;
        if (actions)
            actions->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        if (content) {
            content->setContentsMargins(narrow ? 24 : 40, 32, narrow ? 24 : 40, 32);
            content->setStretch(1, narrow ? 0 : 1);
        }
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const auto bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(bounds, 20, 20);
        painter.setClipPath(clip);
        painter.fillRect(rect(), QColor("#172638"));
        if (!artwork_.isNull()) {
            const auto scale =
                std::max(width() / double(artwork_.width()), height() / double(artwork_.height()));
            const QSizeF source_size(width() / scale, height() / scale);
            const QRectF source((artwork_.width() - source_size.width()) * 0.78,
                                (artwork_.height() - source_size.height()) * 0.15,
                                source_size.width(), source_size.height());
            painter.drawPixmap(bounds, artwork_, source);
        }
        QLinearGradient shade(0, 0, width(), 0);
        shade.setColorAt(0, QColor(15, 27, 41, 245));
        shade.setColorAt(0.38, QColor(15, 27, 41, 205));
        shade.setColorAt(0.72, QColor(15, 27, 41, width() < 620 ? 140 : 35));
        shade.setColorAt(1, QColor(15, 27, 41, width() < 620 ? 100 : 8));
        painter.fillRect(rect(), shade);
        // Static celestial accents keep the welcome page quiet while it is idle.
        painter.setPen(QPen(QColor(220, 190, 240, 55), 1));
        painter.drawEllipse(QPointF(width() - 78, 78), 35, 35);
        painter.drawEllipse(QPointF(width() - 78, 78), 43, 43);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#ecc4df"));
        const QPointF star(width() - 78, 78);
        painter.drawPolygon(QPolygonF{star + QPointF(0, -10), star + QPointF(3, -3),
                                      star + QPointF(10, 0), star + QPointF(3, 3),
                                      star + QPointF(0, 10), star + QPointF(-3, 3),
                                      star + QPointF(-10, 0), star + QPointF(-3, -3)});
        painter.setClipping(false);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QColor("#3c566b"));
        painter.drawRoundedRect(bounds, 20, 20);
    }

  private:
    QPixmap artwork_;
};

class WelcomeFooter final : public QWidget {
  public:
    WelcomeFooter() {
        auto *caption = new QLabel(QStringLiteral("星夜相伴 · 探索有据"));
        caption->setObjectName("welcomeCaption");
        auto *shortcuts = new QLabel(QStringLiteral("Ctrl+N 新建  /  Ctrl+O 打开"));
        shortcuts->setObjectName("welcomeCaption");
        layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(8);
        layout_->addWidget(caption, 1);
        layout_->addWidget(shortcuts, 0, Qt::AlignRight);
    }

  protected:
    void resizeEvent(QResizeEvent *) override {
        layout_->setDirection(width() < 620 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    }

  private:
    QBoxLayout *layout_ = nullptr;
};
} // namespace

QWidget *make_welcome_page(QAction *create, QAction *open, QAction *example) {
    auto *scroll = new QScrollArea;
    scroll->setObjectName("welcomeScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget;
    page->setObjectName("welcome");
    scroll->setWidget(page);
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(24, 28, 24, 28);
    outer->addStretch();
    auto *content = new QWidget;
    content->setMaximumWidth(1140);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *stack = new QVBoxLayout(content);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setSpacing(18);
    auto *hero = new WelcomeArtwork;
    auto *hero_layout = new QHBoxLayout(hero);
    hero->content = hero_layout;
    hero_layout->setContentsMargins(40, 32, 40, 32);
    auto *copy = new QWidget;
    copy->setMaximumWidth(420);
    auto *layout = new QVBoxLayout(copy);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    layout->addStretch();
    auto *brand = new QLabel("O R B I T F O R G E");
    brand->setObjectName("eyebrow");
    layout->addWidget(brand);
    auto *heading =
        new QLabel(QStringLiteral("下一站，<br><span style='color:#ecc4df'>你的轨道。</span>"));
    heading->setObjectName("welcomeTitle");
    heading->setWordWrap(true);
    layout->addWidget(heading);
    auto *description =
        new QLabel(QStringLiteral("从一颗卫星开始，探索轨道、可见性与每一次交会。"));
    description->setObjectName("welcomeDescription");
    description->setWordWrap(true);
    layout->addWidget(description);
    layout->addSpacing(14);
    auto *actions = new QHBoxLayout;
    hero->actions = actions;
    actions->setSpacing(10);
    auto *new_button = new QPushButton(workbench_icon(WorkbenchIcon::new_scenario, Qt::white),
                                       QStringLiteral("新建场景"));
    new_button->setObjectName("welcomeCreate");
    new_button->setProperty("primary", true);
    new_button->setMinimumHeight(28);
    new_button->setToolTip(QStringLiteral("新建场景 · Ctrl+N"));
    QObject::connect(new_button, &QPushButton::clicked, create, &QAction::trigger);
    auto *open_button =
        new QPushButton(workbench_icon(WorkbenchIcon::folder), QStringLiteral("打开项目…"));
    open_button->setObjectName("welcomeOpen");
    open_button->setMinimumHeight(28);
    open_button->setToolTip(QStringLiteral("打开项目 · Ctrl+O"));
    QObject::connect(open_button, &QPushButton::clicked, open, &QAction::trigger);
    actions->addWidget(new_button);
    actions->addWidget(open_button);
    layout->addLayout(actions);
    auto *example_button = new QPushButton(QStringLiteral("先试试二体示例  →"));
    example_button->setObjectName("welcomeExample");
    QObject::connect(example_button, &QPushButton::clicked, example, &QAction::trigger);
    layout->addWidget(example_button, 0, Qt::AlignLeft);
    layout->addStretch();
    hero_layout->addWidget(copy, 1);
    hero_layout->addStretch(1);
    stack->addWidget(hero);
    stack->addWidget(new WelcomeFooter);
    auto *center = new QHBoxLayout;
    center->addStretch();
    center->addWidget(content, 100);
    center->addStretch();
    outer->addLayout(center);
    outer->addStretch();
    return scroll;
}
} // namespace orbitforge::desktop
