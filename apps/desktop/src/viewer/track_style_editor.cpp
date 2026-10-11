#include "track_style_editor.hpp"
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyleOptionSpinBox>
#include <QToolButton>

namespace orbitforge::desktop {
namespace {
class LineWidthSpinBox final : public QDoubleSpinBox {
    void paintEvent(QPaintEvent *event) override {
        QDoubleSpinBox::paintEvent(event);
        // Fusion + the workbench stylesheet can leave spin glyphs blank. Draw
        // vector chevrons in the actual hit rectangles, preserving native stepping.
        QStyleOptionSpinBox option;
        initStyleOption(&option);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        for (const bool up : {true, false}) {
            const auto control = up ? QStyle::SC_SpinBoxUp : QStyle::SC_SpinBoxDown;
            const auto box = style()->subControlRect(QStyle::CC_SpinBox, &option, control, this);
            const QPointF center = QRectF(box).center();
            const bool enabled =
                isEnabled() && (stepEnabled() & (up ? StepUpEnabled : StepDownEnabled));
            painter.setPen(QPen(QColor(enabled ? "#aebdd5" : "#56657b"), 1.3, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            const double direction = up ? 1 : -1;
            painter.drawPolyline(QPolygonF{center + QPointF(-3, direction * 1.5),
                                           center + QPointF(0, -direction * 1.5),
                                           center + QPointF(3, direction * 1.5)});
        }
    }
};
class Preview final : public QWidget {
  public:
    DisplaySettings settings;
    explicit Preview(QWidget *parent) : QWidget(parent) {
        setObjectName("trackStylePreview");
        setMinimumHeight(88);
        setAccessibleName(QStringLiteral("轨迹样式预览"));
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#303b51"), 1));
        p.setBrush(QColor("#171f2e"));
        p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 8, 8);
        p.setPen(QColor("#8d9db7"));
        p.drawText(QRect(16, 10, width() - 32, 20), Qt::AlignLeft, QStringLiteral("预览"));
        QPainterPath path;
        path.moveTo(20, 65);
        path.cubicTo(width() * .30, 65, width() * .55, 34, width() - 22, 44);
        const QColor color = QColor::fromRgb(settings.line_color);
        p.setPen(track_pen(color, settings.line_width, settings.line_style));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(QPen(QColor("#171f2e"), 2));
        p.setBrush(color);
        p.drawEllipse(path.pointAtPercent(.66), 4.5, 4.5);
    }
};
} // namespace
TrackStyleEditor::TrackStyleEditor(QWidget *parent) : QWidget(parent) {
    setObjectName("trackStyleEditor");
    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setHorizontalSpacing(16);
    layout->setVerticalSpacing(12);
    preview_ = new Preview(this);
    layout->addWidget(preview_, 0, 0, 1, 2);
    layout->addWidget(new QLabel(QStringLiteral("颜色")), 1, 0, 1, 2);
    auto *palette = new QHBoxLayout;
    palette->setSpacing(8);
    const std::array colors{"#73cdee", "#a9a1ee", "#e9a2ba", "#edbb7a", "#87d4b1", "#e4eaf5"};
    const std::array names{QStringLiteral("浅蓝"), QStringLiteral("淡紫"), QStringLiteral("樱粉"),
                           QStringLiteral("暖金"), QStringLiteral("薄荷"), QStringLiteral("月白")};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        auto *swatch = new QToolButton;
        swatch->setObjectName(QStringLiteral("trackColor%1").arg(i));
        swatch->setFixedSize(28, 28);
        swatch->setCheckable(true);
        swatch->setAccessibleName(names[i]);
        swatch->setToolTip(names[i]);
        swatch->setProperty("rgb", QColor(colors[i]));
        swatch->setStyleSheet(
            QStringLiteral(
                "QToolButton { background: %1; border: 3px solid #1b2333; border-radius: 7px; }"
                "QToolButton:hover { border-color: #70829c; }"
                "QToolButton:checked, QToolButton:focus { border-color: #e2e7ff; }")
                .arg(colors[i]));
        connect(swatch, &QToolButton::clicked, this,
                [this, swatch] { set_color(swatch->property("rgb").value<QColor>()); });
        palette->addWidget(swatch);
        swatches_.push_back(swatch);
    }
    palette->addStretch();
    custom_ = new QPushButton;
    custom_->setObjectName("trackCustomColor");
    custom_->setMinimumWidth(124);
    custom_->setToolTip(QStringLiteral("选择自定义颜色"));
    custom_->setAccessibleName(QStringLiteral("自定义轨迹颜色"));
    palette->addWidget(custom_);
    connect(custom_, &QPushButton::clicked, this, [this] {
        QColorDialog dialog(color_, this);
        dialog.setObjectName("trackColorDialog");
        dialog.setWindowTitle(QStringLiteral("轨迹颜色"));
        dialog.setOption(QColorDialog::DontUseNativeDialog);
        if (dialog.exec() == QDialog::Accepted)
            set_color(dialog.selectedColor());
    });
    layout->addLayout(palette, 2, 0, 1, 2);
    auto *style_label = new QLabel(QStringLiteral("线型"));
    auto *width_label = new QLabel(QStringLiteral("粗细"));
    layout->addWidget(style_label, 3, 0);
    layout->addWidget(width_label, 3, 1);
    style_ = new QComboBox;
    style_->setObjectName("trackLineStyle");
    style_->setIconSize(QSize(64, 18));
    const std::array styles{QStringLiteral("实线"), QStringLiteral("虚线"), QStringLiteral("点线"),
                            QStringLiteral("点划线")};
    for (std::size_t i = 0; i < styles.size(); ++i) {
        QPixmap icon(192, 54);
        icon.setDevicePixelRatio(3);
        icon.fill(Qt::transparent);
        QPainter p(&icon);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(track_pen(QColor("#c9d4ea"), 1.5, static_cast<application::LineStyle>(i)));
        p.drawLine(QPointF(2, 9), QPointF(62, 9));
        p.end();
        style_->addItem(QIcon(icon), styles[i], static_cast<int>(i));
    }
    width_ = new LineWidthSpinBox;
    width_->setObjectName("trackLineWidth");
    width_->setProperty("stepButtons", true);
    width_->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    width_->setRange(.5, 8);
    width_->setSingleStep(.5);
    width_->setDecimals(2);
    width_->setSuffix(QStringLiteral(" px"));
    width_->setToolTip(QStringLiteral("屏幕逻辑像素；缩放视图时保持粗细。"));
    width_->setKeyboardTracking(false);
    style_label->setBuddy(style_);
    width_label->setBuddy(width_);
    layout->addWidget(style_, 4, 0);
    layout->addWidget(width_, 4, 1);
    layout->setColumnStretch(0, 3);
    layout->setColumnStretch(1, 2);
    connect(style_, &QComboBox::currentIndexChanged, this, [this] { refresh_preview(); });
    connect(width_, &QDoubleSpinBox::valueChanged, this, [this] { refresh_preview(); });
    set_settings({});
}
void TrackStyleEditor::set_color(QColor color) {
    if (!color.isValid())
        return;
    color_ = color;
    custom_->setText(color.name().toUpper() + QStringLiteral("  …"));
    for (auto *swatch : swatches_)
        swatch->setChecked(swatch->property("rgb").value<QColor>() == color);
    refresh_preview();
}
void TrackStyleEditor::set_settings(const DisplaySettings &settings) {
    style_->setCurrentIndex(static_cast<int>(settings.line_style));
    width_->setValue(settings.line_width);
    set_color(QColor::fromRgb(settings.line_color));
}
void TrackStyleEditor::write_settings(DisplaySettings &settings) const {
    settings.line_color = color_.rgb() & 0xffffff;
    settings.line_width = width_->value();
    settings.line_style = static_cast<application::LineStyle>(style_->currentIndex());
}
void TrackStyleEditor::refresh_preview() {
    auto *preview = static_cast<Preview *>(preview_);
    write_settings(preview->settings);
    preview->update();
}
} // namespace orbitforge::desktop
