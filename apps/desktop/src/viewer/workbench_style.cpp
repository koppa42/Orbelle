#include "workbench_style.hpp"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDockWidget>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QToolButton>

namespace orbitforge::desktop {
namespace {
class WorkbenchStyle final : public QProxyStyle {
  public:
    WorkbenchStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}
    void polish(QWidget *widget) override {
        QProxyStyle::polish(widget);
        if (auto *number = qobject_cast<QAbstractSpinBox *>(widget))
            number->setButtonSymbols(widget->property("stepButtons").toBool()
                                         ? QAbstractSpinBox::UpDownArrows
                                         : QAbstractSpinBox::NoButtons);
    }
    int pixelMetric(PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override {
        if (metric == PM_IndicatorWidth || metric == PM_IndicatorHeight ||
            metric == PM_ExclusiveIndicatorWidth || metric == PM_ExclusiveIndicatorHeight)
            return 16;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override {
        if (element != PE_IndicatorCheckBox && element != PE_IndicatorRadioButton) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        const bool enabled = option->state & State_Enabled;
        const bool checked = option->state & (State_On | State_NoChange);
        const bool active = option->state & (State_MouseOver | State_HasFocus);
        const QRectF box = QRectF(option->rect).adjusted(1, 1, -1, -1);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(QColor(!enabled            ? "#3b4357"
                                    : checked || active ? "#a3afff"
                                                        : "#66718a"),
                             1.2));
        painter->setBrush(QColor(!enabled ? "#222837" : checked ? "#7b88d9" : "#171d2b"));
        if (element == PE_IndicatorRadioButton) {
            painter->drawEllipse(box);
            if (checked) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(QColor(enabled ? "#ffffff" : "#778198"));
                painter->drawEllipse(box.center(), 2.7, 2.7);
            }
        } else {
            painter->drawRoundedRect(box, 3, 3);
            if (checked) {
                painter->setPen(QPen(QColor(enabled ? "#ffffff" : "#778198"), 1.8, Qt::SolidLine,
                                     Qt::RoundCap, Qt::RoundJoin));
                const auto center = box.center();
                if (option->state & State_NoChange)
                    painter->drawLine(center + QPointF(-3, 0), center + QPointF(3, 0));
                else {
                    QPainterPath check;
                    check.moveTo(center + QPointF(-3.5, 0));
                    check.lineTo(center + QPointF(-1, 2.5));
                    check.lineTo(center + QPointF(4, -3));
                    painter->drawPath(check);
                }
            }
        }
        painter->restore();
    }
};
} // namespace
QIcon workbench_icon(WorkbenchIcon icon, const QColor &color) {
    // Original vector geometry, painted at multiple sizes for native high-DPI controls.
    QIcon result;
    for (int size : {16, 20, 24, 32, 48, 64}) {
        QPixmap pixels(size, size);
        pixels.fill(Qt::transparent);
        QPainter p(&pixels);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(size / 24.0, size / 24.0);
        p.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const auto line = [&](double x, double y, double a, double b) {
            p.drawLine(QPointF(x, y), QPointF(a, b));
        };
        switch (icon) {
        case WorkbenchIcon::folder: {
            QPainterPath path;
            path.moveTo(3, 19);
            path.lineTo(3, 5);
            path.lineTo(9, 5);
            path.lineTo(12, 8);
            path.lineTo(21, 8);
            path.lineTo(21, 19);
            path.closeSubpath();
            p.drawPath(path);
            line(3, 11, 21, 11);
            break;
        }
        case WorkbenchIcon::save:
            p.drawRoundedRect(QRectF(4, 3, 16, 18), 2, 2);
            p.drawRect(QRectF(8, 3, 8, 6));
            p.drawRect(QRectF(8, 14, 8, 7));
            break;
        case WorkbenchIcon::download:
            line(12, 3, 12, 15);
            line(7, 10, 12, 15);
            line(12, 15, 17, 10);
            line(4, 15, 4, 21);
            line(4, 21, 20, 21);
            line(20, 21, 20, 15);
            break;
        case WorkbenchIcon::database:
            p.drawEllipse(QRectF(4, 3, 16, 6));
            line(4, 6, 4, 18);
            line(20, 6, 20, 18);
            p.drawArc(QRectF(4, 9, 16, 6), 180 * 16, 180 * 16);
            p.drawArc(QRectF(4, 15, 16, 6), 180 * 16, 180 * 16);
            break;
        case WorkbenchIcon::new_scenario:
            line(5, 3, 14, 3);
            line(14, 3, 19, 8);
            line(19, 8, 19, 11);
            line(14, 3, 14, 8);
            line(14, 8, 19, 8);
            line(5, 3, 5, 21);
            line(5, 21, 12, 21);
            line(13, 17, 21, 17);
            line(17, 13, 17, 21);
            break;
        case WorkbenchIcon::chevron_up:
        case WorkbenchIcon::chevron_down:
            if (icon == WorkbenchIcon::chevron_up) {
                p.translate(0, 24);
                p.scale(1, -1);
            }
            line(6, 9, 12, 15);
            line(12, 15, 18, 9);
            break;
        case WorkbenchIcon::zoom_in:
        case WorkbenchIcon::zoom_out:
            p.drawEllipse(QRectF(3, 3, 13, 13));
            line(14, 14, 21, 21);
            line(6, 9.5, 13, 9.5);
            if (icon == WorkbenchIcon::zoom_in)
                line(9.5, 6, 9.5, 13);
            break;
        case WorkbenchIcon::time_fit:
            line(3, 5, 3, 19);
            line(21, 5, 21, 19);
            line(7, 12, 17, 12);
            line(10, 9, 7, 12);
            line(10, 15, 7, 12);
            line(14, 9, 17, 12);
            line(14, 15, 17, 12);
            break;
        case WorkbenchIcon::calendar:
            p.drawRoundedRect(QRectF(4, 5, 16, 16), 2, 2);
            line(4, 10, 20, 10);
            line(8, 3, 8, 7);
            line(16, 3, 16, 7);
            for (int y : {13, 17})
                for (int x : {8, 12, 16})
                    p.drawPoint(QPointF(x, y));
            break;
        case WorkbenchIcon::clock:
            p.drawEllipse(QRectF(3, 3, 18, 18));
            line(12, 6, 12, 12);
            line(12, 12, 16, 14);
            break;
        case WorkbenchIcon::orbit:
            p.drawEllipse(QRectF(5, 5, 14, 14));
            p.save();
            p.translate(12, 12);
            p.rotate(-32);
            p.drawEllipse(QRectF(-11, -4, 22, 8));
            p.restore();
            p.setBrush(color);
            p.drawEllipse(QPointF(19.8, 6.8), 2, 2);
            break;
        case WorkbenchIcon::earth:
            p.drawEllipse(QRectF(3, 3, 18, 18));
            p.drawEllipse(QRectF(8, 3, 8, 18));
            line(3, 12, 21, 12);
            p.drawArc(QRectF(3, 6, 18, 12), 0, 180 * 16);
            break;
        case WorkbenchIcon::map:
            line(3, 5, 9, 3);
            line(9, 3, 15, 6);
            line(15, 6, 21, 4);
            line(21, 4, 21, 19);
            line(21, 19, 15, 21);
            line(15, 21, 9, 18);
            line(9, 18, 3, 20);
            line(3, 20, 3, 5);
            line(9, 3, 9, 18);
            line(15, 6, 15, 21);
            break;
        case WorkbenchIcon::ground_station:
            p.drawArc(QRectF(4, 3, 13, 13), 180 * 16, 180 * 16);
            line(4, 9.5, 17, 9.5);
            line(10, 10, 16, 4);
            p.drawEllipse(QPointF(17, 3), 1, 1);
            line(10, 16, 8, 21);
            line(11, 16, 14, 21);
            line(5, 21, 17, 21);
            break;
        case WorkbenchIcon::satellite:
            p.save();
            p.translate(12, 12);
            p.rotate(-35);
            p.drawRect(QRectF(-3, -4, 6, 8));
            p.drawRect(QRectF(-10, -3, 5, 6));
            p.drawRect(QRectF(5, -3, 5, 6));
            line(-5, 0, -3, 0);
            line(3, 0, 5, 0);
            line(0, -4, 0, -7);
            line(-2, -8, 2, -8);
            p.restore();
            break;
        case WorkbenchIcon::properties:
            for (int y : {6, 12, 18})
                line(4, y, 20, y);
            p.setBrush(color);
            p.drawEllipse(QPointF(9, 6), 2, 2);
            p.drawEllipse(QPointF(16, 12), 2, 2);
            p.drawEllipse(QPointF(8, 18), 2, 2);
            break;
        case WorkbenchIcon::fit:
            line(4, 9, 4, 4);
            line(4, 4, 9, 4);
            line(15, 4, 20, 4);
            line(20, 4, 20, 9);
            line(4, 15, 4, 20);
            line(4, 20, 9, 20);
            line(15, 20, 20, 20);
            line(20, 20, 20, 15);
            p.drawEllipse(QRectF(8, 8, 8, 8));
            break;
        case WorkbenchIcon::focus:
            p.drawEllipse(QRectF(6, 6, 12, 12));
            p.drawEllipse(QRectF(10, 10, 4, 4));
            line(12, 2, 12, 6);
            line(12, 18, 12, 22);
            line(2, 12, 6, 12);
            line(18, 12, 22, 12);
            break;
        case WorkbenchIcon::views:
            p.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
            line(14, 4, 14, 20);
            break;
        case WorkbenchIcon::play: {
            QPainterPath path;
            path.moveTo(8, 5);
            path.lineTo(19, 12);
            path.lineTo(8, 19);
            path.closeSubpath();
            p.fillPath(path, color);
            break;
        }
        case WorkbenchIcon::pause:
            p.fillRect(QRectF(7, 5, 3, 14), color);
            p.fillRect(QRectF(14, 5, 3, 14), color);
            break;
        case WorkbenchIcon::first:
        case WorkbenchIcon::last: {
            if (icon == WorkbenchIcon::last) {
                p.translate(24, 0);
                p.scale(-1, 1);
            }
            line(5, 6, 5, 18);
            QPainterPath path;
            path.moveTo(18, 5);
            path.lineTo(8, 12);
            path.lineTo(18, 19);
            path.closeSubpath();
            p.fillPath(path, color);
            break;
        }
        case WorkbenchIcon::search:
            p.drawEllipse(QRectF(4, 3, 12, 12));
            line(14, 14, 21, 21);
            break;
        case WorkbenchIcon::slower:
        case WorkbenchIcon::faster:
            if (icon == WorkbenchIcon::slower) {
                p.translate(24, 0);
                p.scale(-1, 1);
            }
            line(4, 6, 10, 12);
            line(10, 12, 4, 18);
            line(13, 6, 19, 12);
            line(19, 12, 13, 18);
            break;
        case WorkbenchIcon::close:
            line(7, 7, 17, 17);
            line(7, 17, 17, 7);
            break;
        case WorkbenchIcon::detach:
            p.drawRoundedRect(QRectF(4, 8, 12, 12), 1, 1);
            line(10, 4, 20, 4);
            line(20, 4, 20, 14);
            line(13, 11, 20, 4);
            break;
        }
        p.end();
        result.addPixmap(pixels);
    }
    return result;
}
void style_dock(QDockWidget *dock, WorkbenchIcon icon) {
    auto *bar = new QWidget(dock);
    bar->setObjectName("dockHeading");
    bar->setFixedHeight(34);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 6, 5, 6);
    layout->setSpacing(6);
    auto *symbol = new QLabel;
    symbol->setPixmap(workbench_icon(icon).pixmap(16, 16));
    auto *title = new QLabel(dock->windowTitle());
    title->setObjectName("dockTitle");
    layout->addWidget(symbol);
    layout->addWidget(title);
    layout->addStretch();
    auto *detach = new QToolButton;
    detach->setIcon(workbench_icon(WorkbenchIcon::detach));
    detach->setToolTip(QStringLiteral("浮动 / 停靠面板"));
    detach->setFixedSize(22, 22);
    detach->setIconSize(QSize(15, 15));
    auto *close = new QToolButton;
    close->setIcon(workbench_icon(WorkbenchIcon::close));
    close->setToolTip(QStringLiteral("隐藏面板 · 可从视图菜单恢复"));
    close->setFixedSize(22, 22);
    close->setIconSize(QSize(15, 15));
    layout->addWidget(detach);
    layout->addWidget(close);
    QObject::connect(detach, &QToolButton::clicked, dock,
                     [dock] { dock->setFloating(!dock->isFloating()); });
    QObject::connect(close, &QToolButton::clicked, dock, &QWidget::hide);
    dock->setTitleBarWidget(bar);
}
void apply_workbench_style(QApplication &app) {
    app.setStyle(new WorkbenchStyle);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#1a2030"));
    palette.setColor(QPalette::WindowText, QColor("#e0e5f2"));
    palette.setColor(QPalette::Base, QColor("#161c2a"));
    palette.setColor(QPalette::AlternateBase, QColor("#242d40"));
    palette.setColor(QPalette::Text, QColor("#e0e5f2"));
    palette.setColor(QPalette::Button, QColor("#252d40"));
    palette.setColor(QPalette::ButtonText, QColor("#c4cde0"));
    palette.setColor(QPalette::Highlight, QColor("#343d61"));
    palette.setColor(QPalette::HighlightedText, QColor("#edf0ff"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#6e7990"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#6e7990"));
    app.setPalette(palette);
    QFont font;
    font.setFamilies({"Microsoft YaHei UI", "Segoe UI"});
    font.setStyleHint(QFont::SansSerif);
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setPixelSize(13);
    app.setFont(font);
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Han, "Microsoft YaHei UI");
    app.setWindowIcon(workbench_icon(WorkbenchIcon::orbit, QColor("#a3afff")));
    app.setStyleSheet(QStringLiteral(R"QSS(
QWidget { font-family: "Microsoft YaHei UI"; font-size: 13px; }
QDialog { background: #1c2333; }
QWidget#propertyFields { background: #1c2333; }
QWidget#welcome { background: #121827; }
QMainWindow { background: #0e1420; }
QMainWindow::separator { background: #0e1420; width: 4px; height: 4px; }
QMainWindow::separator:hover { background: #506e88; }
QWidget#dockHeading { background: #161c2a; border-bottom: 1px solid #2e374b; }
QWidget#dockHeading QToolButton { padding: 0; }
QLabel#dockTitle { color: #b6c3d3; font-weight: 600; }
QWidget#viewHeading { background: #1c2333; border-bottom: 1px solid #2e374b; }
QLabel#viewTitle { color: #b9c7d7; font-weight: 600; }
QWidget#viewHeading QLabel#hint { color: #92a4b8; }
QWidget#viewHeading QLabel#badge { background: #243548; border-color: #39526b; color: #b4cee5; }
QLabel#badge { color: #9bc7ed; background: #2c354c; border: 1px solid #3c5269; border-radius: 5px; padding: 3px 8px; font-size: 10px; }
QLabel#muted { color: #a1aec7; }
QLabel#brand { color: #e2e9f1; font-size: 13px; font-weight: 600; padding: 0 2px 0 2px; }
QLabel#workspaceName { color: #a1aec7; padding: 0 8px 0 4px; }
QLabel#sectionTitle { color: #b7c3e5; font-size: 13px; font-weight: 600; padding-top: 4px; padding-bottom: 4px; }
QLabel#panelTitle { color: #eff2fc; font-size: 20px; font-weight: 600; }
QLabel#panelDescription { color: #9ba8c1; font-size: 12px; }
QLabel#hint { color: #9ba8c1; font-size: 12px; }
QToolButton#timeReadout { color: #e9b975; font-family: "Consolas"; font-size: 13px; padding: 4px 8px; }
QLabel#timelineTitle { color: #b7c3e5; font-weight: 600; }
QToolBar#timelineBar QToolButton { padding: 3px; }
QToolBar#timelineBar QComboBox, QToolBar#timelineBar QDoubleSpinBox { min-height: 18px; padding-top: 3px; padding-bottom: 3px; }
QFrame#playbackRateField { background: #161c2a; border: 1px solid #354057; border-radius: 5px; }
QFrame#playbackRateField:hover { border-color: #637397; }
QFrame#playbackRateField QDoubleSpinBox { background: transparent; border: 0; padding: 3px 6px; }
QToolButton#playbackRatePresets { border: 0; border-left: 1px solid #2e394e; border-radius: 0; padding: 3px; }
QToolButton#playbackRatePresets:hover { background: #303b58; }
QLabel#timeBound { color: #8597ad; font-family: "Consolas"; font-size: 10px; }
QFrame#sectionDivider { background: #2e374b; max-height: 1px; border: 0; }
QFrame#timelineDivider { background: #354057; max-width: 1px; border: 0; }
QTreeWidget#propertyNavigation { background: #171e2c; border: 0; border-right: 1px solid #2e374b; padding: 10px 6px; outline: 0; }
QTreeWidget#propertyNavigation::item { height: 28px; padding: 3px 7px; border-radius: 5px; color: #acbacb; }
QTreeWidget#propertyNavigation::item:selected { background: #343d61; color: #e3e8ff; }
QTreeWidget#propertyNavigation::item:disabled { color: #5f7085; }
QTreeWidget#propertyNavigation::branch { background: transparent; }
QTreeWidget#propertyNavigation::branch:selected { background: #171e2c; }
QTreeWidget { background: #171e2c; border: 0; outline: 0; padding: 2px 0; }
QTreeWidget::item { height: 26px; border-radius: 4px; padding: 2px 4px; }
QTreeWidget::item:selected { background: #343d61; color: #e3e8ff; }
QTreeWidget::item:hover:!selected { background: #242f3e; }
QTreeWidget::branch { background: #171e2c; }
QTreeWidget#insertObjectTypes { background: #171e2c; border: 1px solid #2e374b; border-radius: 5px; padding: 4px; }
QTreeWidget#insertObjectTypes::item { height: 24px; padding: 2px 6px; }
QDialog#insertObjectsDialog QRadioButton { color: #dce5ef; spacing: 8px; }
QDialog#insertObjectsDialog QRadioButton:hover { color: #a9d5f6; }
QHeaderView::section { background: #161c2a; color: #a1aec7; border: 0; padding: 5px; }
QToolBar { background: #161c2a; border: 0; spacing: 3px; padding: 3px 8px; }
QToolBar#commandBar { border-bottom: 1px solid #2e374b; padding: 4px 10px; }
QToolBar#timelineBar { background: #161c2a; border-top: 1px solid #2e374b; padding: 0; }
QToolBar::separator { background: #37414f; width: 1px; margin: 4px 8px; }
QLineEdit, QDateTimeEdit, QDoubleSpinBox, QSpinBox, QComboBox {
    background: #161c2a; color: #dce5ef; border: 1px solid #354057;
    border-radius: 5px; padding: 5px 8px; min-height: 20px;
    selection-background-color: #4d5f95; selection-color: #f2f7fc;
}
QLineEdit:hover, QDoubleSpinBox:hover, QSpinBox:hover, QComboBox:hover { border-color: #637397; }
QLineEdit:focus, QDateTimeEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QComboBox:focus { border-color: #a3afff; background: #161c2a; }
QLineEdit:read-only { color: #a0b0c3; background: #21293b; border-color: #2e394e; }
QLineEdit:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled { color: #6a7c92; background: #171e2c; border-color: #2b3543; }
QLineEdit#vectorInput, QSpinBox, QDoubleSpinBox { font-family: "Consolas"; font-size: 13px; }
QSpinBox[stepButtons="true"], QDoubleSpinBox[stepButtons="true"] { padding-right: 29px; min-height: 22px; }
QAbstractSpinBox[stepButtons="true"]::up-button, QAbstractSpinBox[stepButtons="true"]::down-button { subcontrol-origin: border; width: 24px; border-left: 1px solid #354057; background: #202b38; }
QAbstractSpinBox[stepButtons="true"]::up-button { subcontrol-position: top right; border-top-right-radius: 3px; border-bottom: 1px solid #2e394e; }
QAbstractSpinBox[stepButtons="true"]::down-button { subcontrol-position: bottom right; border-bottom-right-radius: 3px; }
QAbstractSpinBox[stepButtons="true"]::up-button:hover, QAbstractSpinBox[stepButtons="true"]::down-button:hover { background: #2a435a; }
QAbstractSpinBox[stepButtons="true"]::up-button:pressed, QAbstractSpinBox[stepButtons="true"]::down-button:pressed { background: #4d5f95; }
QAbstractSpinBox[stepButtons="true"]::up-button:off, QAbstractSpinBox[stepButtons="true"]::down-button:off { background: #1b232e; }
QFrame[quantityField="true"] { background: #161c2a; border: 1px solid #354057; border-radius: 5px; }
QFrame[quantityField="true"]:hover { border-color: #637397; }
QFrame[quantityField="true"][focused="true"] { border-color: #a3afff; }
QFrame[quantityField="true"][invalid="true"] { border-color: #b57467; }
QFrame[quantityField="true"]:disabled { background: #171e2c; border-color: #2b3543; }
QFrame[quantityField="true"] QLineEdit { background: transparent; border: 0; padding: 5px 7px; min-height: 18px; font-family: "Consolas"; font-size: 13px; }
QToolButton#quantityUnit { color: #afbbd8; border: 0; border-left: 1px solid #2e394e; border-radius: 0; padding: 4px 17px 4px 6px; min-height: 18px; }
QToolButton#quantityUnit:hover, QToolButton#quantityUnit:focus { color: #e3e8ff; background: #303b58; }
QToolButton#quantityUnit:disabled { color: #6e7990; }
QToolButton#quantityUnit::menu-indicator { image: url(:/orbitforge/icons/chevron-down.png); subcontrol-origin: padding; subcontrol-position: right center; right: 5px; width: 9px; height: 6px; }
QComboBox { padding-right: 25px; }
QComboBox::drop-down { border: 0; width: 24px; }
QComboBox::down-arrow { image: url(:/orbitforge/icons/chevron-down.png); width: 9px; height: 6px; }
QComboBox QAbstractItemView { background: #161c2a; color: #c4d1e0; border: 1px solid #3f5267; selection-background-color: #343d61; selection-color: #e3e8ff; outline: 0; padding: 4px; }
QPushButton { background: #161c2a; color: #c5d1df; border: 1px solid #414e69; border-radius: 5px; padding: 7px 16px; }
QToolButton { color: #bbcbdc; background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 4px 8px; }
QPushButton:hover, QToolButton:hover { background: #2c354c; border-color: #526b86; }
QPushButton:pressed, QToolButton:pressed { background: #3a4465; }
QToolButton:checked { background: #343d61; color: #d4eaff; border-color: #7488bd; }
QPushButton:disabled { background: #202a37; color: #6b7d92; border-color: #344255; }
QPushButton#applyOrbit, QPushButton#insertObject, QPushButton[primary="true"] { background: #6375ba; border: 1px solid #6375ba; color: #ffffff; font-weight: 600; }
QPushButton#applyOrbit:hover, QPushButton#insertObject:hover, QPushButton[primary="true"]:hover { background: #7488d1; border-color: #7488d1; }
QPushButton#applyOrbit:disabled, QPushButton#insertObject:disabled, QPushButton[primary="true"]:disabled { background: #2b4158; border-color: #2b4158; color: #98a6c5; }
QToolButton#transportPlay { background: #343d61; border: 1px solid #7488bd; }
QToolButton#transportPlay:hover { background: #434f7b; }
QTabWidget::pane { border: 0; }
QTabBar::tab { color: #a0afc2; background: #21293b; min-height: 20px; padding: 6px 12px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: #daeaff; border-bottom-color: #a3afff; background: #161c2a; }
QStatusBar { color: #9ba8c1; background: #171e2c; border-top: 1px solid #2e394e; font-size: 11px; }
QStatusBar::item { border: 0; }
QMenuBar { background: #121827; color: #a5b5c9; padding: 1px 8px; border-bottom: 1px solid #0e1420; }
QMenuBar::item { padding: 4px 10px; }
QMenu { background: #161c2a; color: #c6d3e3; border: 1px solid #414e69; padding: 5px; }
QMenu::item { padding: 7px 28px; border-radius: 5px; }
QMenuBar::item:selected, QMenu::item:selected { background: #343d61; color: #d7ecff; }
QMenu::separator { height: 1px; background: #374458; margin: 4px 8px; }
QScrollArea { background: #1c2333; border: 0; }
QLabel#inputError { color: #e5a091; font-size: 12px; }
QFrame#epochField { background: #161c2a; border: 1px solid #354057; border-radius: 4px; }
QFrame#epochField[readOnly="true"] { background: #21293b; border-color: #2e394e; }
QFrame#epochField[focused="true"] { border-color: #a3afff; }
QFrame#epochField QLineEdit { border: 0; background: transparent; padding: 5px 9px; min-height: 18px; }
QFrame#epochField QToolButton { padding: 3px; border: 0; border-radius: 5px; }
QFrame#epochField QToolButton:hover { background: #2c354c; }
QLineEdit#epochValue { font-family: "Consolas"; font-size: 13px; }
QFrame#epochPopup { background: #161c2a; border: 1px solid #435871; border-radius: 6px; }
QCalendarWidget QWidget { background: #161c2a; }
QCalendarWidget QToolButton { padding: 7px; color: #bbcee3; font-weight: 600; border: 0; }
QCalendarWidget QToolButton#qt_calendar_prevmonth { qproperty-icon: url(:/orbitforge/icons/chevron-left.png); }
QCalendarWidget QToolButton#qt_calendar_nextmonth { qproperty-icon: url(:/orbitforge/icons/chevron-right.png); }
QCalendarWidget QToolButton:hover { background: #2a435e; }
QCalendarWidget QAbstractItemView { border: 0; outline: 0; color: #c7d5e5; background: #161c2a; selection-background-color: #4d5f95; selection-color: #edf0ff; alternate-background-color: #242d40; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px 0; }
QScrollBar::handle:vertical { background: #42556d; min-height: 30px; border-radius: 5px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QWidget#objectExplorer { background: #171e2c; }
QLineEdit#objectSearch { background: #131a29; padding: 4px 7px; border-color: #303d4d; }
QWidget#workspaceHeading { background: #171e2c; border-bottom: 1px solid #2e374b; }
QWidget#workspaceHeading QToolButton, QTabBar QToolButton { padding: 0; }
QSplitter::handle { background: #101725; }
QSplitter::handle:hover { background: #3f607c; }
QWidget#viewHeading QToolButton { padding: 2px; }
QWidget#viewHeading QComboBox { background: transparent; border-color: transparent; min-height: 18px; padding: 3px 25px 3px 6px; }
QWidget#viewHeading QComboBox:hover { background: #2a344b; border-color: #414e69; }
QToolBar#commandBar QToolButton { padding: 4px 9px; min-height: 20px; }
QToolButton:focus, QPushButton:focus { border-color: #a3afff; }
QToolButton:disabled { color: #6e7990; }
QMenu::item:disabled { color: #6e7990; }
QTabBar::tab:hover:!selected { background: #222c3a; color: #cad8e7; }
QToolButton::menu-indicator { image: none; }
QCheckBox { spacing: 8px; min-height: 24px; }
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background: #7182a5; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 0 2px; }
QScrollBar::handle:horizontal { background: #42556d; min-width: 30px; border-radius: 4px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
QLabel[fieldCaption="true"] { color: #9ba8c1; }
QLabel#eyebrow { color: #b3bdf3; font-size: 11px; font-weight: 600; }
QLabel#dialogDescription { color: #a1aec7; padding-bottom: 4px; }
QWidget#dataSidebar { background: #171e2c; border-right: 1px solid #2e374b; }
QLabel#dataSidebarCaption { color: #8f9db7; padding: 0 12px; font-size: 11px; }
QTreeWidget#dataInventory { background: transparent; border: 0; outline: 0; padding: 0; }
QTreeWidget#dataInventory::item { border: 0; padding: 0; }
QTreeWidget#dataInventory::branch { background: #171e2c; }
QListWidget#eopSourcePriority { background: transparent; border: 0; outline: 0; padding: 0; }
QListWidget#eopSourcePriority::item { border: 0; padding: 0; }
QLabel#dataName { font-size: 20px; font-weight: 600; color: #eff2fc; }
QLabel#dataDescription { color: #a1aec7; }
QTabWidget#dataPages::pane { border: 0; background: transparent; }
QTabWidget#dataPages QTabBar::tab { background: transparent; color: #93a2bd; border: 0; border-bottom: 2px solid #30394e; padding: 11px 20px; margin-right: 8px; }
QTabWidget#dataPages QTabBar::tab:selected { color: #e7eaff; border-bottom-color: #a3afff; }
QTabWidget#dataPages QTabBar::tab:hover { color: #cbd5f4; background: #242d43; }
QLabel#dataStatus { border-radius: 9px; padding: 4px 10px; font-size: 11px; background: #303b52; color: #c0cbe2; }
QLabel#dataStatus[tone="ready"] { background: #203c3e; color: #9ee0ce; }
QLabel#dataStatus[tone="warning"] { background: #443727; color: #e6c68b; }
QFrame#dataFacts { background: #20283a; border: 1px solid #323d56; border-radius: 7px; }
QLabel#dataValidFrom, QLabel#dataValidUntil { color: #ecf0ff; font-family: "Consolas"; font-size: 13px; }
QLabel#dataUpdateSchedule { color: #93a2bd; font-size: 11px; }
QProgressBar#dataDownloadProgress { background: #28334b; border: 0; border-radius: 2px; }
QProgressBar#dataDownloadProgress::chunk { background: #93a1e8; border-radius: 2px; }
QLabel#dataSource { color: #a6b7dd; }
QLabel#dataNote { color: #93a2bd; padding: 4px 0; font-size: 11px; }
QLabel#dataPriorityNote { color: #a1aec7; padding: 16px; background: #20293b; border-radius: 6px; }
QLabel#dataInventoryDetails { color: #a1aec7; font-family: "Consolas"; font-size: 11px; padding: 10px; background: #161c2a; border-radius: 6px; }
QLabel#dataCenterStatus { color: #b3c5fa; }
QLabel#dataCenterStatus[warning="true"] { color: #e6c68b; }
QWidget#reportHeading { background: #1c2333; border-bottom: 1px solid #30394e; }
QLabel#reportHeadingTitle { color: #eff2fc; font-size: 18px; font-weight: 600; }
QWidget#reportSettingsPanel, QWidget#reportSettings { background: #171e2c; }
QWidget#reportGenerateActions { background: #171e2c; border-top: 1px solid #30394e; }
QLabel#reportSectionTitle { color: #a8b7d2; font-size: 12px; font-weight: 600; }
QLabel#reportScopeLabel { color: #e4e9fa; font-weight: 600; }
QLabel#reportScopeDetail, QLabel#reportInterval, QLabel#reportEstimate { color: #92a3bd; font-size: 11px; }
QLabel#reportInterval { font-family: "Consolas"; }
QLabel#reportResultTitle { color: #e4e9fa; font-size: 14px; font-weight: 600; }
QLabel#reportResultLabel { color: #a4b3cd; font-size: 11px; }
QLabel#reportEmptyTitle { color: #b1bdd7; font-size: 18px; }
QLabel#reportStatus { color: #9daec8; background: #171e2c; border-top: 1px solid #30394e; font-size: 11px; }
QTableView#reportTable { border: 1px solid #30394e; border-radius: 4px; }
QWidget#reportHeading QPushButton, QWidget#reportSettings QPushButton { background: transparent; }
QWidget#reportSettings QComboBox, QWidget#reportSettings QLineEdit, QWidget#reportSettings QDoubleSpinBox { min-height: 18px; }
QWidget#dataFooter { background: #171e2c; border-top: 1px solid #2e374b; }
QFrame#formSection { background: #20283a; border: 1px solid #2e394e; border-radius: 7px; }
QFrame#choiceCard { background: #20283a; border: 1px solid #354057; border-radius: 8px; }
QFrame#choiceCard[selected="true"] { background: #2c3451; border-color: #7989cb; }
QFrame#choiceCard QLabel, QFrame#choiceCard QRadioButton { background: transparent; border: 0; }
QLabel#welcomeTitle { color: #f3f4ff; font-size: 34px; font-weight: 600; padding: 0; }
QLabel#welcomeDescription { color: #b9c5e2; font-size: 13px; }
QLabel#welcomeCaption { color: #8291af; font-size: 11px; }
QPushButton#welcomeExample { color: #adbce0; background: transparent; border: 0; padding: 8px 0; text-align: left; }
QPushButton#welcomeExample:hover { color: #e1e7ff; }
QPushButton#welcomeExample:focus { border: 1px solid #a3afff; }
QLabel#inputError { padding: 4px 0; }
QLineEdit#scenarioDataHash { font-family: "Consolas"; font-size: 12px; }
QLabel#orbitResult, QLabel#modelResult { color: #a1aec7; }
QToolTip { background: #161c2a; color: #c7d7e9; border: 1px solid #465c76; padding: 6px; }
)QSS"));
}
} // namespace orbitforge::desktop
