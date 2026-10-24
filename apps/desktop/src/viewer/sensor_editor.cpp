#include "sensor_editor.hpp"
#include "../ui_text.hpp"
#include "form_kit.hpp"
#include "object_selection.hpp"
#include "project_update.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace orbelle::desktop {
namespace {
QColor sensor_axis_color(int axis) {
    constexpr std::array colors{"#ee9aab", "#8fd2ab", "#8cbfee"};
    return QColor(colors[axis]);
}
QTableWidgetItem *angle_item(double radians) {
    auto *item = new QTableWidgetItem(QString::number(radians * 180 / std::numbers::pi, 'g', 15));
    item->setData(Qt::UserRole, radians);
    item->setData(Qt::UserRole + 1, item->text());
    return item;
}
double point_angle(const QTableWidgetItem *item) {
    if (item && item->data(Qt::UserRole).isValid() &&
        item->text() == item->data(Qt::UserRole + 1).toString())
        return item->data(Qt::UserRole).toDouble();
    bool valid = false;
    const auto value = item ? item->text().toDouble(&valid) : 0;
    if (!valid || !std::isfinite(value))
        throw std::invalid_argument(ui::utf8("sensor.geometry.invalid_points"));
    return value * std::numbers::pi / 180;
}
} // namespace
// A long satellite name must not widen a docked properties page.
class SensorParentLabel final : public QLabel {
  public:
    explicit SensorParentLabel(QWidget *parent) : QLabel(parent) {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
    }
    void set_name(const QString &name) {
        name_ = name;
        setToolTip(name);
        setAccessibleName(ui::text("sensor_editor.label").arg(name));
        update_text();
    }

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        update_text();
    }

  private:
    void update_text() { setText(fontMetrics().elidedText(name_, Qt::ElideRight, width())); }
    QString name_;
};

// Intrinsic cone cross-section only: no orbital position or ground coverage is implied.
class SensorAnglePreview final : public QWidget {
  public:
    explicit SensorAnglePreview(QWidget *parent) : QWidget(parent) {
        setObjectName("sensorAnglePreview");
        setFixedSize(100, 112);
        setAccessibleName(ui::text("sensor_editor.label_2"));
    }
    void set_angle(std::optional<double> degrees) {
        degrees_ = degrees;
        setAccessibleDescription(degrees ? ui::text("sensor_editor.label_3")
                                               .arg(*degrees, 0, 'g', 6)
                                               .arg(*degrees * 2, 0, 'g', 6)
                                         : ui::text("sensor_editor.label_4"));
        update();
    }
    void set_pattern(const core::SensorPattern &pattern) {
        loops_ = core::SensorGeometry(pattern).boundaries(std::numbers::pi / 60);
        simple_ = pattern.type == core::SensorPatternType::simple_conic;
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (!simple_) {
            QPainterPath path;
            path.setFillRule(Qt::OddEvenFill);
            for (const auto &loop : loops_) {
                for (std::size_t i = 0; i < loop.size(); ++i) {
                    const auto &p = loop[i];
                    const auto cone = std::acos(std::clamp(p[2], -1.0, 1.0));
                    const auto clock = std::atan2(p[1], p[0]);
                    const QPointF point(50 + 44 * cone / std::numbers::pi * std::cos(clock),
                                        50 - 44 * cone / std::numbers::pi * std::sin(clock));
                    if (i == 0)
                        path.moveTo(point);
                    else
                        path.lineTo(point);
                }
                path.closeSubpath();
            }
            if (!path.isEmpty()) {
                const auto bounds = path.boundingRect();
                const auto scale = 80 / std::max({bounds.width(), bounds.height(), 1.0});
                QTransform t;
                t.translate(50, 48);
                t.scale(scale, scale);
                t.translate(-bounds.center().x(), -bounds.center().y());
                painter.setPen(QPen(theme::color(theme::accent), 1.3));
                painter.setBrush(theme::color(theme::accent, 32));
                painter.drawPath(t.map(path));
            }
            painter.setPen(theme::color(theme::text_muted));
            painter.drawText(QRect(0, 92, width(), 18), Qt::AlignCenter,
                             ui::text("sensor.geometry.preview"));
            return;
        }
        const QPointF apex(width() / 2.0, 20);
        constexpr double radius = 64;
        painter.setPen(QPen(theme::color(theme::border_strong), 1, Qt::DashLine));
        painter.drawLine(apex, QPointF(apex.x(), 87));
        if (degrees_) {
            // Fit a true-angle sector even for a 90-degree half-angle.
            const double extent =
                std::max(1.0, std::sin(*degrees_ * std::numbers::pi / 180) * radius);
            const double length = radius * std::min(1.0, (width() / 2.0 - 8) / extent);
            QPainterPath cone;
            cone.moveTo(apex);
            cone.arcTo(QRectF(apex.x() - length, apex.y() - length, length * 2, length * 2),
                       -90 + *degrees_, -*degrees_ * 2);
            cone.closeSubpath();
            painter.setPen(QPen(theme::color(theme::accent), 1.3));
            painter.setBrush(theme::color(theme::accent, 32));
            painter.drawPath(cone);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(theme::color(theme::sakura), 1.5));
            painter.drawArc(QRectF(apex.x() - 25, apex.y() - 25, 50, 50), -90 * 16,
                            qRound(*degrees_ * 16));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::color(theme::accent_bright));
        painter.drawEllipse(apex, 3, 3);
        painter.setPen(theme::color(theme::text_muted));
        auto font = painter.font();
        font.setPixelSize(10);
        painter.setFont(font);
        painter.drawText(QRect(0, 92, width(), 18), Qt::AlignCenter,
                         ui::text("sensor_editor.label_5"));
    }

  private:
    std::optional<double> degrees_;
    bool simple_ = true;
    std::vector<std::vector<core::SensorVector>> loops_;
};

// Local orientation only. The rectangle indicates clock angle, not the actual FOV.
class SensorOrientationPreview final : public QWidget {
  public:
    explicit SensorOrientationPreview(QWidget *parent) : QWidget(parent) {
        setObjectName("sensorOrientationPreview");
        setMinimumWidth(220);
        setFixedHeight(224);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(ui::text("sensor.pointing.diagram.title"));
    }
    void set_angles(std::optional<std::array<double, 3>> angles) {
        axes_.reset();
        if (angles) {
            axes_ =
                core::sensor_axes({1, 0, 0}, {0, 0, 1}, (*angles)[0], (*angles)[1], (*angles)[2]);
            setAccessibleDescription(ui::text("sensor.pointing.diagram.angles")
                                         .arg((*angles)[0] * 180 / std::numbers::pi, 0, 'g', 6)
                                         .arg((*angles)[1] * 180 / std::numbers::pi, 0, 'g', 6)
                                         .arg((*angles)[2] * 180 / std::numbers::pi, 0, 'g', 6));
        } else {
            setAccessibleDescription(ui::text("sensor.pointing.diagram.invalid"));
        }
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(theme::color(theme::text_muted));
        painter.drawText(QRect(8, 0, width() - 16, 20), Qt::AlignCenter,
                         ui::text("sensor.pointing.diagram.heading"));
        if (!axes_) {
            painter.drawText(rect().adjusted(20, 40, -20, -40), Qt::AlignCenter | Qt::TextWordWrap,
                             ui::text("sensor.pointing.diagram.invalid"));
            return;
        }
        using V = core::SensorVector;
        const QPointF origin(width() / 2. - 12, 104);
        const double scale = std::min(68., (width() - 64.) / 2.);
        const auto project = [&](const V &v) {
            return origin +
                   scale * QPointF(.8 * v[0] + .6 * v[1], .36 * v[0] - .48 * v[1] - .8 * v[2]);
        };
        const auto arrow = [&](const V &axis, QColor color, const QString &label, bool baseline) {
            V tip = axis;
            for (auto &v : tip)
                v *= baseline ? 1.17 : .95;
            const auto end = project(tip);
            const auto delta = end - origin;
            const auto length = std::hypot(delta.x(), delta.y());
            painter.setPen(
                QPen(color, baseline ? 1.1 : 2, baseline ? Qt::DashLine : Qt::SolidLine));
            painter.drawLine(origin, end);
            painter.setBrush(color);
            if (length > 8) {
                const auto along = delta / length;
                const QPointF normal(-along.y(), along.x());
                painter.setPen(Qt::NoPen);
                painter.drawPolygon(
                    QPolygonF{end, end - 7 * along + 3 * normal, end - 7 * along - 3 * normal});
            }
            // Put current-axis labels beside the arrow so zero offsets do not
            // superimpose X/Y/Z on the longer baseline's X0/Y0/Z0 labels.
            const auto offset =
                length > 1
                    ? (baseline
                           ? delta * (13 / length)
                           : delta * (6 / length) + QPointF(-delta.y(), delta.x()) * (13 / length))
                    : QPointF(0, -12);
            const auto position = end + offset;
            painter.setPen(color);
            painter.drawText(QRectF(std::clamp(position.x() - 20, 4., width() - 44.),
                                    std::clamp(position.y() - 10, 22., 158.), 40, 20),
                             Qt::AlignCenter, label);
        };
        const auto muted = theme::color(theme::text_muted, 155);
        arrow({1, 0, 0}, muted, QStringLiteral("X0"), true);
        arrow({0, 1, 0}, muted, QStringLiteral("Y0"), true);
        arrow({0, 0, 1}, muted, QStringLiteral("Z0"), true);
        const auto x_color = sensor_axis_color(0), y_color = sensor_axis_color(1),
                   z_color = sensor_axis_color(2);
        // An asymmetric orientation plane makes rotation about Z visible, including
        // when the actual field of view is a rotationally symmetric cone.
        QPolygonF plane;
        for (const auto &corner : std::array<QPointF, 4>{QPointF(-.28, -.17), QPointF(.28, -.17),
                                                         QPointF(.28, .17), QPointF(-.28, .17)}) {
            V point{};
            for (std::size_t i = 0; i < 3; ++i)
                point[i] = .58 * axes_->z[i] + corner.x() * axes_->x[i] + corner.y() * axes_->y[i];
            plane << project(point);
        }
        auto plane_fill = z_color;
        plane_fill.setAlpha(24);
        painter.setPen(QPen(z_color, 1));
        painter.setBrush(plane_fill);
        painter.drawPolygon(plane);
        arrow(axes_->x, x_color, QStringLiteral("X"), false);
        arrow(axes_->y, y_color, QStringLiteral("Y"), false);
        arrow(axes_->z, z_color, QStringLiteral("Z"), false);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::color(theme::text));
        painter.drawEllipse(origin, 3, 3);
        painter.setPen(theme::color(theme::text_muted));
        painter.drawText(QRect(8, 181, width() - 16, 20), Qt::AlignCenter,
                         ui::text("sensor.pointing.diagram.legend"));
        painter.drawText(QRect(8, 204, width() - 16, 16), Qt::AlignCenter,
                         ui::text("sensor.pointing.diagram.scope"));
    }

  private:
    std::optional<core::SensorAxes> axes_;
};

SensorEditor::SensorEditor(QWidget *parent) : QDialog(parent) {
    setObjectName("sensorProperties");
    setWindowIcon(workbench_icon(WorkbenchIcon::focus));
    setMinimumWidth(320);
    resize(760, 660);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *content = new QWidget(this);
    auto *outer = new QHBoxLayout(content);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *compact_bar = new QWidget(content);
    auto *compact_layout = new QHBoxLayout(compact_bar);
    compact_layout->setContentsMargins(16, 12, 16, 0);
    auto *selector = new QComboBox(compact_bar);
    selector->setObjectName("propertyPageSelector");
    selector->setAccessibleName(ui::text("sensor.pages.navigation"));
    auto *tabs = new QTabBar(compact_bar);
    tabs->setObjectName("propertyPageTabs");
    tabs->setAccessibleName(ui::text("sensor.pages.navigation"));
    tabs->setDrawBase(false);
    tabs->setExpanding(false);
    tabs->setUsesScrollButtons(true);
    auto *navigation = new QTreeWidget(content);
    navigation->setObjectName("propertyNavigation");
    navigation->setAccessibleName(ui::text("sensor.pages.navigation"));
    navigation->setFixedWidth(164);
    navigation->setHeaderHidden(true);
    navigation->setRootIsDecorated(false);
    navigation->setIndentation(16);
    navigation->setUniformRowHeights(true);
    navigation->setIconSize({17, 17});
    const QStringList labels{ui::text("sensor.pages.fov"), ui::text("sensor.pages.pointing"),
                             ui::text("sensor.pages.graphics_2d"),
                             ui::text("sensor.pages.graphics_3d")};
    const QStringList tree_labels{ui::text("sensor.pages.definition"), labels[1],
                                  ui::text("sensor.pages.attributes"),
                                  ui::text("sensor.pages.projection")};
    const std::array icons{WorkbenchIcon::focus, WorkbenchIcon::earth, WorkbenchIcon::map,
                           WorkbenchIcon::properties};
    const auto category = [&](const QString &heading) {
        auto *group = new QTreeWidgetItem(navigation, {heading});
        group->setFlags(Qt::ItemIsEnabled);
        auto font = group->font(0);
        font.setWeight(QFont::DemiBold);
        group->setFont(0, font);
        group->setExpanded(true);
        return group;
    };
    auto *basic = category(ui::text("sensor.pages.basic"));
    const std::array groups{basic, basic, category(labels[2]), category(labels[3])};
    std::vector<QTreeWidgetItem *> page_items;
    for (int i = 0; i < labels.size(); ++i) {
        selector->addItem(labels[i]);
        tabs->addTab(labels[i]);
        auto *item = new QTreeWidgetItem(groups[i], {tree_labels[i]});
        item->setIcon(0, workbench_icon(icons[i]));
        item->setData(0, Qt::UserRole, i);
        page_items.push_back(item);
    }
    compact_layout->addWidget(selector, 1);
    compact_layout->addWidget(tabs);
    compact_layout->addStretch();
    auto *pages = new QStackedWidget(content);
    pages->setObjectName("sensorPages");
    pages->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    outer->addWidget(compact_bar);
    outer->addWidget(navigation);
    outer->addWidget(pages, 1);
    // Start in the minimum-width layout before the first queued size observation.
    // Otherwise the initial horizontal layout can push the page off-screen.
    outer->setDirection(QBoxLayout::TopToBottom);
    navigation->hide();
    tabs->hide();
    root->addWidget(content, 1);
    connect(selector, &QComboBox::currentIndexChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(tabs, &QTabBar::currentChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(navigation, &QTreeWidget::currentItemChanged, pages, [pages](QTreeWidgetItem *item) {
        if (item && item->data(0, Qt::UserRole).isValid())
            pages->setCurrentIndex(item->data(0, Qt::UserRole).toInt());
    });
    connect(pages, &QStackedWidget::currentChanged, this, [=](int index) {
        const QSignalBlocker a(selector), b(tabs), c(navigation);
        selector->setCurrentIndex(index);
        tabs->setCurrentIndex(index);
        navigation->setCurrentItem(page_items.at(index));
    });
    new WidthObserver(content, [=, mode = WidthMode{}, tight = WidthMode{}](int width) mutable {
        const bool narrow = mode.narrow(width, 680);
        const bool compact = tight.narrow(width, 400);
        navigation->setVisible(!narrow);
        compact_bar->setVisible(narrow);
        selector->setVisible(narrow && compact);
        tabs->setVisible(narrow && !compact);
        outer->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    });
    // Each page owns its scroll position and editor instances. Resizing or changing
    // navigation never reloads drafts, selected objects, or quantity units.
    QWidget *body = nullptr;
    QVBoxLayout *layout = nullptr;
    const auto page = [&](const char *name, const QString &heading = {}) {
        auto *scroll = new QScrollArea(pages);
        scroll->setObjectName(name);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        body = new QWidget;
        body->setObjectName("sensorFields");
        // Properties are a compact form, not a canvas that fills a wide document pane.
        body->setMaximumWidth(640);
        layout = new QVBoxLayout(body);
        layout->setContentsMargins(theme::space::page);
        layout->setSpacing(12);
        new WidthObserver(body, [rows = layout, mode = WidthMode{}](int width) mutable {
            const auto margins = theme::space::page;
            rows->setContentsMargins(mode.narrow(width, 480)
                                         ? QMargins(16, margins.top(), 16, margins.bottom())
                                         : margins);
        });
        if (!heading.isEmpty()) {
            auto *label = new QLabel(heading, body);
            label->setObjectName("panelTitle");
            layout->addWidget(label);
        }
        scroll->setWidget(body);
        pages->addWidget(scroll);
    };
    page("sensorScroll");

    auto *heading = new QHBoxLayout;
    heading->setSpacing(12);
    auto *icon = new QLabel(body);
    icon->setObjectName("sensorBadge");
    icon->setFixedSize(28, 28);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(workbench_icon(WorkbenchIcon::focus, theme::color(theme::accent))
                        .pixmap(QSize(18, 18), devicePixelRatioF()));
    title_ = new QLabel(ui::text("sensor_editor.heading_text"), body);
    title_->setObjectName("sensorTitle");
    title_->setToolTip(ui::text("sensor_editor.subtitle"));
    heading->addWidget(icon);
    heading->addWidget(title_, 1);
    layout->addLayout(heading);

    auto *parent_row = new QFrame(body);
    parent_row->setObjectName("sensorIdentity");
    auto *parent_layout = new QHBoxLayout(parent_row);
    parent_layout->setContentsMargins(0, 0, 0, 0);
    parent_layout->setSpacing(8);
    auto *parent_icon = new QLabel(parent_row);
    parent_icon->setPixmap(
        workbench_icon(WorkbenchIcon::satellite).pixmap(QSize(16, 16), devicePixelRatioF()));
    auto *parent_caption = new QLabel(ui::text("sensor_editor.parent_caption"), parent_row);
    parent_caption->setObjectName("sensorCaption");
    parent_ = new SensorParentLabel(parent_row);
    parent_->setObjectName("sensorParentName");
    parent_layout->addWidget(parent_icon);
    parent_layout->addWidget(parent_caption);
    parent_layout->addWidget(parent_, 1);
    layout->addWidget(parent_row);

    auto *name_row = new QHBoxLayout;
    name_row->setSpacing(8);
    auto *name_label = new QLabel(ui::text("sensor_editor.name_label"), body);
    name_label->setFixedWidth(80);
    name_ = new QLineEdit(body);
    name_->setObjectName("sensorName");
    name_->setAccessibleName(ui::text("sensor_editor.name_label"));
    name_->setMaximumWidth(360);
    name_->setMaxLength(16384);
    name_->setPlaceholderText(ui::text("sensor_editor.name.set_placeholder_text"));
    name_->setClearButtonEnabled(true);
    name_label->setBuddy(name_);
    name_row->addWidget(name_label);
    name_row->addWidget(name_, 1);
    name_row->addStretch();
    layout->addLayout(name_row);

    auto *type_label = new QLabel(ui::text("sensor.geometry.type"), body);
    type_label->setFixedWidth(80);
    type_ = new QComboBox(body);
    type_->setObjectName("sensorType");
    type_->setMaximumWidth(360);
    for (const auto *key : {"sensor.geometry.simple", "sensor.geometry.complex",
                            "sensor.geometry.rectangular", "sensor.geometry.custom"})
        type_->addItem(ui::text(key));
    type_label->setBuddy(type_);
    auto *type_row = new QHBoxLayout;
    type_row->setSpacing(8);
    type_row->addWidget(type_label);
    type_row->addWidget(type_, 1);
    type_row->addStretch();
    layout->addLayout(type_row);

    auto field = [this](QVBoxLayout *rows, QWidget *owner, const char *key, const char *name,
                        double low, double high, Unit unit = Unit::degree) {
        auto *label = new QLabel(ui::text(key), owner);
        label->setWordWrap(true);
        auto *editor = new QuantityEditor(unit, owner);
        editor->setObjectName(name);
        editor->set_label(ui::text(key));
        editor->set_range(low, high, unit);
        label->setBuddy(editor->findChild<QLineEdit *>("quantityValue"));
        rows->addWidget(label);
        rows->addWidget(editor);
        connect(editor, &QuantityEditor::valueChanged, this, [this] { update_preview(); });
        connect(editor, &QuantityEditor::unitChanged, this, [this] { update_preview(); });
        return editor;
    };
    const auto hint = [](QVBoxLayout *rows, QWidget *owner, const char *key) {
        auto *label = new QLabel(ui::text(key), owner);
        label->setWordWrap(true);
        label->setObjectName("sensorCaption");
        rows->addWidget(label);
    };
    const auto section = [](QVBoxLayout *rows, QWidget *owner, const char *key) {
        auto *label = new QLabel(ui::text(key), owner);
        label->setObjectName("sensorSectionTitle");
        rows->addWidget(label);
    };
    auto *fov = new QFrame(body);
    fov->setObjectName("sensorFov");
    auto *fov_layout = new QHBoxLayout(fov);
    fov_layout->setContentsMargins(12, 12, 12, 12);
    fov_layout->setSpacing(16);
    auto *parameters = new QWidget(fov);
    auto *parameter_rows = new QVBoxLayout(parameters);
    parameter_rows->setContentsMargins(0, 0, 0, 0);
    parameter_rows->setSpacing(16);
    preview_ = new SensorAnglePreview(fov);
    fov_layout->addWidget(parameters, 1);
    fov_layout->addWidget(preview_, 0, Qt::AlignTop | Qt::AlignHCenter);
    responsive_columns(fov, fov_layout, 480);
    layout->addWidget(fov);
    auto group = [&](QWidget *&widget, const char *heading) {
        widget = new QWidget(parameters);
        auto *rows = new QVBoxLayout(widget);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(8);
        parameter_rows->addWidget(widget);
        if (heading)
            section(rows, widget, heading);
        return rows;
    };
    const auto pair = [](QVBoxLayout *rows, QWidget *owner) {
        auto *container = new QWidget(owner);
        // Let the row shrink below its two-column minimum so wider unit buttons can
        // trigger the existing stacked layout instead of widening the scroll content.
        container->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *columns = new QHBoxLayout(container);
        columns->setContentsMargins(0, 0, 0, 0);
        columns->setSpacing(12);
        responsive_columns(container, columns, 310);
        rows->addWidget(container);
        return columns;
    };
    const auto column = [](QBoxLayout *columns, QWidget *&widget) {
        widget = new QWidget(columns->parentWidget());
        auto *rows = new QVBoxLayout(widget);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(6);
        rows->setAlignment(Qt::AlignTop);
        columns->addWidget(widget, 1);
        return rows;
    };
    auto *cone_rows = group(cone_fields_, "sensor.geometry.half_angles");
    auto *half_columns = pair(cone_rows, cone_fields_);
    QWidget *outer_field = nullptr;
    auto *outer_rows = column(half_columns, outer_field);
    angle_ = field(outer_rows, outer_field, "sensor_editor.angle_label", "sensorHalfAngle", .001,
                   179.999);
    angle_label_ = outer_field->findChild<QLabel *>();
    full_angle_ = new QLabel(outer_field);
    full_angle_->setObjectName("sensorFullAngle");
    full_angle_->setWordWrap(true);
    outer_rows->addWidget(full_angle_);
    auto *inner_rows = column(half_columns, inner_fields_);
    inner_ =
        field(inner_rows, inner_fields_, "sensor.geometry.inner", "sensorInnerAngle", 0, 179.998);
    auto *complex_rows = group(complex_fields_, "sensor.geometry.clock_angles");
    auto *clock_columns = pair(complex_rows, complex_fields_);
    QWidget *clock_start = nullptr, *clock_end = nullptr;
    auto *start_rows = column(clock_columns, clock_start);
    auto *end_rows = column(clock_columns, clock_end);
    clock_min_ =
        field(start_rows, clock_start, "sensor.geometry.clock_min", "sensorClockMin", 0, 360);
    clock_max_ = field(end_rows, clock_end, "sensor.geometry.clock_max", "sensorClockMax", 0, 360);
    hint(complex_rows, complex_fields_, "sensor.geometry.clock_hint");
    auto *rect_rows = group(rectangle_fields_, "sensor.geometry.half_angles");
    auto *rect_columns = pair(rect_rows, rectangle_fields_);
    QWidget *horizontal_field = nullptr, *vertical_field = nullptr;
    auto *horizontal_rows = column(rect_columns, horizontal_field);
    auto *vertical_rows = column(rect_columns, vertical_field);
    horizontal_ = field(horizontal_rows, horizontal_field, "sensor.geometry.horizontal",
                        "sensorHorizontal", .001, 89.999);
    vertical_ = field(vertical_rows, vertical_field, "sensor.geometry.vertical", "sensorVertical",
                      .001, 89.999);
    auto *custom_rows = group(custom_fields_, nullptr);
    auto *custom_hint = new QLabel(ui::text("sensor.geometry.custom_hint"), custom_fields_);
    custom_hint->setWordWrap(true);
    custom_rows->addWidget(custom_hint);
    points_ = new QTableWidget(0, 2, custom_fields_);
    points_->setObjectName("sensorPatternPoints");
    points_->setHorizontalHeaderLabels(
        {ui::text("sensor.geometry.cone_column"), ui::text("sensor.geometry.clock_column")});
    points_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    points_->setSelectionBehavior(QAbstractItemView::SelectRows);
    points_->setMinimumHeight(160);
    points_->setMaximumHeight(240);
    custom_rows->addWidget(points_);
    auto *custom_buttons = new QHBoxLayout;
    for (const auto *key :
         {"sensor.geometry.add_point", "sensor.geometry.remove_point", "sensor.geometry.import"}) {
        auto *button = new QPushButton(ui::text(key), custom_fields_);
        button->setAutoDefault(false);
        custom_buttons->addWidget(button);
        const auto action = custom_buttons->count();
        connect(button, &QPushButton::clicked, this, [this, action] {
            if (action == 1) {
                if (points_->rowCount() >= 256)
                    return;
                const int row = points_->rowCount();
                points_->insertRow(row);
                points_->setItem(row, 0, new QTableWidgetItem(QString::number(30)));
                points_->setItem(row, 1, new QTableWidgetItem(QString::number(0)));
                points_->setCurrentCell(row, 0);
                points_->editItem(points_->item(row, 0));
            } else if (action == 2) {
                auto selected = points_->selectionModel()->selectedRows();
                std::sort(selected.begin(), selected.end(),
                          [](const auto &x, const auto &y) { return x.row() > y.row(); });
                for (const auto &row : selected)
                    points_->removeRow(row.row());
            } else {
                const auto path =
                    QFileDialog::getOpenFileName(this, ui::text("sensor.geometry.import"), {},
                                                 ui::text("sensor.geometry.filter"));
                if (path.isEmpty())
                    return;
                try {
                    QFile file(path);
                    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024)
                        throw std::runtime_error("read");
                    const auto imported = core::parse_sensor_pattern(file.readAll().toStdString());
                    const QSignalBlocker blocker(points_);
                    points_->setRowCount(0);
                    for (const auto &point : imported.custom_points) {
                        const int row = points_->rowCount();
                        points_->insertRow(row);
                        points_->setItem(row, 0, angle_item(point.cone_rad));
                        points_->setItem(row, 1, angle_item(point.clock_rad));
                    }
                    show_error({});
                } catch (const std::exception &) {
                    show_error(ui::text("sensor.geometry.import_error"));
                }
            }
            update_preview();
        });
    }
    custom_rows->addLayout(custom_buttons);
    new WidthObserver(custom_fields_, [custom_buttons](int width) {
        custom_buttons->setDirection(width < 300 ? QBoxLayout::TopToBottom
                                                 : QBoxLayout::LeftToRight);
    });
    connect(points_, &QTableWidget::itemChanged, this, [this] { update_preview(); });

    layout->addStretch();
    page("sensorPointingScroll", ui::text("sensor.pages.pointing"));
    auto *pointing_layout = new QVBoxLayout;
    pointing_layout->setSpacing(8);
    pointing_layout->addWidget(
        new QLabel(ui::text("sensor_editor.pointing_layout.add_widget"), body));
    auto *choices = new QHBoxLayout;
    choices->setSpacing(8);
    pointing_ = new QButtonGroup(this);
    auto *nadir = new QPushButton(workbench_icon(WorkbenchIcon::earth),
                                  ui::text("sensor_editor.nadir"), body);
    auto *targeted = new QPushButton(workbench_icon(WorkbenchIcon::ground_station),
                                     ui::text("sensor_editor.targeted"), body);
    nadir->setObjectName("sensorNadir");
    targeted->setObjectName("sensorTargeted");
    for (auto *button : {nadir, targeted}) {
        button->setProperty("sensorChoice", true);
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(40);
        choices->addWidget(button, 1);
    }
    pointing_->addButton(nadir, static_cast<int>(application::SensorPointing::nadir));
    pointing_->addButton(targeted, static_cast<int>(application::SensorPointing::targeted));
    pointing_layout->addLayout(choices);
    pointing_hint_ = new QLabel(body);
    pointing_hint_->setObjectName("sensorCaption");
    pointing_hint_->setWordWrap(true);
    pointing_layout->addWidget(pointing_hint_);

    target_row_ = new QWidget(body);
    target_row_->setObjectName("sensorTargetRow");
    auto *target_layout = new QVBoxLayout(target_row_);
    target_layout->setContentsMargins(0, 8, 0, 0);
    target_layout->setSpacing(8);
    auto *target_label = new QLabel(ui::text("sensor_editor.target_label"), target_row_);
    target_ = new ObjectSelectionField(target_row_);
    target_->setObjectName("sensorTarget");
    target_->set_options({ui::text("sensor_editor.label_6"), false, {}});
    target_->setAccessibleName(ui::text("sensor_editor.target_label"));
    target_label->setBuddy(target_);
    target_hint_ = new QLabel(target_row_);
    target_hint_->setObjectName("sensorTargetHint");
    target_hint_->setWordWrap(true);
    target_layout->addWidget(target_label);
    target_layout->addWidget(target_);
    target_layout->addWidget(target_hint_);
    pointing_layout->addWidget(target_row_);
    layout->addLayout(pointing_layout);
    layout->addSpacing(8);
    auto *rotation_card = new QFrame(body);
    rotation_card->setObjectName("sensorRotationCard");
    auto *card_rows = new QVBoxLayout(rotation_card);
    card_rows->setContentsMargins(16, 12, 16, 12);
    card_rows->setSpacing(12);
    auto *rotation_header = new QHBoxLayout;
    rotation_header->setSpacing(6);
    auto *rotation_title = new QLabel(ui::text("sensor.pointing.rotation"), rotation_card);
    rotation_title->setObjectName("sensorSectionTitle");
    rotation_header->addWidget(rotation_title);
    auto *rotation_help = info_marker(rotation_card);
    append_help(rotation_help, ui::text("sensor.geometry.roll_hint"));
    rotation_header->addWidget(rotation_help);
    rotation_header->addStretch();
    auto *zero_rotation = new QPushButton(ui::text("sensor.pointing.zero_rotation"), rotation_card);
    zero_rotation->setObjectName("sensorZeroRotation");
    zero_rotation->setAutoDefault(false);
    rotation_header->addWidget(zero_rotation);
    card_rows->addLayout(rotation_header);
    auto *rotation = new QWidget(rotation_card);
    auto *rotation_columns = new QHBoxLayout(rotation);
    rotation_columns->setContentsMargins(0, 0, 0, 0);
    rotation_columns->setSpacing(24);
    QWidget *rotation_fields = nullptr;
    auto *rotation_rows = column(rotation_columns, rotation_fields);
    rotation_fields->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    rotation_rows->setSpacing(12);
    hint(rotation_rows, rotation_fields, "sensor.pointing.diagram.order");
    const auto rotation_field = [&](int axis, const char *key, const char *name) {
        auto *row = new QHBoxLayout;
        row->setSpacing(10);
        auto *badge = new QLabel(QString(QChar('X' + axis)), rotation_fields);
        badge->setAlignment(Qt::AlignCenter);
        badge->setFixedSize(24, 28);
        badge->setStyleSheet(QStringLiteral("color: %1; font-weight: 600; font-size: 14px;")
                                 .arg(sensor_axis_color(axis).name()));
        badge->setToolTip(ui::text(key));
        auto *editor = new QuantityEditor(Unit::degree, rotation_fields);
        editor->setObjectName(name);
        editor->set_label(ui::text(key));
        editor->set_range(-360, 360, Unit::degree);
        badge->setBuddy(editor->findChild<QLineEdit *>("quantityValue"));
        row->addWidget(badge);
        row->addWidget(editor, 1);
        rotation_rows->addLayout(row);
        connect(editor, &QuantityEditor::valueChanged, this, [this] { update_preview(); });
        connect(editor, &QuantityEditor::unitChanged, this, [this] { update_preview(); });
        return editor;
    };
    tilt_x_ = rotation_field(0, "sensor.pointing.tilt_x", "sensorTiltX");
    tilt_y_ = rotation_field(1, "sensor.pointing.tilt_y", "sensorTiltY");
    roll_ = rotation_field(2, "sensor.geometry.roll", "sensorRoll");
    hint(rotation_rows, rotation_fields, "sensor.pointing.offset_hint");
    orientation_preview_ = new SensorOrientationPreview(rotation);
    rotation_columns->addWidget(orientation_preview_, 1, Qt::AlignTop);
    // Observe the viewport: a horizontal form's minimum width must not prevent
    // the very resize that switches it back to a vertical layout.
    rotation_columns->setDirection(QBoxLayout::TopToBottom);
    auto *pointing_scroll = qobject_cast<QScrollArea *>(pages->widget(1));
    new WidthObserver(
        pointing_scroll->viewport(), [rotation_columns, mode = WidthMode{}](int width) mutable {
            rotation_columns->setDirection(mode.narrow(width, 590) ? QBoxLayout::TopToBottom
                                                                   : QBoxLayout::LeftToRight);
        });
    card_rows->addWidget(rotation);
    layout->addWidget(rotation_card);
    connect(zero_rotation, &QPushButton::clicked, this, [this] {
        for (auto *angle : {tilt_x_, tilt_y_, roll_})
            angle->set_value(0, Unit::radian);
    });
    layout->addStretch();
    page("sensorDisplayScroll", ui::text("sensor.pages.attributes_2d"));
    show_2d_ = new QCheckBox(ui::text("sensor.geometry.show_2d"), body);
    show_2d_->setObjectName("sensorShow2d");
    layout->addWidget(show_2d_);
    layout->addSpacing(8);
    section(layout, body, "sensor.geometry.shared_appearance");
    hint(layout, body, "sensor.geometry.shared_appearance_hint");
    fill_ = new QCheckBox(ui::text("sensor.geometry.fill"), body);
    fill_->setObjectName("sensorFill");
    layout->addWidget(fill_);
    auto *style_row = new QHBoxLayout;
    color_ = new QPushButton(body);
    color_->setObjectName("sensorColor");
    color_->setMaximumWidth(276);
    color_->setIconSize(QSize(22, 14));
    color_->setToolTip(ui::text("sensor.geometry.color"));
    color_->setAutoDefault(false);
    connect(color_, &QPushButton::clicked, this, [this] {
        const auto color = QColorDialog::getColor(QColor::fromRgb(color_value_), this,
                                                  ui::text("sensor.geometry.color"));
        if (color.isValid()) {
            color_value_ = color.rgb() & 0xffffff;
            update_color();
        }
    });
    layout->addWidget(color_, 0, Qt::AlignLeft);
    opacity_ = new QSpinBox(body);
    opacity_->setObjectName("sensorOpacity");
    opacity_->setFixedWidth(100);
    opacity_->setRange(0, 100);
    opacity_->setSuffix(ui::text("sensor.geometry.percent"));
    opacity_->setAccessibleName(ui::text("sensor.geometry.opacity"));
    style_row->addWidget(new QLabel(ui::text("sensor.geometry.opacity"), body));
    style_row->addWidget(opacity_);
    style_row->addStretch();
    layout->addLayout(style_row);
    connect(fill_, &QCheckBox::toggled, opacity_, &QSpinBox::setEnabled);
    layout->addStretch();

    page("sensorProjectionScroll", ui::text("sensor.pages.projection_3d"));
    show_3d_ = new QCheckBox(ui::text("sensor.geometry.show_3d"), body);
    show_3d_->setObjectName("sensorShow3d");
    layout->addWidget(show_3d_);
    layout->addSpacing(8);
    show_space_ = new QCheckBox(ui::text("sensor.geometry.show_space_extension"), body);
    show_space_->setObjectName("sensorShowSpaceExtension");
    layout->addWidget(show_space_);
    range_ = field(layout, body, "sensor.geometry.range", "sensorSpaceRange", .001, 10000000,
                   Unit::kilometre);
    connect(show_space_, &QCheckBox::toggled, range_, &QWidget::setEnabled);
    auto *range_hint = new QLabel(ui::text("sensor.geometry.range_hint"), body);
    range_hint->setWordWrap(true);
    range_hint->setObjectName("sensorCaption");
    layout->addWidget(range_hint);
    layout->addSpacing(8);
    auto *appearance = new QPushButton(ui::text("sensor.geometry.edit_appearance"), body);
    appearance->setObjectName("sensorEditAppearance");
    appearance->setAutoDefault(false);
    layout->addWidget(appearance, 0, Qt::AlignLeft);
    connect(appearance, &QPushButton::clicked, pages, [pages] { pages->setCurrentIndex(2); });
    layout->addStretch();

    auto *footer = new QFrame(this);
    footer->setObjectName("sensorFooter");
    auto *footer_layout = new QVBoxLayout(footer);
    footer_layout->setContentsMargins(theme::space::footer);
    footer_layout->setSpacing(10);
    error_ = new QLabel(footer);
    error_->setObjectName("sensorError");
    error_->setTextFormat(Qt::PlainText);
    error_->setWordWrap(true);
    footer_layout->addWidget(error_);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    close_ = new QPushButton(ui::text("common.close"), footer);
    close_->setObjectName("closeSensor");
    close_->setAutoDefault(false);
    close_->setToolTip(ui::text("sensor_editor.close.set_tool_tip"));
    apply_ = new QPushButton(ui::text("sensor_editor.heading_text"), footer);
    apply_->setObjectName("applySensor");
    apply_->setProperty("primary", true);
    apply_->setDefault(true);
    auto *reset = new QPushButton(ui::text("sensor_editor.reset"), footer);
    reset->setObjectName("resetSensor");
    connect(reset, &QPushButton::clicked, this, [this] {
        name_->setText(QString::fromStdString(original_.name()));
        load_fields();
        pointing_->button(static_cast<int>(original_.pointing))->setChecked(true);
        target_->set_id(original_.target_station_id);
        show_error({});
        update_pointing();
        update_preview();
    });
    buttons->addWidget(reset);
    buttons->addStretch();
    buttons->addWidget(close_);
    buttons->addWidget(apply_);
    footer_layout->addLayout(buttons);
    root->addWidget(footer);

    connect(close_, &QPushButton::clicked, this, &QDialog::close);
    connect(apply_, &QPushButton::clicked, this, [this] {
        try {
            const auto value = sensor();
            if (apply)
                apply(value);
            show_error({});
        } catch (const std::exception &e) {
            show_error(QString::fromUtf8(e.what()));
        }
    });
    connect(pointing_, &QButtonGroup::idToggled, this, [this](int, bool checked) {
        if (checked)
            update_pointing();
    });
    connect(target_, &ObjectSelectionField::selectionChanged, this, [this] { update_pointing(); });
    for (auto *editor : {angle_, tilt_x_, tilt_y_, roll_}) {
        connect(
            editor->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged, this,
            [this] { update_preview(); }, Qt::QueuedConnection);
    }
    nadir->setChecked(true);
    connect(type_, &QComboBox::currentIndexChanged, this, [this] {
        update_type();
        update_preview();
    });
    load_fields();
    show_error({});
    update_preview();
}

void SensorEditor::set_sensor(application::ProjectSensor sensor,
                              const application::Project &project, bool creating) {
    original_ = std::move(sensor);
    name_->setText(QString::fromStdString(original_.name()));
    QString parent_name = QString::fromStdString(original_.parent_satellite_id);
    for (const auto &sat : project.satellites)
        if (sat.orbit.object_id == original_.parent_satellite_id)
            parent_name = QString::fromStdString(sat.name());
    parent_->set_name(parent_name);
    load_fields();
    target_->set_objects(project_object_choices(project, {ObjectKind::ground_station}));
    if (!original_.target_station_id.empty())
        target_->set_id(original_.target_station_id);
    else
        target_->set_id(
            project.ground_stations.empty() ? "" : project.ground_stations.front().object_id);
    if (auto *button = pointing_->button(static_cast<int>(original_.pointing)))
        button->setChecked(true);
    else
        throw std::invalid_argument(ui::utf8("sensor_editor.button"));
    title_->setText(creating ? ui::text("sensor_editor.heading_text")
                             : ui::text("sensor_editor.title.set_text"));
    apply_->setText(creating ? ui::text("sensor_editor.heading_text") : ui::text("common.apply"));
    close_->setText(ui::text("common.close"));
    setWindowTitle(creating ? ui::text("sensor_editor.heading_text")
                            : QString::fromStdString(original_.name()) +
                                  ui::text("sensor_editor.label_7"));
    show_error({});
    update_pointing();
    update_preview();
}

void SensorEditor::update_pointing() {
    const bool targeted =
        pointing_->checkedId() == static_cast<int>(application::SensorPointing::targeted);
    target_row_->setVisible(targeted);
    pointing_hint_->setText(targeted ? ui::text("sensor_editor.pointing_hint.set_text")
                                     : ui::text("sensor_editor.pointing_hint.set_text_2"));
    target_hint_->setText(target_->objects().empty()
                              ? ui::text("sensor_editor.target_hint.set_text")
                              : ui::text("sensor_editor.target_hint.set_text_2"));
    target_hint_->setVisible(!target_->contains(target_->id()));
}

void SensorEditor::update_preview() {
    if (orientation_preview_) {
        try {
            orientation_preview_->set_angles(std::array{tilt_x_->value(Unit::radian),
                                                        tilt_y_->value(Unit::radian),
                                                        roll_->value(Unit::radian)});
        } catch (const std::exception &) {
            orientation_preview_->set_angles(std::nullopt);
        }
    }
    try {
        const auto p = pattern();
        preview_->set_pattern(p);
        const auto degrees = angle_->value(Unit::degree);
        full_angle_->setText(
            ui::text("sensor_editor.full_angle.set_text").arg(degrees * 2, 0, 'g', 6));
        preview_->set_angle(degrees);
    } catch (const std::exception &) {
        full_angle_->setText(ui::text("sensor_editor.full_angle.set_text_2"));
        preview_->set_angle(std::nullopt);
    }
}

void SensorEditor::update_type() {
    const auto type = type_->currentIndex();
    cone_fields_->setVisible(type <= 1);
    inner_fields_->setVisible(type == 1);
    const auto label = ui::text(type == 1 ? "sensor.geometry.outer" : "sensor_editor.angle_label");
    angle_label_->setText(label);
    angle_->set_label(label);
    complex_fields_->setVisible(type == 1);
    rectangle_fields_->setVisible(type == 2);
    custom_fields_->setVisible(type == 3);
}
void SensorEditor::load_fields() {
    const QSignalBlocker blocker(type_), table_blocker(points_);
    const auto &p = original_.pattern;
    type_->setCurrentIndex(static_cast<int>(p.type));
    angle_->set_value(p.half_angle_rad, Unit::radian);
    inner_->set_value(p.inner_half_angle_rad, Unit::radian);
    clock_min_->set_value(p.minimum_clock_rad, Unit::radian);
    clock_max_->set_value(p.maximum_clock_rad, Unit::radian);
    horizontal_->set_value(p.horizontal_half_angle_rad, Unit::radian);
    vertical_->set_value(p.vertical_half_angle_rad, Unit::radian);
    roll_->set_value(original_.roll_rad, Unit::radian);
    tilt_x_->set_value(original_.tilt_x_rad, Unit::radian);
    tilt_y_->set_value(original_.tilt_y_rad, Unit::radian);
    points_->setRowCount(0);
    auto points = p.custom_points;
    if (points.empty())
        for (int i = 0; i < 4; ++i)
            points.push_back({std::numbers::pi / 6, i * std::numbers::pi / 2});
    for (const auto &point : points) {
        const int row = points_->rowCount();
        points_->insertRow(row);
        points_->setItem(row, 0, angle_item(point.cone_rad));
        points_->setItem(row, 1, angle_item(point.clock_rad));
    }
    show_2d_->setChecked(original_.display.show_2d);
    show_3d_->setChecked(original_.display.show_3d);
    fill_->setChecked(original_.display.fill);
    opacity_->setEnabled(original_.display.fill);
    opacity_->setValue(qRound(original_.display.opacity * 100));
    color_value_ = original_.display.color;
    update_color();
    range_->set_value(original_.display.space_range_m, Unit::metre);
    show_space_->setChecked(original_.display.show_space_extension);
    range_->setEnabled(original_.display.show_space_extension);
    update_type();
}
void SensorEditor::update_color() {
    const auto color = QColor::fromRgb(color_value_);
    color_->setText(ui::text("sensor.geometry.color_value").arg(color.name()));
    QPixmap swatch(color_->iconSize() * color_->devicePixelRatioF());
    swatch.setDevicePixelRatio(color_->devicePixelRatioF());
    swatch.fill(Qt::transparent);
    QPainter painter(&swatch);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(theme::color(theme::border_strong), 1));
    painter.setBrush(color);
    painter.drawRoundedRect(
        QRectF(QPointF{}, QSizeF(color_->iconSize())).adjusted(.5, .5, -.5, -.5), 2, 2);
    painter.end();
    color_->setIcon(QIcon(swatch));
}
core::SensorPattern SensorEditor::pattern() const {
    auto p = original_.pattern;
    p.type = static_cast<core::SensorPatternType>(type_->currentIndex());
    if (p.type == core::SensorPatternType::simple_conic ||
        p.type == core::SensorPatternType::complex_conic)
        p.half_angle_rad = angle_->value(Unit::radian);
    if (p.type == core::SensorPatternType::complex_conic) {
        p.inner_half_angle_rad = inner_->value(Unit::radian);
        p.minimum_clock_rad = clock_min_->value(Unit::radian);
        p.maximum_clock_rad = clock_max_->value(Unit::radian);
    }
    if (p.type == core::SensorPatternType::rectangular) {
        p.horizontal_half_angle_rad = horizontal_->value(Unit::radian);
        p.vertical_half_angle_rad = vertical_->value(Unit::radian);
    }
    if (p.type == core::SensorPatternType::custom) {
        p.custom_points.clear();
        for (int row = 0; row < points_->rowCount(); ++row) {
            p.custom_points.push_back(
                {point_angle(points_->item(row, 0)), point_angle(points_->item(row, 1))});
        }
    }
    try {
        (void)core::SensorGeometry(p);
    } catch (const std::exception &) {
        throw std::invalid_argument(ui::utf8("sensor.geometry.invalid"));
    }
    return p;
}

void SensorEditor::show_error(const QString &text) {
    error_->setText(text);
    error_->setVisible(!text.isEmpty());
}

application::ProjectSensor SensorEditor::sensor() const {
    auto s = original_;
    s.display_name = name_->text().trimmed().toStdString();
    if (s.display_name.empty())
        throw std::invalid_argument(ui::utf8("sensor_editor.s"));
    s.pattern = pattern();
    s.roll_rad = roll_->value(Unit::radian);
    s.tilt_x_rad = tilt_x_->value(Unit::radian);
    s.tilt_y_rad = tilt_y_->value(Unit::radian);
    s.display = {show_2d_->isChecked(), show_3d_->isChecked(),     fill_->isChecked(),
                 color_value_,          opacity_->value() / 100.0, range_->value(Unit::metre)};
    s.display.show_space_extension = show_space_->isChecked();
    if (opacity_->value() == qRound(original_.display.opacity * 100))
        s.display.opacity = original_.display.opacity;
    s.pointing = static_cast<application::SensorPointing>(pointing_->checkedId());
    s.target_station_id = s.pointing == application::SensorPointing::targeted ? target_->id() : "";
    if (s.pointing == application::SensorPointing::targeted &&
        !target_->contains(s.target_station_id))
        throw std::invalid_argument(target_->objects().empty()
                                        ? ui::utf8("sensor_editor.validation")
                                        : ui::utf8("sensor_editor.label_8"));
    return s;
}
bool SensorEditor::has_unapplied_changes() const {
    try {
        return !same_sensor_input(sensor(), original_);
    } catch (const std::exception &) {
        return true;
    }
}
void SensorEditor::accept_sensor(const application::ProjectSensor &sensor) {
    original_ = sensor;
    title_->setText(ui::text("sensor_editor.title.set_text"));
    apply_->setText(ui::text("common.apply"));
    close_->setText(ui::text("common.close"));
}
void SensorEditor::sync_name(const std::string &name) {
    if (name_->text().trimmed().toStdString() == original_.name())
        name_->setText(QString::fromStdString(name));
    original_.display_name = name;
}
void SensorEditor::update_project(const application::Project &project) {
    for (const auto &satellite : project.satellites)
        if (satellite.orbit.object_id == original_.parent_satellite_id)
            parent_->set_name(QString::fromStdString(satellite.name()));
    target_->set_objects(project_object_choices(project, {ObjectKind::ground_station}));
    update_pointing();
}
} // namespace orbelle::desktop
