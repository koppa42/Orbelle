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
#include <QToolButton>

namespace orbitforge::desktop {
namespace {
class WorkbenchStyle final : public QProxyStyle {
  public:
    WorkbenchStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}
    void polish(QWidget *widget) override {
        QProxyStyle::polish(widget);
        if (auto *number = qobject_cast<QAbstractSpinBox *>(widget))
            number->setButtonSymbols(QAbstractSpinBox::NoButtons);
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
            p.setBrush(QColor("#ffffff"));
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
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(12, 6, 6, 6);
    layout->setSpacing(8);
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
    auto *close = new QToolButton;
    close->setIcon(workbench_icon(WorkbenchIcon::close));
    close->setToolTip(QStringLiteral("隐藏面板 · 可从视图菜单恢复"));
    close->setFixedSize(22, 22);
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
    palette.setColor(QPalette::Window, QColor("#151d27"));
    palette.setColor(QPalette::WindowText, QColor("#d5dfeb"));
    palette.setColor(QPalette::Base, QColor("#17232f"));
    palette.setColor(QPalette::AlternateBase, QColor("#1d2a38"));
    palette.setColor(QPalette::Text, QColor("#d5dfeb"));
    palette.setColor(QPalette::Button, QColor("#223142"));
    palette.setColor(QPalette::ButtonText, QColor("#bbcadb"));
    palette.setColor(QPalette::Highlight, QColor("#28415c"));
    palette.setColor(QPalette::HighlightedText, QColor("#d6e8fa"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#617387"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#617387"));
    app.setPalette(palette);
    QFont font;
    font.setFamilies({"Segoe UI", "Microsoft YaHei UI"});
    font.setStyleHint(QFont::SansSerif);
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setPixelSize(12);
    app.setFont(font);
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Han, "Microsoft YaHei UI");
    app.setWindowIcon(workbench_icon(WorkbenchIcon::orbit, QColor("#6d9dc9")));
    app.setStyleSheet(QStringLiteral(R"QSS(
QWidget { font-family: "Segoe UI", "Microsoft YaHei UI"; font-size: 12px; }
QDialog { background: #1a2632; }
QWidget#propertyFields { background: #1a2632; }
QWidget#welcome { background: #141e29; }
QLabel#welcomeTitle { font-size: 24px; font-weight: 600; color: #d3e0ed; padding: 12px; }
QMainWindow { background: #101922; }
QMainWindow::separator { background: #101922; width: 4px; height: 4px; }
QMainWindow::separator:hover { background: #415b76; }
QWidget#dockHeading { background: #17232f; border-bottom: 1px solid #293a4b; }
QWidget#dockHeading QToolButton { padding: 0; }
QLabel#dockTitle { color: #9eb4ca; font-weight: 600; }
QWidget#viewHeading { background: #172331; border-bottom: 1px solid #2b3949; }
QLabel#viewTitle { color: #d8e4ef; font-weight: 600; }
QWidget#viewHeading QLabel#hint { color: #8fa3b8; }
QWidget#viewHeading QLabel#badge { background: #24394d; border-color: #324b60; color: #a9c3db; }
QLabel#badge { color: #8eb5db; background: #223449; border: 1px solid #32495e; border-radius: 3px; padding: 3px 8px; font-size: 10px; }
QLabel#muted { color: #8296aa; }
QLabel#brand { color: #d4e1ee; font-size: 16px; font-weight: 600; padding: 0 12px 0 4px; }
QLabel#workspaceName { color: #8296aa; padding: 0 14px; border-left: 1px solid #304052; }
QLabel#sectionTitle { color: #b6cce0; font-size: 12px; font-weight: 600; padding-top: 8px; padding-bottom: 6px; }
QLabel#panelTitle { color: #d8e5f1; font-size: 19px; font-weight: 600; }
QLabel#panelDescription { color: #788da2; font-size: 11px; }
QLabel#hint { color: #7e93a8; font-size: 11px; }
QLabel#fieldUnit { color: #7890a7; font-size: 11px; }
QToolButton#timeReadout { color: #e4bc88; font-family: "Consolas"; font-size: 13px; padding: 4px 8px; }
QLabel#timelineTitle { color: #b6cce0; font-weight: 600; }
QToolBar#timelineBar QToolButton { padding: 3px; }
QToolBar#timelineBar QComboBox, QToolBar#timelineBar QDoubleSpinBox { min-height: 18px; padding-top: 3px; padding-bottom: 3px; }
QLabel#timeBound { color: #71879e; font-family: "Consolas"; font-size: 10px; }
QFrame#sectionDivider { background: #2a3b4c; max-height: 1px; border: 0; }
QFrame#timelineDivider { background: #304254; max-width: 1px; border: 0; }
QTreeWidget#propertyNavigation { background: #131e29; border: 0; border-right: 1px solid #2b3b4b; padding: 14px 6px; outline: 0; }
QTreeWidget#propertyNavigation::item { height: 29px; padding: 2px 5px; border-radius: 2px; color: #9eafc1; }
QTreeWidget#propertyNavigation::item:selected { background: #243c54; color: #c4def3; }
QTreeWidget#propertyNavigation::item:disabled { color: #4e647a; }
QTreeWidget { background: #14202b; border: 0; outline: 0; padding: 2px 0; }
QTreeWidget::item { height: 25px; border-radius: 3px; padding: 2px; }
QTreeWidget::item:selected { background: #243e57; color: #bad6f0; }
QTreeWidget::item:hover:!selected { background: #1f3041; }
QHeaderView::section { background: #17232f; color: #8296aa; border: 0; padding: 5px; }
QToolBar { background: #17232f; border: 0; spacing: 4px; padding: 6px 12px; }
QToolBar#commandBar { border-bottom: 1px solid #2a3b4c; padding: 7px 12px; }
QToolBar#timelineBar { background: #17232f; border-top: 1px solid #2a3b4c; padding: 0; }
QToolBar::separator { background: #2d3d4e; width: 1px; margin: 6px 10px; }
QLineEdit, QDateTimeEdit, QDoubleSpinBox, QSpinBox, QComboBox {
    background: #17232f; color: #d4dfeb; border: 1px solid #304254;
    border-radius: 4px; padding: 5px 9px; min-height: 20px;
    selection-background-color: #3d5d7b; selection-color: #edf5fc;
}
QLineEdit:hover, QDoubleSpinBox:hover, QSpinBox:hover, QComboBox:hover { border-color: #48637d; }
QLineEdit:focus, QDateTimeEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QComboBox:focus { border-color: #709aca; background: #17232f; }
QLineEdit:read-only { color: #91a6bb; background: #182430; border-color: #2c3b4a; }
QLineEdit:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled { color: #526b81; background: #14202b; border-color: #253748; }
QLineEdit#vectorInput, QSpinBox, QDoubleSpinBox { font-family: "Consolas"; font-size: 13px; }
QComboBox { padding-right: 25px; }
QComboBox::drop-down { border: 0; width: 24px; }
QComboBox::down-arrow { image: url(:/orbitforge/icons/chevron-down.png); width: 9px; height: 6px; }
QComboBox QAbstractItemView { background: #17232f; color: #bdd0e2; border: 1px solid #344b60; selection-background-color: #213952; selection-color: #bbd8f1; outline: 0; padding: 4px; }
QPushButton { background: #17232f; color: #adbed0; border: 1px solid #354b60; border-radius: 4px; padding: 7px 15px; }
QToolButton { color: #a6bcd0; background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 5px 8px; }
QPushButton:hover, QToolButton:hover { background: #223449; border-color: #46617b; }
QPushButton:pressed, QToolButton:pressed { background: #2b435b; }
QToolButton:checked { background: #26415a; color: #aacfeb; border-color: #3b5e7e; }
QPushButton:disabled { background: #192632; color: #5e7489; border-color: #2a3a4c; }
QPushButton#applyOrbit { background: #3e648b; border: 1px solid #3e648b; color: #ffffff; font-weight: 600; }
QPushButton#applyOrbit:hover { background: #4c769f; border-color: #4c769f; }
QPushButton#applyOrbit:disabled { background: #2b4056; border-color: #2b4056; color: #819bb3; }
QToolButton#transportPlay { background: #263f57; border: 1px solid #42617e; }
QToolButton#transportPlay:hover { background: #304f6a; }
QTabWidget::pane { border: 0; }
QTabBar::tab { color: #89a0b7; background: #182430; padding: 9px 18px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: #aaccec; border-bottom-color: #7fa6d0; background: #17232f; }
QStatusBar { color: #6f89a2; background: #14202b; border-top: 1px solid #2c3b4a; font-size: 10px; }
QStatusBar::item { border: 0; }
QMenuBar { background: #141e29; color: #8ca3ba; padding: 1px 8px; border-bottom: 1px solid #101922; }
QMenuBar::item { padding: 5px 11px; }
QMenu { background: #17232f; color: #b7cce0; border: 1px solid #354b60; padding: 5px; }
QMenu::item { padding: 7px 28px; border-radius: 3px; }
QMenuBar::item:selected, QMenu::item:selected { background: #263f57; color: #bcdaf3; }
QMenu::separator { height: 1px; background: #2b3d50; margin: 4px 8px; }
QScrollArea { background: #1a2632; border: 0; }
QLabel#inputError { color: #e6a197; font-size: 11px; }
QFrame#epochField { background: #17232f; border: 1px solid #304254; border-radius: 4px; }
QFrame#epochField[readOnly="true"] { background: #182430; border-color: #2c3b4a; }
QFrame#epochField QLineEdit { border: 0; background: transparent; padding: 7px 10px; min-height: 18px; }
QFrame#epochField QToolButton { padding: 3px; border: 0; border-radius: 3px; }
QFrame#epochField QToolButton:hover { background: #223449; }
QLineEdit#epochValue { font-family: "Consolas"; font-size: 13px; }
QFrame#epochPopup { background: #17232f; border: 1px solid #3b5065; border-radius: 6px; }
QCalendarWidget QWidget { background: #17232f; }
QCalendarWidget QToolButton { padding: 7px; color: #a5bed6; font-weight: 600; border: 0; }
QCalendarWidget QToolButton#qt_calendar_prevmonth { qproperty-icon: url(:/orbitforge/icons/chevron-left.png); }
QCalendarWidget QToolButton#qt_calendar_nextmonth { qproperty-icon: url(:/orbitforge/icons/chevron-right.png); }
QCalendarWidget QToolButton:hover { background: #263e55; }
QCalendarWidget QAbstractItemView { border: 0; outline: 0; color: #b8ccdf; background: #17232f; selection-background-color: #2e4d6b; selection-color: #d6e8fa; alternate-background-color: #1d2a38; }
QScrollBar:vertical { background: #172330; width: 6px; margin: 2px 0; }
QScrollBar::handle:vertical { background: #3b5269; min-height: 30px; border-radius: 3px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QToolTip { background: #17232f; color: #b8cde0; border: 1px solid #3c546b; padding: 6px; }
)QSS"));
}
} // namespace orbitforge::desktop
