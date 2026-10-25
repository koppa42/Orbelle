#include "region_editor.hpp"
#include "../ui_text.hpp"
#include "form_kit.hpp"
#include "ground_map.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbelle::desktop {
namespace {
// Embedded editors consume mouse/focus events before the table sees them. Keep
// its selection on the edited vertex, including after rows are inserted/removed.
class VertexSelectionFilter final : public QObject {
  public:
    VertexSelectionFilter(QuantityEditor *field, QTableWidget *table, const QModelIndex &index)
        : QObject(field), table_(table), index_(index) {
        field->installEventFilter(this);
        field->findChild<QLineEdit *>()->installEventFilter(this);
        field->findChild<QToolButton *>()->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (index_.isValid() &&
            (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress))
            table_->setCurrentCell(index_.row(), index_.column(),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        return QObject::eventFilter(object, event);
    }

  private:
    QTableWidget *table_;
    QPersistentModelIndex index_;
};
} // namespace
class RegionBoundaryPreview final : public QWidget {
  public:
    explicit RegionBoundaryPreview(QWidget *parent) : QWidget(parent) {
        setObjectName("regionBoundaryPreview");
        setAccessibleName(ui::text("region.preview.title"));
        setMinimumWidth(200);
        setFixedHeight(210);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        new WidthObserver(
            this, [this](int width) { setFixedHeight(int(std::min(width - 64., 292.) / 2 + 64)); });
    }
    void set_boundary(std::optional<core::RegionBoundary> boundary, QColor color) {
        if (boundary == boundary_ && color == color_)
            return;
        boundary_ = std::move(boundary);
        color_ = color;
        fill_ = {};
        outline_ = {};
        if (boundary_) {
            const auto points = core::region_outline(*boundary_);
            fill_ = ground_region_path(points);
            std::vector<QPointF> coordinates;
            for (const auto &p : points)
                coordinates.emplace_back(
                    std::remainder(p.longitude_rad * 180 / std::numbers::pi, 360.),
                    p.latitude_rad * 180 / std::numbers::pi);
            outline_ = ground_path(coordinates);
            const auto bounds = core::region_grid_bounds(*boundary_);
            const auto degrees = 180 / std::numbers::pi;
            const auto span = core::coverage_longitude_span(bounds) * degrees;
            const auto extent = std::min(
                360., 1.35 * std::max(span, 2 * (bounds.north_rad - bounds.south_rad) * degrees));
            const auto centre = std::remainder(bounds.west_rad * degrees + span / 2, 360.);
            const auto south =
                std::clamp((bounds.north_rad + bounds.south_rad) * degrees / 2 - extent / 4, -90.,
                           90. - extent / 2);
            view_ = {centre - extent / 2, south, extent, extent / 2};
        }
        setAccessibleDescription(
            ui::text(boundary_ ? "region.preview.hint" : "region.preview.invalid"));
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(theme::color(theme::line), 1));
        painter.setBrush(theme::color(theme::field));
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 6, 6);
        if (!boundary_) {
            painter.setPen(theme::color(theme::text_muted));
            painter.drawText(QRectF(rect()).adjusted(16, 16, -16, -16),
                             Qt::AlignCenter | Qt::TextWordWrap,
                             ui::text("region.preview.invalid"));
            return;
        }
        auto tick_font = painter.font();
        tick_font.setPointSizeF(tick_font.pointSizeF() * .85);
        const QFontMetricsF metrics(tick_font);
        std::array<QString, 3> latitudes, longitudes;
        std::array<qreal, 3> longitude_widths;
        qreal latitude_width = 0;
        for (int i = 0; i <= 2; ++i) {
            latitudes[i] = QString::number(view_.bottom() - i * view_.height() / 2, 'g', 3) +
                           QStringLiteral("°");
            longitudes[i] =
                QString::number(std::remainder(view_.left() + i * view_.width() / 2, 360.), 'g',
                                4) +
                QStringLiteral("°");
            latitude_width = std::max(latitude_width, metrics.horizontalAdvance(latitudes[i]));
            longitude_widths[i] = std::ceil(metrics.horizontalAdvance(longitudes[i]));
        }
        latitude_width = std::ceil(latitude_width);
        const auto tick_height = std::ceil(metrics.height());
        constexpr qreal padding = 16, tick_gap = 8;
        const auto left = padding + std::max(latitude_width + tick_gap, longitude_widths[0] / 2);
        const auto right = padding + longitude_widths[2] / 2;
        const auto map_width = std::max(1., std::min(width() - left - right, 292.));
        const auto top = padding + tick_height / 2;
        const auto bottom = padding + tick_gap + tick_height;
        const QRectF map(left + std::max(0., (width() - left - right - map_width) / 2),
                         top + (height() - top - bottom - map_width / 2) / 2, map_width,
                         map_width / 2);
        const QTransform transform(map.width() / view_.width(), 0, 0,
                                   -map.height() / view_.height(),
                                   map.left() - view_.left() * map.width() / view_.width(),
                                   map.bottom() + view_.top() * map.height() / view_.height());
        painter.setPen(QPen(theme::color(theme::line), 1, Qt::DotLine));
        for (int i = 1; i < 4; ++i) {
            const auto x = map.left() + map.width() * i / 4;
            const auto y = map.top() + map.height() * i / 4;
            painter.drawLine(QPointF(x, map.top()), QPointF(x, map.bottom()));
            painter.drawLine(QPointF(map.left(), y), QPointF(map.right(), y));
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(theme::color(theme::border), 1));
        painter.drawRect(map);
        auto tint = color_;
        tint.setAlphaF(.22f);
        painter.save();
        painter.setClipRect(map.adjusted(-1, -1, 1, 1));
        painter.setPen(QPen(color_, 1.8));
        // Repeat the clipped pieces around the fitted longitude range so a
        // date-line region appears continuous in its local preview.
        for (const double offset : {-360., 0., 360.}) {
            painter.fillPath(transform.map(fill_.translated(offset, 0)), tint);
            painter.drawPath(transform.map(outline_.translated(offset, 0)));
            if (boundary_->vertices.size() <= 24) {
                painter.setBrush(color_);
                for (const auto &p : boundary_->vertices)
                    painter.drawEllipse(
                        transform.map(QPointF(p.longitude_rad * 180 / std::numbers::pi + offset,
                                              p.latitude_rad * 180 / std::numbers::pi)),
                        2.5, 2.5);
                painter.setBrush(Qt::NoBrush);
            }
        }
        painter.restore();
        painter.setFont(tick_font);
        painter.setPen(theme::color(theme::text_muted));
        for (int i = 0; i <= 2; ++i) {
            painter.drawText(QRectF(map.left() - tick_gap - latitude_width,
                                    map.top() + i * map.height() / 2 - tick_height / 2,
                                    latitude_width, tick_height),
                             Qt::AlignRight | Qt::AlignVCenter, latitudes[i]);
            // Keep endpoint labels readable when the preview becomes narrow.
            if (i == 1 && (map.width() < longitude_widths[0] + longitude_widths[1] + 2 * tick_gap ||
                           map.width() < longitude_widths[1] + longitude_widths[2] + 2 * tick_gap))
                continue;
            painter.drawText(QRectF(map.left() + i * map.width() / 2 - longitude_widths[i] / 2,
                                    map.bottom() + tick_gap, longitude_widths[i], tick_height),
                             Qt::AlignCenter, longitudes[i]);
        }
    }

  private:
    std::optional<core::RegionBoundary> boundary_;
    QColor color_;
    QPainterPath fill_, outline_;
    QRectF view_;
};
RegionEditor::RegionEditor(QWidget *parent) : QDialog(parent) {
    preview_timer_ = new QTimer(this);
    preview_timer_->setSingleShot(true);
    connect(preview_timer_, &QTimer::timeout, this, [this] { update_preview(); });
    setObjectName("regionProperties");
    setWindowIcon(workbench_icon(WorkbenchIcon::map));
    resize(960, 640);
    setMinimumSize(320, 280);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    fields_ = new QWidget(this);
    auto *outer = new QHBoxLayout(fields_);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *compact = new QWidget(fields_);
    auto *compact_layout = new QHBoxLayout(compact);
    compact_layout->setContentsMargins(16, 12, 16, 0);
    auto *selector = new QComboBox(compact);
    selector->setObjectName("propertyPageSelector");
    selector->setAccessibleName(ui::text("region.pages.navigation"));
    auto *tabs = new QTabBar(compact);
    tabs->setObjectName("propertyPageTabs");
    tabs->setAccessibleName(ui::text("region.pages.navigation"));
    tabs->setDrawBase(false);
    tabs->setExpanding(false);
    tabs->setUsesScrollButtons(true);
    auto *navigation = new QTreeWidget(fields_);
    navigation->setObjectName("propertyNavigation");
    navigation->setAccessibleName(ui::text("region.pages.navigation"));
    navigation->setFixedWidth(164);
    navigation->setHeaderHidden(true);
    navigation->setRootIsDecorated(false);
    navigation->setIndentation(16);
    navigation->setUniformRowHeights(true);
    const QStringList labels{ui::text("region.pages.boundary"), ui::text("region.pages.grid"),
                             ui::text("region.description"), ui::text("region.pages.display")};
    const auto category = [&](const QString &heading) {
        auto *item = new QTreeWidgetItem(navigation, {heading});
        item->setFlags(Qt::ItemIsEnabled);
        auto font = item->font(0);
        font.setWeight(QFont::DemiBold);
        item->setFont(0, font);
        item->setExpanded(true);
        return item;
    };
    auto *basic = category(ui::text("region.pages.basic"));
    auto *graphics = category(ui::text("region.pages.graphics"));
    std::vector<QTreeWidgetItem *> page_items;
    for (int i = 0; i < labels.size(); ++i) {
        selector->addItem(labels[i]);
        tabs->addTab(labels[i]);
        auto *item = new QTreeWidgetItem(i == 3 ? graphics : basic, {labels[i]});
        item->setData(0, Qt::UserRole, i);
        page_items.push_back(item);
    }
    compact_layout->addWidget(selector, 1);
    compact_layout->addWidget(tabs);
    compact_layout->addStretch();
    auto *sections = new QStackedWidget(fields_);
    sections->setObjectName("regionPages");
    sections->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    outer->addWidget(compact);
    outer->addWidget(navigation);
    outer->addWidget(sections, 1);
    outer->setDirection(QBoxLayout::TopToBottom);
    navigation->hide();
    tabs->hide();
    layout->addWidget(fields_, 1);
    connect(selector, &QComboBox::currentIndexChanged, sections, &QStackedWidget::setCurrentIndex);
    connect(tabs, &QTabBar::currentChanged, sections, &QStackedWidget::setCurrentIndex);
    connect(navigation, &QTreeWidget::currentItemChanged, sections,
            [sections](QTreeWidgetItem *item) {
                if (item && item->data(0, Qt::UserRole).isValid())
                    sections->setCurrentIndex(item->data(0, Qt::UserRole).toInt());
            });
    connect(sections, &QStackedWidget::currentChanged, this, [=](int index) {
        const QSignalBlocker a(selector), b(tabs), c(navigation);
        selector->setCurrentIndex(index);
        tabs->setCurrentIndex(index);
        navigation->setCurrentItem(page_items.at(index));
    });
    new WidthObserver(fields_, [=, mode = WidthMode{}, tight = WidthMode{}](int width) mutable {
        const bool narrow = mode.narrow(width, 680), small = tight.narrow(width, 440);
        navigation->setVisible(!narrow);
        compact->setVisible(narrow);
        selector->setVisible(narrow && small);
        tabs->setVisible(narrow && !small);
        outer->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    });
    QWidget *body = nullptr;
    QVBoxLayout *rows = nullptr;
    const auto page = [&](const char *name, const QString &heading) {
        auto *scroll = new QScrollArea(sections);
        scroll->setObjectName(name);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        body = new QWidget;
        body->setMaximumWidth(640);
        rows = new QVBoxLayout(body);
        rows->setContentsMargins(theme::space::page);
        rows->setSpacing(theme::space::sm);
        new WidthObserver(body, [rows, mode = WidthMode{}](int width) mutable {
            const auto margins = theme::space::page;
            rows->setContentsMargins(mode.narrow(width, 480)
                                         ? QMargins(16, margins.top(), 16, margins.bottom())
                                         : margins);
        });
        auto *title = new QLabel(heading, body);
        title->setObjectName("panelTitle");
        rows->addWidget(title);
        scroll->setWidget(body);
        sections->addWidget(scroll);
    };
    const auto group = [&](const QString &heading, const char *name) {
        auto *frame = new FormGroup(heading, name, body);
        rows->addWidget(frame);
        responsive_form(frame->form());
        return frame;
    };
    page("regionBoundaryScroll", ui::text("region.title"));
    body->setMaximumWidth(960);
    auto *identity = new QWidget(body);
    auto *identity_form = new QFormLayout(identity);
    identity_form->setContentsMargins(0, theme::space::sm, 0, theme::space::sm);
    identity_form->setHorizontalSpacing(theme::space::md);
    identity_form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    responsive_form(identity_form);
    rows->addWidget(identity);
    name_ = new QLineEdit(identity);
    name_->setObjectName("regionName");
    name_->setMaximumWidth(276);
    name_->setMaxLength(16384);
    name_->setAccessibleName(ui::text("region.name"));
    identity_form->addRow(ui::text("region.name"), name_);
    auto *boundary_content = new QWidget(body);
    auto *boundary_layout = new QHBoxLayout(boundary_content);
    boundary_layout->setContentsMargins(0, 0, 0, 0);
    boundary_layout->setSpacing(theme::space::xl);
    auto *boundary =
        new FormGroup(ui::text("region.pages.boundary"), "regionBoundaryGroup", boundary_content);
    boundary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    boundary_layout->addWidget(boundary, 3, Qt::AlignTop);
    auto *preview_group =
        new FormGroup(ui::text("region.preview.title"), "regionPreviewGroup", boundary_content);
    preview_group->add_help(ui::text("region.preview.hint"));
    preview_ = new RegionBoundaryPreview(preview_group);
    preview_group->form()->addRow(preview_);
    boundary_layout->addWidget(preview_group, 2, Qt::AlignTop);
    responsive_columns(boundary_content, boundary_layout, 620);
    rows->addWidget(boundary_content);
    responsive_form(boundary->form(), 340);
    boundary->set_summary(QStringLiteral("WGS84"));
    boundary->add_help(ui::text("region.rectangle_hint"));
    auto *form = boundary->form();
    shape_ = new QComboBox(body);
    shape_->setObjectName("regionShape");
    shape_->setMaximumWidth(276);
    shape_->setAccessibleName(ui::text("region.shape"));
    shape_->addItems({ui::text("region.rectangle"), ui::text("region.polygon")});
    form->addRow(ui::text("region.shape"), shape_);
    auto *shape_fields = new QWidget(body);
    shape_fields->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *shape_layout = new QVBoxLayout(shape_fields);
    shape_layout->setContentsMargins(0, 0, 0, 0);
    shape_layout->setSpacing(0);
    auto *rectangle = new QWidget(shape_fields);
    auto *rectangle_form = new QFormLayout(rectangle);
    rectangle_form->setContentsMargins(0, 0, 0, 0);
    rectangle_form->setHorizontalSpacing(theme::space::md);
    rectangle_form->setVerticalSpacing(theme::space::row);
    responsive_form(rectangle_form, 340);
    const std::array keys{"region.south", "region.north", "region.west", "region.east"};
    const std::array names{"regionSouth", "regionNorth", "regionWest", "regionEast"};
    for (std::size_t i = 0; i < bounds_.size(); ++i) {
        bounds_[i] = new QuantityEditor(Unit::degree, rectangle);
        bounds_[i]->setObjectName(names[i]);
        bounds_[i]->set_label(ui::text(keys[i]));
        bounds_[i]->set_range(i < 2 ? -90 : -180, i < 2 ? 90 : 180, Unit::degree);
        rectangle_form->addRow(ui::text(keys[i]), bounds_[i]);
        connect(bounds_[i]->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged, this,
                [this] { request_preview(); });
        connect(bounds_[i], &QuantityEditor::valueChanged, this, [this] { request_preview(); });
    }
    auto *global = new QPushButton(ui::text("report_dialog.global"), rectangle);
    global->setObjectName("regionGlobal");
    global->setAutoDefault(false);
    global->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    rectangle_form->addRow(QString{}, global);
    connect(global, &QPushButton::clicked, this, [this] {
        bounds_[0]->set_value(-90, Unit::degree);
        bounds_[1]->set_value(90, Unit::degree);
        bounds_[2]->set_value(-180, Unit::degree);
        bounds_[3]->set_value(180, Unit::degree);
    });
    shape_layout->addWidget(rectangle);
    auto *polygon = new QWidget(shape_fields);
    auto *polygon_layout = new QVBoxLayout(polygon);
    polygon_layout->setContentsMargins(0, 0, 0, 0);
    vertices_ = new QTableWidget(0, 2, polygon);
    vertices_->setObjectName("regionVertices");
    vertices_->setHorizontalHeaderLabels(
        {ui::text("region.longitude"), ui::text("region.latitude")});
    vertices_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    vertices_->horizontalHeader()->setMinimumSectionSize(154);
    vertices_->verticalHeader()->setObjectName("regionVertexNumbers");
    vertices_->setSelectionBehavior(QAbstractItemView::SelectRows);
    vertices_->setSelectionMode(QAbstractItemView::SingleSelection);
    vertices_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vertices_->setMinimumHeight(190);
    vertices_->setMaximumHeight(300);
    polygon_layout->addWidget(vertices_);
    auto *row_buttons = new QHBoxLayout;
    auto *add = new QPushButton(ui::text("region.add_vertex"), polygon);
    add->setObjectName("regionAddVertex");
    add->setAutoDefault(false);
    auto *remove = new QPushButton(ui::text("region.remove_vertex"), polygon);
    remove->setObjectName("regionRemoveVertex");
    remove->setAutoDefault(false);
    remove->setEnabled(false);
    row_buttons->addWidget(add);
    row_buttons->addWidget(remove);
    row_buttons->addStretch();
    polygon_layout->addLayout(row_buttons);
    connect(vertices_, &QTableWidget::itemSelectionChanged, remove, [this, remove] {
        remove->setEnabled(!vertices_->selectionModel()->selectedRows().isEmpty());
    });
    connect(add, &QPushButton::clicked, this, [this] {
        if (vertices_->rowCount() < 256) {
            append_vertex({});
            const auto row = vertices_->rowCount() - 1;
            vertices_->setCurrentCell(row, 0);
            vertices_->scrollTo(vertices_->model()->index(row, 0));
            vertices_->cellWidget(row, 0)->setFocus();
        }
        request_preview();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto selection = vertices_->selectionModel()->selectedRows();
        if (selection.isEmpty())
            return;
        const auto row = selection.front().row();
        vertices_->removeRow(row);
        if (vertices_->rowCount() > 0)
            vertices_->setCurrentCell(std::min(row, vertices_->rowCount() - 1), 0);
        request_preview();
    });
    auto *polygon_hint = caution_label(ui::text("region.polygon_limits"));
    polygon_layout->addWidget(polygon_hint);
    boundary->add_help(ui::text("region.polygon_hint"));
    shape_layout->addWidget(polygon);
    polygon->hide();
    // Hidden fields keep their draft without imposing their height on the
    // active shape, so the diagram follows the last visible field directly.
    connect(shape_, &QComboBox::currentIndexChanged, shape_fields, [rectangle, polygon](int index) {
        rectangle->setVisible(index == 0);
        polygon->setVisible(index == 1);
    });
    connect(shape_, &QComboBox::currentIndexChanged, this, [this] { request_preview(); });
    form->addRow(shape_fields);
    align_form_labels(body);
    rows->addStretch();
    page("regionGridScroll", ui::text("region.pages.grid"));
    auto *grid = group(ui::text("region.grid_sampling"), "regionGridGroup");
    grid->setProperty("leading", true);
    grid->add_help(ui::text("region.grid_estimate"));
    grid_spacing_ = new QuantityEditor(Unit::degree, body);
    grid_spacing_->setObjectName("regionGridSpacing");
    grid_spacing_->set_label(ui::text("report_dialog.label_90"));
    grid_spacing_->set_range(0, 180, Unit::degree);
    grid->form()->addRow(ui::text("report_dialog.label_90"), grid_spacing_);
    align_form_labels(body);
    rows->addStretch();
    page("regionDescriptionScroll", ui::text("region.description"));
    description_ = new QPlainTextEdit(body);
    description_->setObjectName("regionDescription");
    description_->setAccessibleName(ui::text("region.description"));
    description_->setMinimumHeight(180);
    rows->addWidget(description_, 1);
    page("regionDisplayScroll", ui::text("region.pages.display"));
    auto *visibility = group(ui::text("region.display.visibility"), "regionVisibilityGroup");
    visibility->setProperty("leading", true);
    show_2d_ = new QCheckBox(ui::text("region.display.show_2d"), body);
    show_2d_->setObjectName("regionShow2d");
    show_3d_ = new QCheckBox(ui::text("region.display.show_3d"), body);
    show_3d_->setObjectName("regionShow3d");
    visibility->form()->addRow(show_2d_);
    visibility->form()->addRow(show_3d_);
    auto *appearance = group(ui::text("region.display.appearance"), "regionAppearanceGroup");
    appearance->add_help(ui::text("region.display.shared_help"));
    color_ = new QPushButton(body);
    color_->setObjectName("regionColor");
    color_->setMaximumWidth(276);
    color_->setIconSize(QSize(22, 14));
    color_->setAutoDefault(false);
    color_->setAccessibleName(ui::text("region.display.color"));
    appearance->form()->addRow(ui::text("region.display.color"), color_);
    connect(color_, &QPushButton::clicked, this, [this] {
        const auto color = QColorDialog::getColor(QColor::fromRgb(color_value_), this,
                                                  ui::text("region.display.color"));
        if (color.isValid()) {
            color_value_ = color.rgb() & 0xffffff;
            update_color();
        }
    });
    fill_ = new QCheckBox(ui::text("region.display.fill"), body);
    fill_->setObjectName("regionFill");
    appearance->form()->addRow(fill_);
    opacity_ = new QDoubleSpinBox(body);
    opacity_->setObjectName("regionOpacity");
    opacity_->setMaximumWidth(140);
    opacity_->setRange(0, 100);
    opacity_->setDecimals(2);
    opacity_->setSuffix(QStringLiteral(" %"));
    opacity_->setAccessibleName(ui::text("region.display.opacity"));
    appearance->form()->addRow(ui::text("region.display.opacity"), opacity_);
    connect(fill_, &QCheckBox::toggled, opacity_, &QWidget::setEnabled);
    align_form_labels(body);
    rows->addStretch();
    navigation->setCurrentItem(page_items.front());
    auto *footer_widget = new QWidget(this);
    auto *footer_layout = new QVBoxLayout(footer_widget);
    footer_layout->setContentsMargins(theme::space::footer);
    result_ = new QLabel(footer_widget);
    result_->setObjectName("regionResult");
    result_->setWordWrap(true);
    result_->setTextFormat(Qt::PlainText);
    result_->hide();
    footer_layout->addWidget(result_);
    auto *footer = new QHBoxLayout;
    reset_ = new QPushButton(ui::text("region.reset"), this);
    reset_->setObjectName("resetRegion");
    reset_->setAutoDefault(false);
    auto *close = new QPushButton(ui::text("common.close"), this);
    close->setObjectName("closeRegion");
    close->setAutoDefault(false);
    apply_ = new QPushButton(ui::text("common.apply"), this);
    apply_->setObjectName("applyRegion");
    apply_->setProperty("primary", true);
    apply_->setDefault(true);
    footer->addWidget(reset_);
    footer->addStretch();
    footer->addWidget(close);
    footer_layout->addLayout(footer);
    footer_layout->addWidget(apply_);
    layout->addWidget(footer_widget);
    new WidthObserver(footer_widget, [this, footer, footer_layout, mode = WidthMode{},
                                      previous = true](int width) mutable {
        const bool narrow = mode.narrow(width, 380);
        if (narrow == previous)
            return;
        previous = narrow;
        if (narrow) {
            footer->removeWidget(apply_);
            footer_layout->addWidget(apply_);
        } else {
            footer_layout->removeWidget(apply_);
            footer->addWidget(apply_);
        }
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(reset_, &QPushButton::clicked, this, [this] { set_region(original_, creating_); });
    connect(apply_, &QPushButton::clicked, this, [this] {
        try {
            auto candidate = region();
            if (apply)
                apply(std::move(candidate));
        } catch (const std::exception &e) {
            show_result(QString::fromUtf8(e.what()));
        }
    });
}
void RegionEditor::append_vertex(core::RegionVertex vertex) {
    const auto row = vertices_->rowCount();
    vertices_->insertRow(row);
    for (int col = 0; col < 2; ++col) {
        auto *field = new QuantityEditor(Unit::degree, vertices_);
        field->setMaximumWidth(QWIDGETSIZE_MAX);
        field->set_range(col == 0 ? -180 : -90, col == 0 ? 180 : 90, Unit::degree);
        field->set_label(ui::text(col == 0 ? "region.longitude" : "region.latitude"));
        field->set_value(col == 0 ? vertex.longitude_rad : vertex.latitude_rad, Unit::radian);
        auto *input = field->findChild<QLineEdit *>();
        const auto reveal_start = [input] {
            input->setCursorPosition(
                0); // Keep the sign and leading digits visible in narrow cells.
            input->setToolTip(input->text());
        };
        reveal_start();
        connect(input, &QLineEdit::editingFinished, field, reveal_start);
        connect(field, &QuantityEditor::unitChanged, field, reveal_start);
        connect(input, &QLineEdit::textChanged, this, [this] { request_preview(); });
        connect(field, &QuantityEditor::valueChanged, this, [this] { request_preview(); });
        vertices_->setCellWidget(row, col, field);
        new VertexSelectionFilter(field, vertices_, vertices_->model()->index(row, col));
    }
    vertices_->setRowHeight(row, 40);
}
void RegionEditor::set_region(const application::ProjectRegion &value, bool creating) {
    original_ = value;
    creating_ = creating;
    name_->setText(QString::fromStdString(value.name()));
    const auto &b = value.boundary;
    const std::array values{b.south_rad, b.north_rad, b.west_rad, b.east_rad};
    for (std::size_t i = 0; i < values.size(); ++i)
        bounds_[i]->set_value(values[i], Unit::radian);
    vertices_->setRowCount(0);
    if (b.vertices.empty())
        for (auto v : std::array{core::RegionVertex{b.west_rad, b.south_rad},
                                 core::RegionVertex{b.east_rad, b.south_rad},
                                 core::RegionVertex{b.east_rad, b.north_rad},
                                 core::RegionVertex{b.west_rad, b.north_rad}})
            append_vertex(v);
    else
        for (auto v : b.vertices)
            append_vertex(v);
    shape_->setCurrentIndex(b.vertices.empty() ? 0 : 1);
    description_->setPlainText(QString::fromStdString(value.description));
    grid_spacing_->set_value(value.grid_resolution_rad, Unit::radian);
    show_2d_->setChecked(value.display.show_2d);
    show_3d_->setChecked(value.display.show_3d);
    fill_->setChecked(value.display.fill);
    opacity_->setValue(value.display.opacity * 100);
    loaded_opacity_percent_ = opacity_->value();
    opacity_->setEnabled(value.display.fill);
    color_value_ = value.display.color;
    update_color();
    setWindowTitle(ui::text(creating ? "region.create" : "region.properties")
                       .arg(QString::fromStdString(value.name())));
    apply_->setText(ui::text(creating ? "region.create_button" : "common.apply"));
    show_result({});
    update_preview();
}
core::RegionBoundary RegionEditor::boundary() const {
    auto b = original_.boundary;
    b.south_rad = bounds_[0]->value(Unit::radian);
    b.north_rad = bounds_[1]->value(Unit::radian);
    b.west_rad = bounds_[2]->value(Unit::radian);
    b.east_rad = bounds_[3]->value(Unit::radian);
    b.vertices.clear();
    if (shape_->currentIndex() == 1) {
        if (vertices_->rowCount() < 3)
            throw std::invalid_argument(ui::utf8("region.minimum_vertices"));
        for (int row = 0; row < vertices_->rowCount(); ++row)
            b.vertices.push_back(
                {static_cast<QuantityEditor *>(vertices_->cellWidget(row, 0))->value(Unit::radian),
                 static_cast<QuantityEditor *>(vertices_->cellWidget(row, 1))
                     ->value(Unit::radian)});
    }
    core::validate_region_boundary(b);
    return b;
}
application::ProjectRegion RegionEditor::region() const {
    auto result = original_;
    result.display_name = name_->text().trimmed().toStdString();
    if (result.display_name.empty())
        throw std::invalid_argument(ui::utf8("region.name_required"));
    result.boundary = boundary();
    result.description = description_->toPlainText().toStdString();
    result.grid_resolution_rad = grid_spacing_->value(Unit::radian);
    result.display.show_2d = show_2d_->isChecked();
    result.display.show_3d = show_3d_->isChecked();
    result.display.fill = fill_->isChecked();
    result.display.color = color_value_;
    result.display.opacity = opacity_->value() == loaded_opacity_percent_
                                 ? original_.display.opacity
                                 : opacity_->value() / 100;
    auto grid = core::region_grid_bounds(result.boundary);
    grid.resolution_rad = result.grid_resolution_rad;
    core::validate_coverage_grid(grid);
    return result;
}
bool RegionEditor::has_unapplied_changes() const {
    try {
        const auto value = region();
        return value.name() != original_.name() || value.boundary != original_.boundary ||
               value.description != original_.description || value.display != original_.display ||
               value.grid_resolution_rad != original_.grid_resolution_rad;
    } catch (...) {
        return true;
    }
}
void RegionEditor::accept_region(const application::ProjectRegion &value) {
    original_ = value;
    loaded_opacity_percent_ = opacity_->value();
    creating_ = false;
    apply_->setText(ui::text("common.apply"));
    setWindowTitle(ui::text("region.properties").arg(QString::fromStdString(value.name())));
}
void RegionEditor::sync_name(const std::string &name) {
    if (name_->text().trimmed().toStdString() == original_.name())
        name_->setText(QString::fromStdString(name));
    original_.display_name = name;
    setWindowTitle(ui::text(creating_ ? "region.create" : "region.properties")
                       .arg(QString::fromStdString(name)));
}
void RegionEditor::set_busy(bool busy) {
    fields_->setEnabled(!busy);
    apply_->setEnabled(!busy);
    reset_->setEnabled(!busy);
}
void RegionEditor::show_result(const QString &message) {
    result_->setText(message);
    result_->setVisible(!message.isEmpty());
}
void RegionEditor::update_color() {
    const auto color = QColor::fromRgb(color_value_);
    color_->setText(color.name());
    QPixmap swatch(color_->iconSize() * color_->devicePixelRatioF());
    swatch.setDevicePixelRatio(color_->devicePixelRatioF());
    swatch.fill(color);
    color_->setIcon(QIcon(swatch));
    request_preview();
}
void RegionEditor::request_preview() { preview_timer_->start(0); }
void RegionEditor::update_preview() {
    preview_timer_->stop();
    try {
        preview_->set_boundary(boundary(), QColor::fromRgb(color_value_));
    } catch (const std::exception &) {
        preview_->set_boundary(std::nullopt, QColor::fromRgb(color_value_));
    }
}
} // namespace orbelle::desktop
