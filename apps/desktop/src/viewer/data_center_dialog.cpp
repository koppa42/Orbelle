#include "../ui_text.hpp"
#include "eop_catalog.hpp"
#include "form_kit.hpp"
#include "responsive_layout.hpp"
#include "workbench_spin_box.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTimeZone>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace orbelle::desktop {
namespace {
QString product_name(DataProduct p) {
    switch (p) {
    case DataProduct::finals2000a:
        return ui::text("model.finals2000a");
    case DataProduct::c04:
        return ui::text("model.c04");
    case DataProduct::utc:
        return ui::text("data_center_dialog.label");
    case DataProduct::egm2008:
        return ui::text("model.egm2008");
    case DataProduct::egm96:
        return ui::text("model.egm96");
    case DataProduct::custom_gravity:
        return ui::text("data_center_dialog.label_2");
    case DataProduct::fes2014bv1:
        return ui::text("data_center_dialog.label_3");
    case DataProduct::custom_ocean_tides:
        return ui::text("data_center_dialog.label_4");
    case DataProduct::space_weather:
        return ui::text("data_center_dialog.label_5");
    case DataProduct::de440s:
        return ui::text("model.de440s");
    }
    return {};
}

QString installation_text(const DataInventoryItem &item) {
    switch (item.installation) {
    case DataInstallation::missing:
        return item.online_updates ? ui::text("data_center_dialog.label_6")
                                   : ui::text("data_center_dialog.label_7");
    case DataInstallation::bundled_sample:
        return ui::text("data_center_dialog.label_8");
    case DataInstallation::local_file:
        return ui::text("data_center_dialog.label_9");
    case DataInstallation::downloaded:
        return ui::text("data_center_dialog.label_10");
    case DataInstallation::builtin:
        return ui::text("data_center_dialog.label_11");
    }
    return {};
}
QString check_text(DataUpdateState state) {
    switch (state) {
    case DataUpdateState::never_checked:
        return ui::text("data_center_dialog.label_12");
    case DataUpdateState::checking:
        return ui::text("data_center_dialog.label_13");
    case DataUpdateState::downloading:
        return ui::text("data_center_dialog.label_14");
    case DataUpdateState::updated:
        return ui::text("data_center_dialog.label_15");
    case DataUpdateState::unchanged:
        return ui::text("data_center_dialog.label_16");
    case DataUpdateState::failed:
        return ui::text("data_center_dialog.label_17");
    case DataUpdateState::cancelled:
        return ui::text("data_center_dialog.label_18");
    case DataUpdateState::interrupted:
        return ui::text("data_center_dialog.label_19");
    case DataUpdateState::local_changed:
        return ui::text("data_center_dialog.label_20");
    }
    return {};
}
QString local_time(qint64 seconds) {
    return QDateTime::fromSecsSinceEpoch(seconds).toLocalTime().toString("yyyy-MM-dd HH:mm");
}
QString coverage_time(const QDateTime &value) {
    if (!value.isValid())
        return QStringLiteral("—");
    return value.toString(value.time() == QTime(0, 0) ? "yyyy-MM-dd" : "yyyy-MM-dd HH:mm:ss");
}
QString bytes_text(quint64 bytes) {
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KiB").arg(static_cast<double>(bytes) / 1024, 0, 'f', 1);
    return QStringLiteral("%1 MiB").arg(static_cast<double>(bytes) / (1024 * 1024), 0, 'f', 1);
}
class DataTreeWidget final : public QTreeWidget {
  protected:
    void drawBranches(QPainter *painter, const QRect &rect,
                      const QModelIndex &index) const override {
        painter->fillRect(rect, theme::color(theme::panel));
        if (!model()->hasChildren(index))
            return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(
            QPen(theme::color(theme::text_muted), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF center(rect.right() - indentation() / 2.0, rect.center().y());
        const auto points = isExpanded(index)
                                ? QPolygonF{center + QPointF(-3, -2), center + QPointF(0, 1),
                                            center + QPointF(3, -2)}
                                : QPolygonF{center + QPointF(-1, -3), center + QPointF(2, 0),
                                            center + QPointF(-1, 3)};
        painter->drawPolyline(points);
        painter->restore();
    }
};
class DataItemDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        // A desktop list row: transparent at rest, a soft wash on hover, and the selected row
        // marked by an accent wash with a leading stripe.
        const auto bounds = QRectF(option.rect).adjusted(2, 2, -2, -2);
        const bool selected = option.state & QStyle::State_Selected;
        paint_list_row(painter, option, bounds, theme::color(theme::accent));
        const bool child = index.parent().isValid();
        const auto icon = child ? WorkbenchIcon::database
                          : index.data(Qt::UserRole).toInt() == static_cast<int>(DataProduct::utc)
                              ? WorkbenchIcon::clock
                              : WorkbenchIcon::earth;
        const auto top = static_cast<int>(bounds.top());
        workbench_icon(icon, theme::color(selected ? theme::accent_bright
                                          : child  ? theme::text_muted
                                                   : theme::accent))
            .paint(painter, QRect(static_cast<int>(bounds.left()) + 12, top + 11, 16, 16));
        const auto left = static_cast<int>(bounds.left()) + 38;
        const auto width = static_cast<int>(bounds.right()) - left - 8;
        const auto lines = index.data().toString().split('\n');
        auto title_font = option.font;
        title_font.setPixelSize(child ? 13 : 12);
        title_font.setWeight(QFont::Normal);
        painter->setFont(title_font);
        painter->setPen(theme::color(selected ? theme::accent_bright
                                     : child  ? theme::text
                                              : theme::text_soft));
        painter->drawText(QRect(left, top + 6, width, 22), Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(lines.value(0), Qt::ElideRight, width));
        title_font.setPixelSize(11);
        painter->setFont(title_font);
        painter->setPen(theme::color(index.data(Qt::UserRole + 1).toBool() ? theme::warning
                                                                           : theme::text_muted));
        painter->drawText(QRect(left, top + 28, width, 17), Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(lines.value(1), Qt::ElideRight, width));
        painter->restore();
    }
};
class PriorityDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const auto bounds = option.rect.adjusted(1, 4, -1, -4);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        painter->setPen(theme::color(selected ? theme::accent : theme::line));
        painter->setBrush(theme::color(selected ? theme::accent_wash
                                       : hover  ? theme::accent_hover
                                                : theme::panel));
        painter->drawRoundedRect(bounds, 6, 6);
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::color(theme::text_muted));
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 3; ++y)
                painter->drawEllipse(
                    QPointF(bounds.left() + 15 + x * 5, bounds.center().y() - 5 + y * 5), 1.2, 1.2);
        auto font = option.font;
        font.setPixelSize(14);
        font.setWeight(QFont::DemiBold);
        painter->setFont(font);
        painter->setPen(theme::color(theme::accent));
        painter->drawText(QRect(bounds.left() + 33, bounds.top(), 24, bounds.height()),
                          Qt::AlignCenter, QString::number(index.row() + 1));
        const auto lines = index.data().toString().split('\n');
        const auto left = bounds.left() + 72;
        const auto width = bounds.width() - 96;
        painter->setPen(theme::color(theme::text));
        painter->drawText(QRect(left, bounds.top() + 12, width, 22), Qt::AlignVCenter,
                          lines.value(0));
        font.setPixelSize(12);
        font.setWeight(QFont::Normal);
        painter->setFont(font);
        painter->setPen(theme::color(theme::text_muted));
        painter->drawText(QRect(left, bounds.top() + 40, width, 20), Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(lines.value(1), Qt::ElideRight, width));
        painter->restore();
    }
};
} // namespace
void show_data_center(QWidget *parent, EopCatalog &catalog, std::optional<DataProduct> initial) {
    QDialog dialog(parent);
    dialog.setObjectName("eopDataDialog");
    dialog.setWindowTitle(ui::text("common.data_center"));
    dialog.setWindowIcon(workbench_icon(WorkbenchIcon::database));
    dialog.resize(
        QSize(1020, 720).boundedTo(dialog.screen()->availableGeometry().size() - QSize(48, 72)));
    dialog.setSizeGripEnabled(true);
    dialog.setMinimumSize(360, 360);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *header = new QWidget;
    header->setObjectName("dataHeader");
    auto *heading = new QHBoxLayout(header);
    heading->setContentsMargins(18, 10, 18, 10);
    heading->setSpacing(16);
    auto *heading_title = new QLabel(ui::text("common.data_center"));
    heading_title->setObjectName("panelTitle");
    heading->addWidget(heading_title);
    auto *back = new QPushButton(ui::text("data_center_dialog.back"), &dialog);
    back->setObjectName("dataBackToList");
    back->setIcon(workbench_icon(WorkbenchIcon::database));
    back->hide();
    heading->addWidget(back);
    auto *description = new QLabel(ui::text("data_center_dialog.description"));
    description->setObjectName("dialogDescription");
    heading->addWidget(description);
    heading->addStretch();
    layout->addWidget(header);
    auto *panels = new QHBoxLayout;
    panels->setSpacing(0);
    auto *sidebar = new QWidget;
    sidebar->setObjectName("dataSidebar");
    sidebar->setFixedWidth(std::clamp(dialog.width() / 4, 220, 258));
    auto *sidebar_layout = new QVBoxLayout(sidebar);
    sidebar_layout->setContentsMargins(8, 12, 8, 12);
    sidebar_layout->setSpacing(8);
    auto *inventory_caption = new QLabel(ui::text("data_center_dialog.back"));
    inventory_caption->setObjectName("dataSidebarCaption");
    sidebar_layout->addWidget(inventory_caption);
    auto *list = new DataTreeWidget;
    list->setObjectName("dataInventory");
    list->setAccessibleName(ui::text("data_center_dialog.list.set_accessible_name"));
    list->setItemDelegate(new DataItemDelegate(list));
    list->setMouseTracking(true);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setMinimumHeight(100);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setHeaderHidden(true);
    list->setIndentation(16);
    auto *eop_node = new QTreeWidgetItem(list, {ui::text("data_center_dialog.eop_node")});
    eop_node->setSizeHint(0, QSize(0, 52));
    const auto add_source = [&](DataProduct product, QTreeWidgetItem *parent) {
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(list);
        item->setData(0, Qt::UserRole, static_cast<int>(product));
        item->setSizeHint(0, QSize(0, 52));
        return item;
    };
    auto *finals_node = add_source(DataProduct::finals2000a, eop_node);
    auto *c04_node = add_source(DataProduct::c04, eop_node);
    auto *utc_node = add_source(DataProduct::utc, nullptr);
    auto *gravity_node = new QTreeWidgetItem(list, {ui::text("data_center_dialog.gravity_node")});
    gravity_node->setSizeHint(0, QSize(0, 54));
    auto *egm2008_node = add_source(DataProduct::egm2008, gravity_node);
    auto *egm96_node = add_source(DataProduct::egm96, gravity_node);
    auto *custom_node = add_source(DataProduct::custom_gravity, gravity_node);
    gravity_node->setExpanded(true);
    auto *ephemeris_node =
        new QTreeWidgetItem(list, {ui::text("data_center_dialog.ephemeris_node")});
    ephemeris_node->setSizeHint(0, QSize(0, 54));
    auto *de440s_node = add_source(DataProduct::de440s, ephemeris_node);
    auto *weather_node = add_source(DataProduct::space_weather, nullptr);
    auto *ocean_node = new QTreeWidgetItem(list, {ui::text("data_center_dialog.ocean_node")});
    ocean_node->setSizeHint(0, QSize(0, 54));
    auto *fes_node = add_source(DataProduct::fes2014bv1, ocean_node);
    auto *custom_ocean_node = add_source(DataProduct::custom_ocean_tides, ocean_node);
    ocean_node->setExpanded(true);
    ephemeris_node->setExpanded(true);
    eop_node->setExpanded(true);
    list->setCurrentItem(eop_node);
    sidebar_layout->addWidget(list, 1);
    auto *inventory_note = new QLabel(ui::text("data_center_dialog.inventory_note"));
    inventory_note->setObjectName("dataSidebarCaption");
    sidebar_layout->addWidget(inventory_note);
    panels->addWidget(sidebar);
    auto *panel = new QWidget;
    panel->setObjectName("dataDetailPanel");
    panel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto *detail_layout = new QVBoxLayout(panel);
    detail_layout->setContentsMargins(theme::space::page);
    detail_layout->setSpacing(10);
    panels->addWidget(panel, 1);
    layout->addLayout(panels, 1);
    auto *title = new QLabel;
    title->setObjectName("dataName");
    title->setWordWrap(true);
    auto *data_heading = new QHBoxLayout;
    data_heading->addWidget(title, 1);
    auto *state = new QLabel;
    state->setObjectName("dataStatus");
    state->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    data_heading->addWidget(state, 0, Qt::AlignVCenter);
    detail_layout->addLayout(data_heading);
    auto *data_description = new QLabel;
    data_description->setObjectName("dataDescription");
    data_description->setWordWrap(true);
    detail_layout->addWidget(data_description);
    auto *gravity_summary = new QLabel(ui::text("data_center_dialog.gravity_summary"));
    gravity_summary->setWordWrap(true);
    gravity_summary->setObjectName("hint");
    detail_layout->addWidget(gravity_summary);
    auto *priority_scroll = new QScrollArea;
    priority_scroll->setWidgetResizable(true);
    priority_scroll->setFrameShape(QFrame::NoFrame);
    priority_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    priority_scroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto *priority_panel = new QWidget;
    priority_scroll->setWidget(priority_panel);
    auto *priority_layout = new QVBoxLayout(priority_panel);
    priority_layout->setContentsMargins(0, 12, 0, 0);
    priority_layout->setSpacing(12);
    auto *priority_heading = new QHBoxLayout;
    auto *priority_caption = new QLabel(ui::text("data_center_dialog.priority_caption"));
    priority_caption->setObjectName("sectionTitle");
    priority_heading->addWidget(priority_caption);
    priority_heading->addStretch();
    auto *move_up = new QToolButton;
    move_up->setObjectName("eopPriorityUp");
    move_up->setIcon(workbench_icon(WorkbenchIcon::chevron_up));
    move_up->setToolTip(ui::text("data_center_dialog.move_up.set_tool_tip"));
    move_up->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    auto *move_down = new QToolButton;
    move_down->setObjectName("eopPriorityDown");
    move_down->setIcon(workbench_icon(WorkbenchIcon::chevron_down));
    move_down->setToolTip(ui::text("data_center_dialog.move_down.set_tool_tip"));
    move_down->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Down));
    priority_heading->addWidget(move_up);
    priority_heading->addWidget(move_down);
    priority_layout->addLayout(priority_heading);
    auto *priority = new QListWidget;
    priority->setObjectName("eopSourcePriority");
    priority->setAccessibleName(ui::text("data_center_dialog.priority.set_accessible_name"));
    priority->setItemDelegate(new PriorityDelegate(priority));
    priority->setMouseTracking(true);
    priority->setCursor(Qt::OpenHandCursor);
    priority->setDragDropMode(QAbstractItemView::InternalMove);
    priority->setDefaultDropAction(Qt::MoveAction);
    priority->setDragDropOverwriteMode(false);
    priority->setSelectionMode(QAbstractItemView::SingleSelection);
    priority->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    priority->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    priority->setFixedHeight(148);
    for (const auto product : {DataProduct::finals2000a, DataProduct::c04}) {
        auto *item = new QListWidgetItem(priority);
        item->setData(Qt::UserRole, static_cast<int>(product));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        item->setSizeHint(QSize(0, 70));
    }
    priority->setCurrentRow(0);
    priority_layout->addWidget(priority);
    auto *priority_note = new QLabel(ui::text("data_center_dialog.priority_note"));
    priority_note->setObjectName("dataPriorityNote");
    priority_note->setWordWrap(true);
    priority_layout->addWidget(priority_note);
    auto *priority_error = new QLabel;
    priority_error->setObjectName("inputError");
    priority_error->setWordWrap(true);
    priority_error->hide();
    priority_layout->addWidget(priority_error);
    priority_layout->addStretch();
    detail_layout->addWidget(priority_scroll, 1);
    auto *pages = new QTabWidget;
    pages->setObjectName("dataPages");
    pages->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    const auto page = [&](const QString &label) {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *widget = new QWidget;
        auto *page_layout = new QVBoxLayout(widget);
        page_layout->setContentsMargins(0, 12, 0, 0);
        page_layout->setSpacing(16);
        scroll->setWidget(widget);
        pages->addTab(scroll, label);
        return page_layout;
    };
    auto *overview_layout = page(ui::text("data_center_dialog.overview_layout"));
    auto *settings_layout = page(ui::text("data_center_dialog.settings_layout"));
    auto *file_layout = page(ui::text("data_center_dialog.file_layout"));
    detail_layout->addWidget(pages, 1);
    auto *facts = new QFrame;
    facts->setObjectName("dataFacts");
    auto *form = new QFormLayout(facts);
    form->setContentsMargins(0, 4, 0, 8);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(10);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto field = [&](const QString &caption, const char *name) {
        auto *label = new QLabel;
        label->setObjectName(name);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *field_caption = new QLabel(caption);
        field_caption->setProperty("fieldCaption", true);
        form->addRow(field_caption, label);
        return label;
    };
    auto *acquired = field(ui::text("data_center_dialog.acquired"), "dataAcquiredAt");
    auto *first = field(ui::text("data_center_dialog.first"), "dataValidFrom");
    auto *until = field(ui::text("data_center_dialog.until"), "dataValidUntil");
    auto *checked = field(ui::text("data_center_dialog.checked"), "dataLastChecked");
    acquired->setToolTip(ui::text("data_center_dialog.acquired.set_tool_tip"));
    checked->setToolTip(ui::text("data_center_dialog.acquired.set_tool_tip"));
    overview_layout->addWidget(facts);
    responsive_form(form, 300);
    auto *note = new QLabel;
    note->setObjectName("dataNote");
    note->setWordWrap(true);
    auto *acquisition = new QWidget;
    acquisition->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *acquisition_layout = new QVBoxLayout(acquisition);
    acquisition_layout->setContentsMargins(0, 0, 0, 0);
    acquisition_layout->setSpacing(12);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(10);
    auto *download = new QPushButton;
    download->setObjectName("downloadEop");
    download->setProperty("primary", true);
    download->setIcon(workbench_icon(WorkbenchIcon::download, theme::color(theme::on_accent)));
    auto *raw = new QPushButton(ui::text("data_center_dialog.raw"));
    raw->setObjectName("importRawEop");
    raw->setIcon(workbench_icon(WorkbenchIcon::folder));
    auto *manifest = new QPushButton(ui::text("data_center_dialog.manifest"));
    manifest->setObjectName("importEopSnapshot");
    actions->addWidget(download);
    actions->addWidget(raw);
    file_layout->addWidget(manifest, 0, Qt::AlignLeft);
    actions->addStretch();
    acquisition_layout->addLayout(actions);
    auto *planetary_centres = new QPushButton(ui::text("data_center_dialog.planetary_centres"));
    planetary_centres->setObjectName("importPlanetaryCentres");
    planetary_centres->setIcon(workbench_icon(WorkbenchIcon::folder));
    acquisition_layout->addWidget(planetary_centres, 0, Qt::AlignLeft);
    // Update and extrapolation settings: two inspector groups, each opened by a switch.
    settings_layout->setSpacing(0);
    auto *automatic_row = new FormGroup(ui::text("data_center_dialog.automatic_row"));
    automatic_row->setProperty("leading", true);
    auto *automatic = new QCheckBox(ui::text("data_center_dialog.automatic"));
    automatic->setObjectName("dataAutoUpdate");
    automatic->setChecked(catalog.automatic_updates());
    make_switch(automatic);
    auto *frequency = new QComboBox;
    frequency->setObjectName("dataUpdateFrequency");
    frequency->setAccessibleName(ui::text("data_center_dialog.frequency.set_accessible_name"));
    frequency->addItem(ui::text("data_center_dialog.frequency.add_item"), 6);
    frequency->addItem(ui::text("data_center_dialog.frequency.add_item_2"), 24);
    frequency->addItem(ui::text("data_center_dialog.frequency.add_item_3"), 168);
    frequency->setMaximumWidth(276);
    automatic_row->form()->addRow(automatic);
    automatic_row->form()->addRow(ui::text("data_center_dialog.label_21"), frequency);
    settings_layout->addWidget(automatic_row);
    auto *schedule = new QLabel;
    schedule->setObjectName("dataUpdateSchedule");
    schedule->setWordWrap(true);
    automatic_row->form()->addRow(schedule);
    auto *progress = new QProgressBar;
    progress->setObjectName("dataDownloadProgress");
    progress->setTextVisible(false);
    progress->setFixedHeight(5);
    acquisition_layout->addWidget(progress);
    overview_layout->addWidget(acquisition);
    overview_layout->addWidget(note);
    overview_layout->addStretch();
    auto *policy_title = new FormGroup(ui::text("data_center_dialog.policy_title"));
    settings_layout->addWidget(policy_title);
    auto *extrapolation = policy_title->body();
    auto *allow = new QCheckBox(ui::text("data_center_dialog.allow"));
    allow->setObjectName("allowEopExtrapolation");
    make_switch(allow);
    auto *days = new WorkbenchSpinBox<QSpinBox>;
    days->setObjectName("eopExtrapolationDays");
    days->setAccessibleName(ui::text("data_center_dialog.days.set_accessible_name"));
    days->setProperty("stepButtons", true);
    days->setMinimumWidth(90);
    days->setRange(1, core::max_eop_extrapolation_days);
    days->setValue(3);
    days->setSuffix(ui::text("data_center_dialog.days.set_suffix"));
    days->setMaximumWidth(160);
    policy_title->form()->addRow(allow);
    auto *limit_caption = new QLabel(ui::text("data_center_dialog.limit_caption"));
    policy_title->form()->addRow(limit_caption, days);
    auto *extrapolation_note =
        policy_title->add_caution(ui::text("data_center_dialog.policy_title.add_caution"));
    align_form_labels(policy_title->parentWidget());
    settings_layout->addStretch();
    auto *technical = new QLabel;
    technical->setObjectName("dataInventoryDetails");
    technical->setTextFormat(Qt::PlainText);
    technical->setWordWrap(true);
    technical->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    technical->setTextInteractionFlags(Qt::TextSelectableByMouse);
    file_layout->addWidget(technical);
    file_layout->addStretch();
    auto *warnings = new QLabel(catalog.warnings().join('\n'), &dialog);
    warnings->setObjectName("inputError");
    warnings->setContentsMargins(24, 8, 24, 8);
    warnings->setTextFormat(Qt::PlainText);
    warnings->setWordWrap(true);
    warnings->setVisible(!warnings->text().isEmpty());
    layout->addWidget(warnings);
    auto *status = new QLabel;
    status->setObjectName("dataCenterStatus");
    status->setTextFormat(Qt::PlainText);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status->setWordWrap(true);
    acquisition_layout->addWidget(status);
    auto *copy_diagnostics = new QPushButton(ui::text("data_center_dialog.copy_diagnostics"));
    copy_diagnostics->setObjectName("copyDownloadDiagnostics");
    acquisition_layout->addWidget(copy_diagnostics, 0, Qt::AlignLeft);
    auto *footer_panel = new QWidget;
    footer_panel->setObjectName("dataFooter");
    auto *footer = new QHBoxLayout(footer_panel);
    footer->setContentsMargins(theme::space::footer);
    auto *usage = new QLabel(ui::text("data_center_dialog.usage"));
    usage->setWordWrap(true);
    usage->setObjectName("hint");
    footer->addWidget(usage, 1);
    auto *cancel = new QPushButton(ui::text("data_center_dialog.cancel"));
    cancel->setObjectName("cancelEopDownload");
    actions->insertWidget(3, cancel);
    for (auto *button : {download, raw, cancel}) {
        button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        actions->setAlignment(button, Qt::AlignLeft | Qt::AlignVCenter);
    }
    auto *close = new QPushButton(ui::text("common.close"));
    close->setMinimumWidth(88);
    footer->addWidget(close);
    layout->addWidget(footer_panel);
    responsive_columns(acquisition, actions, 380);
    bool narrow = false, showing_details = false;
    const auto show_panels = [&] {
        sidebar->setVisible(!narrow || !showing_details);
        panel->setVisible(!narrow || showing_details);
        back->setVisible(narrow && showing_details);
        description->setVisible(!narrow);
    };
    new WidthObserver(&dialog, [&, mode = WidthMode{}](int width) mutable {
        narrow = mode.narrow(width, 760);
        sidebar->setMinimumWidth(narrow ? 0 : std::clamp(width / 4, 220, 258));
        sidebar->setMaximumWidth(narrow ? QWIDGETSIZE_MAX : sidebar->minimumWidth());
        detail_layout->setContentsMargins(narrow ? 18 : 24, 16, narrow ? 18 : 24, 18);
        show_panels();
    });
    QObject::connect(back, &QPushButton::clicked, &dialog, [&] {
        showing_details = false;
        show_panels();
        list->setFocus();
    });
    const auto open_details = [&] {
        showing_details = true;
        show_panels();
        if (narrow)
            back->setFocus();
    };
    QObject::connect(list, &QTreeWidget::itemClicked, &dialog, open_details);
    QObject::connect(list, &QTreeWidget::itemActivated, &dialog, open_details);
    auto entries = catalog.inventory();
    QString operation_error;
    bool syncing_priority = false;
    const auto selected = [&]() -> std::optional<DataProduct> {
        return list->currentItem() == eop_node || list->currentItem() == gravity_node ||
                       list->currentItem() == ephemeris_node || list->currentItem() == ocean_node
                   ? std::nullopt
                   : std::optional{static_cast<DataProduct>(
                         list->currentItem()->data(0, Qt::UserRole).toInt())};
    };
    const auto inventory_item = [&](DataProduct product) -> const DataInventoryItem & {
        return *std::find_if(entries.begin(), entries.end(),
                             [&](const auto &entry) { return entry.product == product; });
    };
    const auto selected_item = [&]() -> const DataInventoryItem & {
        return inventory_item(*selected());
    };
    const auto update_details = [&] {
        const bool group = !selected();
        gravity_summary->setVisible(list->currentItem() == gravity_node ||
                                    list->currentItem() == ephemeris_node ||
                                    list->currentItem() == ocean_node);
        priority_scroll->setVisible(group);
        pages->setVisible(!group);
        state->setVisible(!group);
        facts->setVisible(!group);
        note->setVisible(!group);
        technical->setVisible(!group);
        if (group) {
            if (list->currentItem() == ephemeris_node) {
                priority_scroll->hide();
                title->setText(ui::text("data_center_dialog.title.set_text"));
                data_description->setText(ui::text("data_center_dialog.data_description.set_text"));
                gravity_summary->setText(ui::text("data_center_dialog.gravity_summary.set_text"));
                return;
            }
            if (list->currentItem() == ocean_node) {
                priority_scroll->hide();
                title->setText(ui::text("data_center_dialog.title.set_text_2"));
                data_description->setText(
                    ui::text("data_center_dialog.data_description.set_text_2"));
                gravity_summary->setText(ui::text("data_center_dialog.gravity_summary.set_text_2"));
                return;
            }
            if (list->currentItem() == gravity_node) {
                gravity_summary->setText(ui::text("data_center_dialog.gravity_summary.set_text_3"));
                priority_scroll->hide();
                title->setText(ui::text("data_center_dialog.title.set_text_3"));
                data_description->setText(
                    ui::text("data_center_dialog.data_description.set_text_3"));
                return;
            }
            title->setText(ui::text("data_center_dialog.title.set_text_4"));
            data_description->setText(ui::text("data_center_dialog.data_description.set_text_4"));
            acquisition->hide();
            policy_title->hide();
            extrapolation->hide();
            return;
        }
        const auto &item = selected_item();
        const bool is_time = item.product == DataProduct::utc;
        const bool is_gravity = gravity_product(item.product);
        const bool is_ocean = ocean_product(item.product);
        const bool is_ephemeris = item.product == DataProduct::de440s;
        acquisition->show();
        policy_title->setVisible(item.extrapolation);
        extrapolation->setVisible(item.extrapolation);
        extrapolation_note->setVisible(item.extrapolation);
        download->setVisible(item.online_updates);
        raw->setVisible(item.raw_import);
        manifest->setVisible(item.manifest_import || is_ephemeris);
        manifest->setText(is_ephemeris ? ui::text("data_center_dialog.manifest.set_text")
                                       : ui::text("data_center_dialog.manifest.set_text_2"));
        planetary_centres->setVisible(is_ephemeris);
        automatic_row->setVisible(item.online_updates);
        schedule->setVisible(item.online_updates);
        const QSignalBlocker auto_block(automatic), frequency_block(frequency), allow_block(allow),
            days_block(days);
        automatic->setChecked(catalog.automatic_updates(item.product));
        schedule->setText(automatic->isChecked() ? ui::text("data_center_dialog.schedule.set_text")
                                                 : ui::text("data_center_dialog.edule.set_text"));
        const auto hours = catalog.update_interval_hours(item.product);
        if (frequency->findData(hours) < 0)
            frequency->addItem(ui::text("data_center_dialog.frequency.add_item_4").arg(hours),
                               hours);
        frequency->setCurrentIndex(frequency->findData(hours));
        frequency->setEnabled(automatic->isChecked());
        if (item.provider_index) {
            const auto limit = catalog.extrapolation_days(*item.provider_index);
            allow->setChecked(limit > 0);
            days->setValue(limit > 0 ? limit : 3);
            days->setEnabled(limit > 0);
            limit_caption->setEnabled(limit > 0);
        }
        title->setText(product_name(item.product));
        data_description->setText(item.product == DataProduct::space_weather
                                      ? ui::text("data_center_dialog.data_description.set_text_5")
                                  : is_ocean     ? ui::text("data_center_dialog.label_22")
                                  : is_gravity   ? ui::text("data_center_dialog.label_23")
                                  : is_ephemeris ? ui::text("data_center_dialog.label_24")
                                  : is_time      ? ui::text("data_center_dialog.label_25")
                                  : item.product == DataProduct::finals2000a
                                      ? ui::text("data_center_dialog.label_26")
                                      : ui::text("data_center_dialog.label_27"));
        state->setText(installation_text(item));
        state->setProperty("tone", item.installation == DataInstallation::bundled_sample ||
                                           item.installation == DataInstallation::missing
                                       ? "warning"
                                       : "ready");
        state->style()->unpolish(state);
        state->style()->polish(state);
        acquired->setText(item.acquired_at ? local_time(*item.acquired_at)
                          : item.acquisition == runtime::DataAcquisition::bundled
                              ? ui::text("data_center_dialog.label_28")
                          : item.installation == DataInstallation::missing
                              ? ui::text("data_center_dialog.label_29")
                              : ui::text("data_center_dialog.label_30"));
        qobject_cast<QLabel *>(form->labelForField(first))
            ->setText(is_gravity || is_ocean ? ui::text("data_center_dialog.label_31")
                      : is_ephemeris         ? ui::text("data_center_dialog.label_32")
                                             : ui::text("data_center_dialog.first"));
        first->setText(is_ocean     ? (item.ocean_tides ? QStringLiteral("%1 × %2")
                                                          .arg(item.ocean_tides->degree)
                                                          .arg(item.ocean_tides->order)
                                                        : ui::text("data_center_dialog.label_33"))
                       : is_gravity ? (item.gravity ? QStringLiteral("%1 × %2")
                                                          .arg(item.gravity->degree)
                                                          .arg(item.gravity->order)
                                                    : ui::text("data_center_dialog.label_33"))
                                    : coverage_time(item.first));
        until->setText(
            is_gravity || is_ocean
                ? ui::text("data_center_dialog.until.set_text")
                : coverage_time(item.last) +
                      (item.exclusive_end ? ui::text("data_center_dialog.label_34") : QString{}));
        qobject_cast<QLabel *>(form->labelForField(until))
            ->setText(is_gravity || is_ocean ? ui::text("data_center_dialog.label_35")
                      : is_ephemeris         ? ui::text("data_center_dialog.label_36")
                      : is_time              ? ui::text("data_center_dialog.label_37")
                                             : ui::text("data_center_dialog.until"));
        if (is_gravity)
            note->setText(
                item.gravity
                    ? ui::text("data_center_dialog.note.set_text")
                              .arg(QString::fromStdString(item.gravity->model),
                                   QString::fromStdString(item.gravity->tide_system))
                              .arg(item.gravity->mu, 0, 'g', 15)
                              .arg(item.gravity->radius_m, 0, 'g', 15)
                              .arg(item.gravity->solid_tide_k20, 0, 'g', 15) +
                          (item.gravity->secular_variations
                               ? ui::text("data_center_dialog.label_38")
                                     .arg(2451545.0 +
                                              (item.gravity->secular_variations->reference_epoch
                                                   .seconds_since(core::Epoch{0}) +
                                               32.184) /
                                                  86400,
                                          0, 'f', 9)
                                     .arg(item.gravity->secular_variations->rates_per_year.size())
                               : ui::text("data_center_dialog.label_39"))
                : item.product == DataProduct::egm2008 ? ui::text("data_center_dialog.label_40")
                                                       : ui::text("data_center_dialog.label_41"));
        else if (is_ocean)
            note->setText(item.ocean_tides
                              ? ui::text("data_center_dialog.note.set_text_2")
                                    .arg(QString::fromStdString(item.ocean_tides->model))
                                    .arg(item.ocean_tides->constituents)
                              : ui::text("data_center_dialog.label_42"));
        else if (is_ephemeris)
            note->setText(ui::text("data_center_dialog.note.set_text_3"));
        else if (is_time)
            note->setText(QDateTime::currentDateTimeUtc() >= item.last
                              ? ui::text("data_center_dialog.note.set_text_4")
                              : ui::text("data_center_dialog.label_43"));
        else if (item.installation == DataInstallation::bundled_sample)
            note->setText(item.online_updates ? ui::text("data_center_dialog.note.set_text_5")
                                              : ui::text("data_center_dialog.note.set_text_6"));
        else if (item.installation == DataInstallation::missing)
            note->setText(item.online_updates ? ui::text("data_center_dialog.note.set_text_7")
                                              : ui::text("data_center_dialog.note.set_text_8"));
        else
            note->setText(ui::text("data_center_dialog.note.set_text_9"));
        const auto location = is_time && item.file.empty() ? ui::text("data_center_dialog.location")
                              : item.file.empty() ? ui::text("data_center_dialog.location_2")
                                                  : QString::fromStdWString(item.file.wstring());
        technical->setText(
            ui::text("data_center_dialog.technical.set_text")
                .arg(item.source_url.isEmpty() ? ui::text("data_center_dialog.technical.set_text_2")
                                               : item.source_url,
                     location, item.file_size ? bytes_text(*item.file_size) : QStringLiteral("—"),
                     item.sha256.isEmpty() ? QStringLiteral("—") : item.sha256,
                     item.identity_sha256.isEmpty()
                         ? QString{}
                         : ui::text("data_center_dialog.label_44") + item.identity_sha256));
    };
    const auto show_error = [&](const std::exception &error) {
        operation_error = QString::fromUtf8(error.what());
    };
    const auto select_provider = [&](int index) {
        for (const auto &entry : entries)
            if (entry.provider_index == static_cast<std::size_t>(index)) {
                eop_node->setExpanded(true);
                list->setCurrentItem(entry.product == DataProduct::finals2000a ? finals_node
                                                                               : c04_node);
                return;
            }
    };
    const auto rebuild = [&] {
        entries = catalog.inventory();
        update_details();
    };
    const auto selection_changed = [&] {
        operation_error.clear();
        pages->setCurrentIndex(0);
        qobject_cast<QScrollArea *>(pages->widget(0))->verticalScrollBar()->setValue(0);
        update_details();
    };
    QObject::connect(list, &QTreeWidget::currentItemChanged, &dialog, selection_changed);
    QObject::connect(priority->model(), &QAbstractItemModel::rowsMoved, &dialog, [&] {
        move_up->setEnabled(priority->currentRow() > 0);
        move_down->setEnabled(priority->currentRow() >= 0 &&
                              priority->currentRow() < priority->count() - 1);
        if (syncing_priority)
            return;
        operation_error.clear();
        try {
            catalog.set_preferred_product(
                static_cast<DataProduct>(priority->item(0)->data(Qt::UserRole).toInt()));
        } catch (const std::exception &error) {
            show_error(error);
        }
    });
    const auto move_priority = [&](int offset) {
        const auto row = priority->currentRow();
        const auto target = row + offset;
        if (row >= 0 && target >= 0 && target < priority->count())
            priority->model()->moveRow({}, row, {}, target > row ? target + 1 : target);
    };
    QObject::connect(move_up, &QToolButton::clicked, &dialog, [&] { move_priority(-1); });
    QObject::connect(move_down, &QToolButton::clicked, &dialog, [&] { move_priority(1); });
    QObject::connect(download, &QPushButton::clicked, &dialog, [&] {
        operation_error.clear();
        catalog.start_download({}, *selected());
    });
    QObject::connect(manifest, &QPushButton::clicked, &dialog, [&] {
        const auto file = QFileDialog::getOpenFileName(
            &dialog,
            selected() == DataProduct::de440s ? ui::text("data_center_dialog.label_45")
                                              : ui::text("data_center_dialog.label_46"),
            {}, ui::text("data_center_dialog.label_47"));
        if (file.isEmpty())
            return;
        operation_error.clear();
        try {
            if (selected() == DataProduct::de440s) {
                catalog.import_ephemeris_snapshot(std::filesystem::path(file.toStdWString()));
                rebuild();
            } else {
                const auto index =
                    catalog.import_snapshot(std::filesystem::path(file.toStdWString()));
                rebuild();
                select_provider(static_cast<int>(index));
            }
        } catch (const std::exception &error) {
            show_error(error);
        }
    });
    QObject::connect(planetary_centres, &QPushButton::clicked, &dialog, [&] {
        const auto file = QFileDialog::getOpenFileName(&dialog, ui::text("data_center_dialog.file"),
                                                       {}, ui::text("data_center_dialog.file_2"));
        if (file.isEmpty())
            return;
        operation_error.clear();
        try {
            catalog.import_planetary_centres(std::filesystem::path(file.toStdWString()));
            rebuild();
        } catch (const std::exception &error) {
            show_error(error);
        }
    });
    QObject::connect(raw, &QPushButton::clicked, &dialog, [&] {
        const auto file = QFileDialog::getOpenFileName(
            &dialog, ui::text("data_center_dialog.file_3").arg(title->text()), {},
            ocean_product(*selected())                  ? ui::text("data_center_dialog.label_48")
            : *selected() == DataProduct::space_weather ? ui::text("data_center_dialog.label_49")
            : *selected() == DataProduct::de440s        ? ui::text("data_center_dialog.label_50")
            : gravity_product(*selected())              ? ui::text("data_center_dialog.label_51")
                                                        : ui::text("data_center_dialog.label_52"));
        if (file.isEmpty())
            return;
        operation_error.clear();
        try {
            catalog.import_raw(std::filesystem::path(file.toStdWString()), *selected());
            rebuild();
            if (catalog.last_installed())
                select_provider(static_cast<int>(*catalog.last_installed()));
        } catch (const std::exception &error) {
            show_error(error);
        }
    });
    QObject::connect(automatic, &QCheckBox::toggled, &dialog, [&](bool value) {
        try {
            catalog.set_automatic_updates(value, *selected());
        } catch (const std::exception &error) {
            show_error(error);
        }
        update_details();
    });
    QObject::connect(frequency, &QComboBox::currentIndexChanged, &dialog, [&] {
        try {
            catalog.set_update_interval_hours(frequency->currentData().toInt(), *selected());
        } catch (const std::exception &error) {
            show_error(error);
        }
        update_details();
    });
    const auto save_extrapolation = [&] {
        if (!selected() || !selected_item().provider_index)
            return;
        try {
            catalog.set_extrapolation_days(*selected_item().provider_index,
                                           allow->isChecked() ? days->value() : 0);
        } catch (const std::exception &error) {
            show_error(error);
        }
        update_details();
    };
    QObject::connect(allow, &QCheckBox::toggled, &dialog, save_extrapolation);
    QObject::connect(days, &QSpinBox::valueChanged, &dialog, save_extrapolation);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, [&] { catalog.cancel_download(); });
    QObject::connect(copy_diagnostics, &QPushButton::clicked, &dialog, [&] {
        if (const auto product = selected())
            QGuiApplication::clipboard()->setText(catalog.update_status(*product).diagnostics);
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    QTimer timer;
    std::uint64_t seen_revision = 0;
    QDate seen_day;
    const auto refresh = [&] {
        const auto today = QDateTime::currentDateTimeUtc().date();
        if (seen_revision != catalog.revision() || seen_day != today) {
            rebuild();
            seen_revision = catalog.revision();
            seen_day = today;
        }
        const bool busy = catalog.downloading();
        for (auto *node : {finals_node, c04_node, utc_node, egm2008_node, egm96_node, custom_node,
                           de440s_node, weather_node, fes_node, custom_ocean_node}) {
            const auto &entry =
                inventory_item(static_cast<DataProduct>(node->data(0, Qt::UserRole).toInt()));
            const auto name = product_name(entry.product);
            const auto update = catalog.update_status(entry.product);
            const bool failed = update.state == DataUpdateState::failed ||
                                update.state == DataUpdateState::interrupted;
            const auto installation = (busy && catalog.active_product() == entry.product) || failed
                                          ? check_text(update.state)
                                          : installation_text(entry);
            node->setText(
                0, name + "\n" + installation + "\n" +
                       (ocean_product(entry.product) ? ui::text("data_center_dialog.node.set_text")
                        : gravity_product(entry.product) ? ui::text("data_center_dialog.label_53")
                        : entry.last.isValid()           ? (entry.product == DataProduct::utc
                                                                ? ui::text("data_center_dialog.label_54")
                                                                : ui::text("data_center_dialog.label_55")) +
                                                     entry.last.date().toString(Qt::ISODate)
                                               : ui::text("data_center_dialog.label_56")));
            node->setData(0, Qt::UserRole + 1, entry.online_updates && failed);
            node->setToolTip(0, node->text(0));
        }
        if (priority->item(0)->data(Qt::UserRole).toInt() !=
            static_cast<int>(catalog.preferred_product())) {
            syncing_priority = true;
            priority->model()->moveRow({}, 1, {}, 0);
            syncing_priority = false;
        }
        for (int row = 0; row < priority->count(); ++row) {
            auto *entry_item = priority->item(row);
            const auto &entry =
                inventory_item(static_cast<DataProduct>(entry_item->data(Qt::UserRole).toInt()));
            const auto name = entry.product == DataProduct::finals2000a
                                  ? ui::text("model.finals2000a")
                                  : ui::text("model.c04");
            entry_item->setText(name + "\n" + installation_text(entry) +
                                (entry.first.isValid() ? QStringLiteral(" · %1 — %2 UTC")
                                                             .arg(coverage_time(entry.first),
                                                                  coverage_time(entry.last))
                                                       : ui::text("data_center_dialog.label_57")));
        }
        move_up->setEnabled(priority->currentRow() > 0);
        move_down->setEnabled(priority->currentRow() >= 0 &&
                              priority->currentRow() < priority->count() - 1);
        priority_error->setText(operation_error);
        priority_error->setVisible(!operation_error.isEmpty());
        if (!selected())
            return;
        const auto &item = selected_item();
        const auto update = catalog.update_status(item.product);
        copy_diagnostics->setVisible(!update.diagnostics.isEmpty());
        const bool downloading_this = busy && catalog.active_product() == item.product;
        checked->setText(
            item.online_updates
                ? (update.attempted_at ? local_time(*update.attempted_at) + "\n" : QString{}) +
                      check_text(update.state)
            : item.product == DataProduct::utc ? ui::text("data_center_dialog.label_58")
                                               : ui::text("data_center_dialog.label_59"));
        download->setText(downloading_this ? check_text(update.state)
                          : update.state == DataUpdateState::failed ||
                                  update.state == DataUpdateState::interrupted
                              ? ui::text("data_center_dialog.label_60")
                          : item.installation == DataInstallation::missing ||
                                  item.installation == DataInstallation::bundled_sample
                              ? ui::text("data_center_dialog.label_61")
                              : ui::text("data_center_dialog.label_62"));
        download->setEnabled(!busy);
        raw->setEnabled(!busy);
        planetary_centres->setEnabled(!busy);
        manifest->setEnabled(!busy);
        cancel->setVisible(downloading_this);
        progress->setVisible(downloading_this);
        if (busy && catalog.total_bytes() > 0) {
            progress->setRange(0, 1000);
            progress->setValue(static_cast<int>(
                std::min<qint64>(1000, catalog.received_bytes() * 1000 / catalog.total_bytes())));
        } else {
            progress->setRange(0, 0);
        }
        schedule->setText(automatic->isChecked() ? ui::text("data_center_dialog.schedule.set_text")
                                                 : ui::text("data_center_dialog.edule.set_text"));
        QString message = operation_error;
        if (message.isEmpty() &&
            (item.online_updates || gravity_product(item.product) || ocean_product(item.product))) {
            if (downloading_this) {
                message = update.message;
                if (catalog.received_bytes() > 0)
                    message +=
                        "  " + bytes_text(static_cast<quint64>(catalog.received_bytes())) +
                        (catalog.total_bytes() > 0
                             ? " / " + bytes_text(static_cast<quint64>(catalog.total_bytes()))
                             : QString{});
            } else if (update.state == DataUpdateState::failed ||
                       update.state == DataUpdateState::interrupted ||
                       update.state == DataUpdateState::cancelled ||
                       update.state == DataUpdateState::local_changed) {
                message = update.message;
                if (update.checked_at)
                    message +=
                        ui::text("data_center_dialog.label_63") + local_time(*update.checked_at);
            }
        }
        status->setText(message);
        const bool warning =
            !operation_error.isEmpty() ||
            (item.online_updates && (update.state == DataUpdateState::failed ||
                                     update.state == DataUpdateState::interrupted));
        if (status->property("warning").toBool() != warning) {
            status->setProperty("warning", warning);
            status->style()->unpolish(status);
            status->style()->polish(status);
        }
        status->setVisible(!status->text().isEmpty());
    };
    QObject::connect(list, &QTreeWidget::currentItemChanged, &dialog, refresh);
    QObject::connect(priority, &QListWidget::currentRowChanged, &dialog, [&] {
        move_up->setEnabled(priority->currentRow() > 0);
        move_down->setEnabled(priority->currentRow() >= 0 &&
                              priority->currentRow() < priority->count() - 1);
    });
    QObject::connect(download, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(raw, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(manifest, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(&timer, &QTimer::timeout, &dialog, refresh);
    timer.start(150);
    refresh();
    if (initial) {
        for (auto *node : {finals_node, c04_node, utc_node, egm2008_node, egm96_node, custom_node,
                           de440s_node, weather_node, fes_node, custom_ocean_node})
            if (node->data(0, Qt::UserRole).toInt() == static_cast<int>(*initial)) {
                list->setCurrentItem(node);
                list->itemActivated(node, 0);
                break;
            }
    }
    dialog.exec();
}
} // namespace orbelle::desktop
