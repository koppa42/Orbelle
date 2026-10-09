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
    palette.setColor(QPalette::Window, QColor("#191e26"));
    palette.setColor(QPalette::WindowText, QColor("#dce3ec"));
    palette.setColor(QPalette::Base, QColor("#191f28"));
    palette.setColor(QPalette::AlternateBase, QColor("#242d39"));
    palette.setColor(QPalette::Text, QColor("#dce3ec"));
    palette.setColor(QPalette::Button, QColor("#252e3a"));
    palette.setColor(QPalette::ButtonText, QColor("#c3cedb"));
    palette.setColor(QPalette::Highlight, QColor("#29455f"));
    palette.setColor(QPalette::HighlightedText, QColor("#e0f0ff"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#657282"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#657282"));
    app.setPalette(palette);
    QFont font;
    font.setFamilies({"Segoe UI", "Microsoft YaHei UI"});
    font.setStyleHint(QFont::SansSerif);
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setPixelSize(12);
    app.setFont(font);
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Han, "Microsoft YaHei UI");
    app.setWindowIcon(workbench_icon(WorkbenchIcon::orbit, QColor("#74b9ee")));
    app.setStyleSheet(QStringLiteral(R"QSS(
QWidget { font-family: "Segoe UI", "Microsoft YaHei UI"; font-size: 12px; }
QDialog { background: #1d242e; }
QWidget#propertyFields { background: #1d242e; }
QWidget#welcome { background: #151a22; }
QLabel#welcomeTitle { font-size: 24px; font-weight: 600; color: #e2e8f0; padding: 12px; }
QMainWindow { background: #10151c; }
QMainWindow::separator { background: #10151c; width: 4px; height: 4px; }
QMainWindow::separator:hover { background: #506e88; }
QWidget#dockHeading { background: #191f28; border-bottom: 1px solid #303947; }
QWidget#dockHeading QToolButton { padding: 0; }
QLabel#dockTitle { color: #b6c3d3; font-weight: 600; }
QWidget#viewHeading { background: #1c232d; border-bottom: 1px solid #303947; }
QLabel#viewTitle { color: #b9c7d7; font-weight: 600; }
QWidget#viewHeading QLabel#hint { color: #92a4b8; }
QWidget#viewHeading QLabel#badge { background: #243548; border-color: #39526b; color: #b4cee5; }
QLabel#badge { color: #9bc7ed; background: #293645; border: 1px solid #3c5269; border-radius: 3px; padding: 3px 8px; font-size: 10px; }
QLabel#muted { color: #93a2b5; }
QLabel#brand { color: #e2e9f1; font-size: 13px; font-weight: 600; padding: 0 2px 0 2px; }
QLabel#workspaceName { color: #93a2b5; padding: 0 8px 0 4px; }
QLabel#sectionTitle { color: #b8c9da; font-size: 12px; font-weight: 600; padding-top: 5px; padding-bottom: 5px; }
QLabel#panelTitle { color: #e2e9f2; font-size: 17px; font-weight: 600; }
QLabel#panelDescription { color: #8596aa; font-size: 11px; }
QLabel#hint { color: #8c9db1; font-size: 11px; }
QLabel#fieldUnit { color: #8e9eb0; font-size: 11px; }
QToolButton#timeReadout { color: #e9b975; font-family: "Consolas"; font-size: 13px; padding: 4px 8px; }
QLabel#timelineTitle { color: #b8c9da; font-weight: 600; }
QToolBar#timelineBar QToolButton { padding: 3px; }
QToolBar#timelineBar QComboBox, QToolBar#timelineBar QDoubleSpinBox { min-height: 18px; padding-top: 3px; padding-bottom: 3px; }
QLabel#timeBound { color: #8597ad; font-family: "Consolas"; font-size: 10px; }
QFrame#sectionDivider { background: #303947; max-height: 1px; border: 0; }
QFrame#timelineDivider { background: #374354; max-width: 1px; border: 0; }
QTreeWidget#propertyNavigation { background: #171d26; border: 0; border-right: 1px solid #303947; padding: 10px 6px; outline: 0; }
QTreeWidget#propertyNavigation::item { height: 26px; padding: 2px 5px; border-radius: 2px; color: #acbacb; }
QTreeWidget#propertyNavigation::item:selected { background: #263e55; color: #d3eaff; }
QTreeWidget#propertyNavigation::item:disabled { color: #5f7085; }
QTreeWidget { background: #171d26; border: 0; outline: 0; padding: 2px 0; }
QTreeWidget::item { height: 24px; border-radius: 2px; padding: 2px 4px; }
QTreeWidget::item:selected { background: #263e55; color: #d3eaff; }
QTreeWidget::item:hover:!selected { background: #242f3e; }
QHeaderView::section { background: #191f28; color: #93a2b5; border: 0; padding: 5px; }
QToolBar { background: #191f28; border: 0; spacing: 3px; padding: 3px 8px; }
QToolBar#commandBar { border-bottom: 1px solid #303947; padding: 4px 10px; }
QToolBar#timelineBar { background: #191f28; border-top: 1px solid #303947; padding: 0; }
QToolBar::separator { background: #37414f; width: 1px; margin: 4px 8px; }
QLineEdit, QDateTimeEdit, QDoubleSpinBox, QSpinBox, QComboBox {
    background: #191f28; color: #dce5ef; border: 1px solid #374354;
    border-radius: 3px; padding: 4px 8px; min-height: 18px;
    selection-background-color: #365d80; selection-color: #f2f7fc;
}
QLineEdit:hover, QDoubleSpinBox:hover, QSpinBox:hover, QComboBox:hover { border-color: #51657c; }
QLineEdit:focus, QDateTimeEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QComboBox:focus { border-color: #74b9ee; background: #191f28; }
QLineEdit:read-only { color: #a0b0c3; background: #202833; border-color: #303c4b; }
QLineEdit:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled { color: #6a7c92; background: #171d26; border-color: #2b3543; }
QLineEdit#vectorInput, QSpinBox, QDoubleSpinBox { font-family: "Consolas"; font-size: 13px; }
QComboBox { padding-right: 25px; }
QComboBox::drop-down { border: 0; width: 24px; }
QComboBox::down-arrow { image: url(:/orbitforge/icons/chevron-down.png); width: 9px; height: 6px; }
QComboBox QAbstractItemView { background: #191f28; color: #c4d1e0; border: 1px solid #3f5267; selection-background-color: #263e55; selection-color: #d3eaff; outline: 0; padding: 4px; }
QPushButton { background: #191f28; color: #c5d1df; border: 1px solid #405169; border-radius: 3px; padding: 6px 14px; }
QToolButton { color: #bbcbdc; background: transparent; border: 1px solid transparent; border-radius: 3px; padding: 4px 8px; }
QPushButton:hover, QToolButton:hover { background: #293645; border-color: #526b86; }
QPushButton:pressed, QToolButton:pressed { background: #31445a; }
QToolButton:checked { background: #29455f; color: #d4eaff; border-color: #507799; }
QPushButton:disabled { background: #202a37; color: #6b7d92; border-color: #344255; }
QPushButton#applyOrbit { background: #39688e; border: 1px solid #39688e; color: #ffffff; font-weight: 600; }
QPushButton#applyOrbit:hover { background: #467da8; border-color: #467da8; }
QPushButton#applyOrbit:disabled { background: #2b4158; border-color: #2b4158; color: #91a9be; }
QToolButton#transportPlay { background: #263f55; border: 1px solid #4d7193; }
QToolButton#transportPlay:hover { background: #34536f; }
QTabWidget::pane { border: 0; }
QTabBar::tab { color: #a0afc2; background: #202833; min-height: 20px; padding: 6px 12px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: #daeaff; border-bottom-color: #74b9ee; background: #191f28; }
QStatusBar { color: #8d9eb2; background: #171d26; border-top: 1px solid #303c4b; font-size: 10px; }
QStatusBar::item { border: 0; }
QMenuBar { background: #151a22; color: #a5b5c9; padding: 1px 8px; border-bottom: 1px solid #10151c; }
QMenuBar::item { padding: 4px 10px; }
QMenu { background: #191f28; color: #c6d3e3; border: 1px solid #405169; padding: 5px; }
QMenu::item { padding: 7px 28px; border-radius: 3px; }
QMenuBar::item:selected, QMenu::item:selected { background: #263f55; color: #d7ecff; }
QMenu::separator { height: 1px; background: #374458; margin: 4px 8px; }
QScrollArea { background: #1d242e; border: 0; }
QLabel#inputError { color: #e5a091; font-size: 11px; }
QFrame#epochField { background: #191f28; border: 1px solid #374354; border-radius: 4px; }
QFrame#epochField[readOnly="true"] { background: #202833; border-color: #303c4b; }
QFrame#epochField QLineEdit { border: 0; background: transparent; padding: 5px 9px; min-height: 18px; }
QFrame#epochField QToolButton { padding: 3px; border: 0; border-radius: 3px; }
QFrame#epochField QToolButton:hover { background: #293645; }
QLineEdit#epochValue { font-family: "Consolas"; font-size: 13px; }
QFrame#epochPopup { background: #191f28; border: 1px solid #435871; border-radius: 6px; }
QCalendarWidget QWidget { background: #191f28; }
QCalendarWidget QToolButton { padding: 7px; color: #bbcee3; font-weight: 600; border: 0; }
QCalendarWidget QToolButton#qt_calendar_prevmonth { qproperty-icon: url(:/orbitforge/icons/chevron-left.png); }
QCalendarWidget QToolButton#qt_calendar_nextmonth { qproperty-icon: url(:/orbitforge/icons/chevron-right.png); }
QCalendarWidget QToolButton:hover { background: #2a435e; }
QCalendarWidget QAbstractItemView { border: 0; outline: 0; color: #c7d5e5; background: #191f28; selection-background-color: #345c80; selection-color: #e0f0ff; alternate-background-color: #242d39; }
QScrollBar:vertical { background: #191f28; width: 6px; margin: 2px 0; }
QScrollBar::handle:vertical { background: #42556d; min-height: 30px; border-radius: 3px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QWidget#objectExplorer { background: #171d26; }
QLineEdit#objectSearch { background: #141a22; padding: 4px 7px; border-color: #303d4d; }
QWidget#workspaceHeading { background: #171d26; border-bottom: 1px solid #303947; }
QWidget#workspaceHeading QToolButton, QTabBar QToolButton { padding: 0; }
QSplitter::handle { background: #111720; }
QSplitter::handle:hover { background: #3f607c; }
QWidget#viewHeading QToolButton { padding: 2px; }
QWidget#viewHeading QComboBox { background: transparent; border-color: transparent; min-height: 18px; padding: 3px 25px 3px 6px; }
QWidget#viewHeading QComboBox:hover { background: #263242; border-color: #405169; }
QToolBar#commandBar QToolButton { padding: 4px 9px; min-height: 20px; }
QToolButton:focus, QPushButton:focus { border-color: #74b9ee; }
QToolButton:disabled { color: #657282; }
QMenu::item:disabled { color: #657282; }
QTabBar::tab:hover:!selected { background: #222c3a; color: #cad8e7; }
QToolButton::menu-indicator { image: none; }
QCheckBox { spacing: 8px; min-height: 24px; }
QToolTip { background: #191f28; color: #c7d7e9; border: 1px solid #465c76; padding: 6px; }
)QSS"));
}
} // namespace orbitforge::desktop
