#include "hpop_settings.hpp"
#include "../ui_text.hpp"
#include "eop_catalog.hpp"
#include "form_kit.hpp"
#include "hpop_covariance_editor.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <orbitforge/runtime/ephemeris.hpp>

namespace orbitforge::desktop {
namespace {
constexpr int summary_role = Qt::UserRole + 1;
constexpr int heading_role = Qt::UserRole + 2;
class NavigationDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        return {160, index.data(heading_role).toBool() ? option.fontMetrics.height() + 14
                                                       : option.fontMetrics.height() * 2 + 18};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        const QRect row = option.rect.adjusted(2, 1, -2, -1);
        auto font = option.font;
        painter->save();
        if (index.data(heading_role).toBool()) {
            font.setPixelSize(std::max(10, font.pixelSize() - 1));
            font.setLetterSpacing(QFont::AbsoluteSpacing, 1);
            painter->setFont(font);
            painter->setPen(theme::color(theme::text_muted));
            painter->drawText(row.adjusted(10, 6, -6, 0), Qt::AlignLeft | Qt::AlignVCenter,
                              index.data().toString());
            painter->restore();
            return;
        }
        const bool selected = option.state & QStyle::State_Selected;
        paint_list_row(painter, option, QRectF(row), theme::color(theme::accent));
        const QFontMetrics metrics(font);
        painter->setFont(font);
        painter->setPen(theme::color(selected ? theme::accent_bright : theme::text));
        painter->drawText(QRect(row.left() + 12, row.top() + 7, row.width() - 20, metrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, index.data().toString());
        font.setPixelSize(std::max(10, font.pixelSize() - 1));
        painter->setFont(font);
        const QFontMetrics small(font);
        const auto summary = index.data(summary_role).toString();
        painter->setPen(
            theme::color(summary.startsWith(ui::text("hpop_settings.painter.set_pen")) ||
                                 summary.startsWith(ui::text("hpop_settings.painter.set_pen_2"))
                             ? theme::text_faint
                             : theme::text_muted));
        painter->drawText(QRect(row.left() + 12, row.top() + 9 + metrics.height(), row.width() - 20,
                                small.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          small.elidedText(summary, Qt::ElideRight, row.width() - 20));
        painter->restore();
    }
};
} // namespace
bool edit_hpop_settings(
    core::HpopOptions &options, QWidget *parent, const std::vector<runtime::CatalogGravity> &models,
    const std::vector<runtime::CatalogEphemeris> &ephemerides,
    const std::function<std::vector<runtime::CatalogEphemeris>()> &manage_ephemerides,
    EopCatalog *catalog, application::HpopCovarianceSettings *covariance) {
    QPointer<EopCatalog> data = catalog;
    auto installed_models = data ? data->data().gravity_models() : models;
    if (data)
        std::erase_if(installed_models, [](const auto &entry) {
            std::error_code error;
            return !std::filesystem::is_regular_file(entry.file, error);
        });
    auto installed_ephemerides = data ? data->data().ephemerides() : ephemerides;
    QDialog dialog(parent);
    dialog.setObjectName("hpopSettingsDialog");
    dialog.setWindowTitle(ui::text("hpop_settings.label"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(16);
    auto *heading = new QLabel(ui::text("hpop_settings.heading"));
    heading->setObjectName("panelTitle");
    layout->addWidget(heading);
    auto *subtitle = new QLabel(ui::text("hpop_settings.subtitle"));
    subtitle->setObjectName("hint");
    layout->addWidget(subtitle);
    // A navigation list with live summaries replaces the two levels of tabs; the tab widgets
    // keep the pages (and their indices) and show their bars only in narrow windows.
    auto *columns = new QWidget;
    auto *body_layout = new QHBoxLayout(columns);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(16);
    auto *navigation = new QListWidget;
    navigation->setObjectName("hpopNavigation");
    navigation->setAccessibleName(ui::text("hpop_settings.navigation.set_accessible_name"));
    navigation->setItemDelegate(new NavigationDelegate(navigation));
    navigation->setFixedWidth(196);
    navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body_layout->addWidget(navigation);
    auto *tabs = new QTabWidget;
    tabs->setObjectName("hpopSettingsTabs");
    body_layout->addWidget(tabs, 1);
    layout->addWidget(columns, 1);
    auto *force_page = new QWidget;
    force_page->setObjectName("hpopForceModelsPage");
    auto *force_layout = new QVBoxLayout(force_page);
    force_layout->setContentsMargins(0, 0, 0, 0);
    auto *force_tabs = new QTabWidget;
    force_tabs->setObjectName("hpopForceModelTabs");
    force_layout->addWidget(force_tabs);
    tabs->addTab(force_page, ui::text("hpop_settings.tabs.add_tab"));
    const auto page = [&](QTabWidget *owner, const QString &title, const char *name) {
        auto *scroll = new QScrollArea;
        scroll->setObjectName(name);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *content = new QWidget;
        auto *form = new QFormLayout(content);
        form->setContentsMargins(0, 4, 12, 8);
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(10);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        scroll->setWidget(content);
        owner->addTab(scroll, title);
        return form;
    };
    const auto note = [](QFormLayout *form, const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("hint");
        label->setWordWrap(true);
        form->addRow(label);
    };
    const auto section = [](QFormLayout *parent_form, const QString &title, const char *name) {
        auto *group = new FormGroup(title, name);
        parent_form->addRow(group);
        return group->form();
    };
    const auto group_of = [](QFormLayout *form) -> FormGroup * {
        return form && form->parentWidget()
                   ? dynamic_cast<FormGroup *>(form->parentWidget()->parentWidget())
                   : nullptr;
    };
    // Explanations sit behind the group's info marker; limitations stay on the page.
    const auto help = [&](QFormLayout *form, const QString &text) {
        if (auto *group = group_of(form))
            group->add_help(text);
        else
            note(form, text);
    };
    const auto caution = [&](QFormLayout *form, const QString &text) {
        if (auto *group = group_of(form))
            group->add_caution(text);
        else
            form->addRow(caution_label(text));
    };
    auto *gravity_page =
        page(force_tabs, ui::text("hpop_settings.gravity_page"), "hpopGravityPage");
    gravity_page->setVerticalSpacing(12);
    gravity_page->setContentsMargins(0, 4, 12, 8);
    auto *gravity =
        section(gravity_page, ui::text("hpop_settings.gravity"), "hpopCentralGravitySection");
    auto *model = new QComboBox;
    model->setObjectName("hpopGravityModel");
    model->addItems({ui::text("hpop_settings.model"), ui::text("hpop_settings.label_2"),
                     ui::text("model.egm2008"), ui::text("model.egm96")});
    model->setItemData(0, QString{});
    model->setItemData(1, QString{});
    model->setItemData(2, QStringLiteral("egm2008"));
    model->setItemData(3, QStringLiteral("egm96"));
    const auto refresh_models = [&] {
        for (const auto &entry : installed_models) {
            const auto id = QString::fromStdString(entry.selection);
            const auto title = QString::fromStdString(entry.info.model) +
                               (entry.selection == "egm2008" || entry.selection == "egm96"
                                    ? QString{}
                                    : ui::text("hpop_settings.label_3"));
            const auto index = model->findData(id);
            if (index < 0)
                model->addItem(title, id);
            else
                model->setItemText(index, title);
        }
    };
    refresh_models();
    model->setCurrentIndex(options.gravity_degree == 0 ? 1 : 0);
    if (!options.gravity_model.empty()) {
        const auto id = QString::fromStdString(options.gravity_model);
        if (model->findData(id) < 0)
            model->addItem(id, id);
        model->setCurrentIndex(model->findData(id));
    }
    gravity->addRow(ui::text("hpop_settings.gravity.add_row"), model);
    auto *degree = new QSpinBox;
    degree->setObjectName("hpopGravityDegree");
    degree->setRange(2, core::gravity_max_degree);
    degree->setValue(std::max(2, options.gravity_degree));
    auto *order = new QSpinBox;
    order->setObjectName("hpopGravityOrder");
    order->setRange(0, degree->value());
    order->setValue(options.gravity_order);
    // Truncation is read as one quantity ("8 × 8"), so degree and order share a row.
    degree->setAccessibleName(ui::text("hpop_settings.degree.set_accessible_name"));
    order->setAccessibleName(ui::text("hpop_settings.order.set_accessible_name"));
    degree->setToolTip(ui::text("hpop_settings.degree.set_accessible_name"));
    order->setToolTip(ui::text("hpop_settings.order.set_accessible_name"));
    auto *truncation = new QWidget;
    auto *truncation_layout = new QHBoxLayout(truncation);
    truncation_layout->setContentsMargins(0, 0, 0, 0);
    truncation_layout->setSpacing(8);
    truncation_layout->addWidget(degree, 1);
    auto *times = new QLabel(QStringLiteral("×"));
    times->setObjectName("muted");
    truncation_layout->addWidget(times);
    truncation_layout->addWidget(order, 1);
    truncation->setMaximumWidth(276);
    gravity->addRow(ui::text("hpop_settings.gravity.add_row_2"), truncation);
    const auto gravity_limits = [&] {
        const auto id = model->currentData().toString();
        int max_degree = id.isEmpty() ? core::egm96_max_degree : core::gravity_max_degree;
        int max_order = id == QStringLiteral("egm2008") ? 2159 : max_degree;
        for (const auto &entry : installed_models)
            if (entry.selection == id.toStdString()) {
                max_degree = std::min(entry.info.degree, core::gravity_max_degree);
                max_order = std::min(entry.info.order, max_degree);
            }
        return std::pair{max_degree, max_order};
    };
    QObject::connect(degree, &QSpinBox::valueChanged, &dialog, [&](int value) {
        order->setMaximum(std::min(value, gravity_limits().second));
    });
    const auto update_gravity = [&] {
        degree->setEnabled(model->currentIndex() != 1);
        order->setEnabled(model->currentIndex() != 1);
        const auto [max_degree, max_order] = gravity_limits();
        degree->setMaximum(max_degree);
        order->setMaximum(std::min(degree->value(), max_order));
    };
    QObject::connect(model, &QComboBox::currentIndexChanged, &dialog, update_gravity);
    update_gravity();
    auto *gravity_status = new QLabel;
    gravity_status->setObjectName("hpopGravityStatus");
    gravity_status->setWordWrap(true);
    gravity->addRow(QString{}, gravity_status);
    auto *gravity_progress = new QProgressBar;
    gravity_progress->setObjectName("hpopGravityProgress");
    gravity_progress->hide();
    gravity->addRow(QString{}, gravity_progress);
    auto *gravity_actions = new QWidget;
    auto *gravity_action_layout = new QHBoxLayout(gravity_actions);
    gravity_action_layout->setContentsMargins(0, 0, 0, 0);
    auto *download_gravity = new QPushButton(ui::text("hpop_settings.download_gravity"));
    download_gravity->setObjectName("hpopDownloadGravity");
    auto *cancel_gravity = new QPushButton(ui::text("hpop_settings.cancel_gravity"));
    cancel_gravity->setObjectName("hpopCancelGravityDownload");
    gravity_action_layout->addWidget(download_gravity);
    gravity_action_layout->addWidget(cancel_gravity);
    gravity_action_layout->addStretch();
    gravity->addRow(QString{}, gravity_actions);
    help(gravity, ui::text("hpop_settings.label_4"));
    const auto quantity = [](QFormLayout *form, const QString &label, const char *id, Unit unit,
                             double value, double low, double high) {
        auto *field = new QuantityEditor(unit);
        field->setObjectName(id);
        field->set_label(label);
        field->set_range(low, high, unit);
        field->set_value(value, unit);
        form->addRow(label, field);
        return field;
    };
    auto *secular_use = new QCheckBox(ui::text("hpop_settings.secular_use"));
    secular_use->setObjectName("hpopSecularVariations");
    secular_use->setChecked(options.include_secular_variations);
    secular_use->setToolTip(ui::text("hpop_settings.secular_use.set_tool_tip"));
    gravity->addRow(secular_use);
    auto *secular_status = new QLabel;
    secular_status->setObjectName("hpopSecularStatus");
    secular_status->setWordWrap(true);
    gravity->addRow(QString{}, secular_status);
    auto *solid = section(gravity_page, ui::text("hpop_settings.solid"), "hpopSolidTidesSection");
    auto *solid_mode = new QComboBox;
    solid_mode->setObjectName("hpopSolidTideMode");
    solid_mode->addItems({ui::text("hpop_settings.solid_mode"),
                          ui::text("hpop_settings.solid_mode_2"),
                          ui::text("hpop_settings.label_5")});
    solid_mode->setCurrentIndex(static_cast<int>(options.solid_tides.mode));
    solid_mode->setItemData(2, ui::text("hpop_settings.solid_mode.set_item_data"), Qt::ToolTipRole);
    solid->addRow(ui::text("hpop_settings.solid.add_row"), solid_mode);
    auto *solid_k20 = new QLabel;
    solid_k20->setObjectName("hpopSolidTideK20");
    solid_k20->setWordWrap(true);
    solid_k20->setToolTip(ui::text("hpop_settings.solid_k20.set_tool_tip"));
    solid->addRow(ui::text("hpop_settings.solid.add_row_2"), solid_k20);
    auto *solid_truncate = new QCheckBox(ui::text("hpop_settings.solid_truncate"));
    solid_truncate->setObjectName("hpopSolidTideTruncate");
    solid_truncate->setChecked(options.solid_tides.truncate_to_gravity);
    solid->addRow(solid_truncate);
    auto *solid_time = new QCheckBox(ui::text("hpop_settings.solid_time"));
    solid_time->setObjectName("hpopSolidTideTimeDependent");
    solid_time->setChecked(options.solid_tides.include_time_dependent_terms);
    solid_time->setToolTip(ui::text("hpop_settings.solid_time.set_tool_tip"));
    solid->addRow(solid_time);
    const auto refresh_solid = [solid_mode, solid_truncate, solid_time] {
        solid_truncate->setEnabled(solid_mode->currentIndex() != 0);
        solid_time->setEnabled(solid_mode->currentIndex() == 2);
    };
    QObject::connect(solid_mode, &QComboBox::currentIndexChanged, &dialog, refresh_solid);
    refresh_solid();
    help(solid, ui::text("hpop_settings.label_6"));
    caution(solid, ui::text("hpop_settings.label_7"));
    auto installed_oceans =
        data ? data->data().ocean_tide_models() : std::vector<runtime::CatalogOceanTides>{};
    auto *ocean = section(gravity_page, ui::text("hpop_settings.ocean"), "hpopOceanTidesSection");
    auto *ocean_use = new QCheckBox(ui::text("hpop_settings.ocean_use"));
    ocean_use->setObjectName("hpopUseOceanTides");
    ocean_use->setChecked(options.ocean_tides.enabled);
    ocean->addRow(ocean_use);
    auto *ocean_model = new QComboBox;
    ocean_model->setObjectName("hpopOceanTideModel");
    ocean_model->addItem(ui::text("model.fes2014bv1"), QStringLiteral("fes2014bv1"));
    const auto refresh_ocean_models = [&] {
        const QSignalBlocker blocker(ocean_model);
        for (const auto &entry : installed_oceans) {
            const auto id = QString::fromStdString(entry.selection);
            if (entry.selection == "fes2014bv1")
                continue;
            const auto title =
                QString::fromStdString(entry.info.model) + ui::text("hpop_settings.label_3");
            const int index = ocean_model->findData(id);
            if (index < 0)
                ocean_model->addItem(title, id);
            else
                ocean_model->setItemText(index, title);
        }
    };
    refresh_ocean_models();
    const auto ocean_id = QString::fromStdString(options.ocean_tides.model);
    if (ocean_model->findData(ocean_id) < 0)
        ocean_model->addItem(ocean_id, ocean_id);
    ocean_model->setCurrentIndex(ocean_model->findData(ocean_id));
    ocean->addRow(ui::text("hpop_settings.solid.add_row"), ocean_model);
    auto *ocean_degree = new QSpinBox;
    ocean_degree->setObjectName("hpopOceanTideDegree");
    ocean_degree->setRange(0, core::gravity_max_degree);
    ocean_degree->setValue(options.ocean_tides.degree);
    auto *ocean_order = new QSpinBox;
    ocean_order->setObjectName("hpopOceanTideOrder");
    ocean_order->setRange(0, options.ocean_tides.degree);
    ocean_order->setValue(options.ocean_tides.order);
    ocean->addRow(ui::text("hpop_settings.degree.set_accessible_name"), ocean_degree);
    ocean->addRow(ui::text("hpop_settings.order.set_accessible_name"), ocean_order);
    QObject::connect(ocean_degree, &QSpinBox::valueChanged, ocean_order, &QSpinBox::setMaximum);
    auto *ocean_status = new QLabel;
    ocean_status->setObjectName("hpopOceanTideStatus");
    ocean_status->setWordWrap(true);
    ocean->addRow(ocean_status);
    auto *ocean_progress = new QProgressBar;
    ocean_progress->setObjectName("hpopOceanTideProgress");
    ocean_progress->hide();
    ocean->addRow(ocean_progress);
    auto *ocean_download = new QPushButton(ui::text("hpop_settings.ocean_download"));
    ocean_download->setObjectName("hpopDownloadOceanTides");
    ocean->addRow(ocean_download);
    auto *ocean_cancel = new QPushButton(ui::text("hpop_settings.cancel_gravity"));
    ocean_cancel->setObjectName("hpopCancelOceanDownload");
    ocean->addRow(ocean_cancel);
    auto *ocean_manage = new QPushButton(ui::text("hpop_settings.ocean_manage"));
    ocean_manage->setObjectName("hpopManageOceanTides");
    // Attach optional controls before making them visible; unparented buttons are windows.
    ocean->addRow(ocean_manage);
    ocean_manage->setVisible(data != nullptr);
    help(ocean, ui::text("hpop_settings.label_8"));
    caution(ocean, ui::text("hpop_settings.label_9"));
    auto *third = section(gravity_page, ui::text("hpop_settings.third"), "hpopThirdBodySection");
    third->setVerticalSpacing(8);
    auto *resource = new QFrame;
    resource->setObjectName("hpopResource");
    auto *resource_layout = new QVBoxLayout(resource);
    resource_layout->setContentsMargins(theme::space::card);
    resource_layout->setSpacing(6);
    auto *resource_header = new QHBoxLayout;
    auto *resource_title = new QLabel(options.ephemeris_model == "de440s"
                                          ? ui::text("model.jpl_de440s")
                                          : QString::fromStdString(options.ephemeris_model));
    resource_title->setObjectName("hpopResourceTitle");
    auto *resource_badge = new QLabel;
    resource_badge->setObjectName("hpopResourceBadge");
    resource_header->addWidget(resource_title);
    resource_header->addStretch();
    resource_header->addWidget(resource_badge);
    resource_layout->addLayout(resource_header);
    auto *ephemeris_status = new QLabel;
    ephemeris_status->setObjectName("hpopEphemerisStatus");
    ephemeris_status->setWordWrap(true);
    resource_layout->addWidget(ephemeris_status);
    auto *manage = new QToolButton;
    manage->setObjectName("hpopManageEphemerides");
    manage->setText(ui::text("hpop_settings.manage.set_text"));
    manage->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    manage->setIcon(workbench_icon(WorkbenchIcon::database));
    resource_layout->addWidget(manage, 0, Qt::AlignLeft);
    manage->setVisible(static_cast<bool>(manage_ephemerides));
    const auto refresh_ephemeris = [&] {
        const auto found =
            std::find_if(installed_ephemerides.begin(), installed_ephemerides.end(),
                         [&](const auto &v) { return v.selection == options.ephemeris_model; });
        const bool ready = found != installed_ephemerides.end();
        resource_badge->setText(ready ? ui::text("hpop_settings.resource_badge.set_text")
                                      : ui::text("hpop_settings.resource_badge.set_text_2"));
        const auto date = [](core::Epoch epoch) {
            return QDateTime(QDate(2000, 1, 1), QTime(12, 0), QTimeZone::UTC)
                .addSecs(epoch.tai_nanoseconds() / 1000000000LL)
                .toString("yyyy-MM-dd");
        };
        ephemeris_status->setText(ready ? ui::text("hpop_settings.ephemeris_status.set_text")
                                              .arg(date(found->info.first), date(found->info.last))
                                        : ui::text("hpop_settings.label_10"));
    };
    QObject::connect(manage, &QToolButton::clicked, &dialog, [&] {
        try {
            installed_ephemerides = manage_ephemerides();
            refresh_ephemeris();
        } catch (const std::exception &e) {
            ephemeris_status->setText(QString::fromUtf8(e.what()));
        }
    });
    refresh_ephemeris();
    third->addRow(resource);
    struct ManualDraft {
        QString text;
        Unit unit;
        std::optional<double> si;
    };
    struct BodyFields {
        QCheckBox *enabled;
        QComboBox *source;
        QuantityEditor *mu;
        std::shared_ptr<ManualDraft> manual;
        std::shared_ptr<std::optional<core::CbGravity>> cb;
        QString default_manual;
        std::function<void()> update;
    };
    auto *search = new QLineEdit;
    search->setObjectName("hpopThirdBodySearch");
    search->setPlaceholderText(ui::text("hpop_settings.search.set_placeholder_text"));
    third->addRow(search);
    auto *table = new QTableWidget(static_cast<int>(core::third_body_count), 4);
    table->setObjectName("hpopThirdBodyTable");
    table->setHorizontalHeaderLabels(
        {ui::text("hpop_settings.label_11"), ui::text("hpop_settings.label_12"),
         ui::text("hpop_settings.label_13"), ui::text("hpop_settings.label_14")});
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(36);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setMinimumHeight(200);
    table->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table->horizontalHeader()->setMinimumSectionSize(36);
    table->setColumnWidth(0, 110);
    table->setColumnWidth(1, 48);
    table->setColumnWidth(2, 140);
    table->setColumnWidth(3, 220);
    table->horizontalHeader()->setStretchLastSection(true);
    // Table scrolls internally at narrow widths; fields stay alive across resizing.
    third->addRow(table);
    constexpr std::array name_keys{ui::key("body.sun"),     ui::key("body.moon"),
                                   ui::key("body.jupiter"), ui::key("body.venus"),
                                   ui::key("body.saturn"),  ui::key("body.mars"),
                                   ui::key("body.mercury"), ui::key("body.uranus"),
                                   ui::key("body.neptune"), ui::key("body.pluto")};
    core::ThirdBodyDataInfo constants;
    constants.sun_mu = runtime::de440_sun_mu;
    constants.moon_mu = runtime::de440_moon_mu;
    constants.planet_mu = runtime::de440_planet_mu;
    std::array<BodyFields, core::third_body_count> fields;
    for (auto body : core::third_body_list) {
        const auto row = static_cast<int>(body);
        const auto english = QString::fromUtf8(core::third_body_name(body).data());
        const auto title = ui::text(name_keys[row]);
        auto *name = new QTableWidgetItem(title + " " + english);
        name->setData(Qt::UserRole, title + " " + english);
        name->setToolTip(core::third_body_naif_id(body) < 10
                             ? ui::text("hpop_settings.name.set_tool_tip")
                             : ui::text("hpop_settings.label_15"));
        table->setItem(row, 0, name);
        const auto &settings = options.third_body(body);
        auto *check = new QCheckBox;
        check->setObjectName("hpop" + english + "Gravity");
        check->setAccessibleName(title + ui::text("hpop_settings.gravity_page"));
        check->setChecked(settings.enabled);
        auto *check_cell = new QWidget;
        auto *check_layout = new QHBoxLayout(check_cell);
        check_layout->setContentsMargins(0, 0, 0, 0);
        check_layout->addWidget(check, 0, Qt::AlignCenter);
        table->setCellWidget(row, 1, check_cell);
        auto *source = new QComboBox;
        source->setObjectName("hpop" + english + "Source");
        source->addItems({ui::text("hpop.source.cb_file"), ui::text("hpop.source.jpl_de"),
                          ui::text("hpop.source.user_specified")});
        source->setMinimumWidth(0);
        source->setStyleSheet(QStringLiteral(
            "QComboBox { padding: 4px 5px; } QComboBox::drop-down { width: 18px; }"));
        source->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        source->setMinimumContentsLength(3);
        source->setCurrentIndex(static_cast<int>(settings.source));
        table->setCellWidget(row, 2, source);
        auto *mu = new QuantityEditor(Unit::kilometres_cubed_per_second_squared);
        mu->setObjectName("hpop" + english + "Mu");
        mu->set_label(title + QStringLiteral(" GM"));
        mu->setMaximumWidth(QWIDGETSIZE_MAX);
        mu->set_range(std::numeric_limits<double>::min(), std::numeric_limits<double>::max(),
                      Unit::metres_cubed_per_second_squared);
        mu->set_value(settings.user_mu.value_or(constants.mu(body)),
                      Unit::metres_cubed_per_second_squared);
        auto *input = mu->findChild<QLineEdit *>("quantityValue");
        auto manual = std::make_shared<ManualDraft>(
            ManualDraft{input->text(), mu->unit(), settings.user_mu.value_or(constants.mu(body))});
        auto cb = std::make_shared<std::optional<core::CbGravity>>(settings.cb);
        const auto update = [=] {
            source->setEnabled(check->isChecked());
            mu->setEnabled(check->isChecked());
            mu->set_read_only(source->currentIndex() != 2);
            if (source->currentIndex() == 1)
                mu->set_value(constants.mu(body), Unit::metres_cubed_per_second_squared);
            else if (source->currentIndex() == 0) {
                if (*cb)
                    mu->set_value((*cb)->mu, Unit::metres_cubed_per_second_squared);
                else
                    input->setText(ui::text("hpop_settings.input.set_text"));
            }
            if (source->currentIndex() != 2)
                input->setCursorPosition(0);
            mu->setToolTip(
                source->currentIndex() == 0 && *cb
                    ? QString::fromStdString((*cb)->name + "\nSHA-256: " + (*cb)->content_sha256)
                    : ui::text("hpop_settings.label_16"));
        };
        auto previous = std::make_shared<int>(source->currentIndex());
        QObject::connect(source, &QComboBox::currentIndexChanged, &dialog, [=](int index) {
            if (*previous == 2) {
                manual->text = input->text();
                manual->unit = mu->unit();
                try {
                    manual->si = mu->value(Unit::metres_cubed_per_second_squared);
                } catch (const std::exception &) {
                    manual->si.reset();
                }
            }
            if (index == 2) {
                // Reset an unavailable Cb display before converting units. Retain both
                // the exact SI value and literal text, including unfinished manual input.
                mu->set_value(manual->si.value_or(constants.mu(body)),
                              Unit::metres_cubed_per_second_squared);
                mu->set_unit(manual->unit);
                if (manual->si) {
                    const QSignalBlocker blocked(input);
                    input->setText(manual->text);
                } else
                    input->setText(manual->text);
            }
            *previous = index;
            update();
        });
        QObject::connect(check, &QCheckBox::toggled, &dialog, [=] { update(); });
        table->setCellWidget(row, 3, mu);
        fields[row] = {check, source, mu, manual, cb, manual->text, update};
        update();
    }
    const auto resize_columns = [=] {
        const bool compact = table->viewport()->width() < 520;
        const QSignalBlocker blocked(table->horizontalHeader());
        table->setColumnWidth(0, compact ? 54 : 110);
        table->setColumnWidth(1, 40);
        table->setColumnWidth(2, compact ? 92 : 140);
        table->setColumnWidth(3, compact ? 182 : 220);
        table->horizontalHeaderItem(0)->setText(compact ? ui::text("hpop_settings.label_17")
                                                        : ui::text("hpop_settings.label_11"));
        table->horizontalHeaderItem(2)->setText(compact ? ui::text("hpop_settings.label_18")
                                                        : ui::text("hpop_settings.label_13"));
        for (int row = 0; row < table->rowCount(); ++row) {
            table->item(row, 0)->setText(compact
                                             ? ui::text(name_keys[row])
                                             : table->item(row, 0)->data(Qt::UserRole).toString());
            fields[row].source->setItemText(2, compact
                                                   ? ui::text("hpop_settings.source.set_item_text")
                                                   : ui::text("hpop.source.user_specified"));
            fields[row].source->setToolTip(ui::text("hpop.source.choices"));
        }
    };
    QObject::connect(table->horizontalHeader(), &QHeaderView::geometriesChanged, &dialog,
                     resize_columns);
    resize_columns();
    QObject::connect(search, &QLineEdit::textChanged, &dialog, [=](const QString &text) {
        for (int row = 0; row < table->rowCount(); ++row)
            table->setRowHidden(row, !table->item(row, 0)
                                          ->data(Qt::UserRole)
                                          .toString()
                                          .contains(text.trimmed(), Qt::CaseInsensitive));
    });
    auto *cb_controls = new QWidget;
    auto *cb_layout = new QFormLayout(cb_controls);
    cb_layout->setContentsMargins(0, 0, 0, 0);
    cb_layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto *import_cb = new QToolButton;
    import_cb->setObjectName("hpopImportCb");
    import_cb->setText(ui::text("hpop_settings.import_cb.set_text"));
    auto *cb_units = new QComboBox;
    cb_units->setObjectName("hpopCbUnits");
    cb_units->addItems({ui::text("hpop_settings.cb_units"), ui::text("hpop_settings.cb_units_2")});
    cb_layout->addRow(import_cb, cb_units);
    third->addRow(cb_controls);
    auto *cb_status = new QLabel;
    cb_status->setObjectName("hpopCbStatus");
    cb_status->setWordWrap(true);
    third->addRow(cb_status);
    QObject::connect(import_cb, &QToolButton::clicked, &dialog, [&] {
        const auto row = table->currentRow();
        if (row < 0) {
            cb_status->setText(ui::text("hpop_settings.cb_status.set_text"));
            return;
        }
        const auto path = QFileDialog::getOpenFileName(&dialog, ui::text("hpop_settings.path"), {},
                                                       ui::text("hpop.cb_file_filter"));
        if (path.isEmpty())
            return;
        try {
            auto value = runtime::load_cb_gravity(std::filesystem::path(path.toStdWString()),
                                                  core::third_body_list[row],
                                                  cb_units->currentIndex() == 0 ? 1.0 : 1e9);
            *fields[row].cb = std::move(value);
            fields[row].source->setCurrentIndex(0);
            fields[row].update();
            cb_status->setText(ui::text("hpop_settings.cb_status.set_text_2")
                                   .arg(QString::fromStdString((*fields[row].cb)->name)));
        } catch (const std::exception &e) {
            cb_status->setText(QString::fromUtf8(e.what()));
        }
    });
    table->selectRow(0);
    help(third, ui::text("hpop_settings.label_19"));
    auto *drag_page = page(force_tabs, ui::text("hpop_settings.drag_page"), "hpopDragPage");
    auto *drag_use = new QCheckBox(ui::text("hpop_settings.drag_use"));
    drag_use->setObjectName("hpopUseDrag");
    drag_use->setChecked(options.drag.enabled);
    make_switch(drag_use);
    drag_page->addRow(drag_use);
    auto *drag_off = new QLabel(ui::text("hpop_settings.drag_off"));
    drag_off->setObjectName("hint");
    drag_off->setWordWrap(true);
    drag_page->addRow(drag_off);
    auto *drag_model =
        section(drag_page, ui::text("hpop_settings.drag_model"), "hpopDragModelSection");
    auto *drag_type = new QComboBox;
    drag_type->setObjectName("hpopDragType");
    drag_type->addItem(ui::text("hpop_settings.drag_type.add_item"));
    drag_model->addRow(ui::text("hpop_settings.drag_model.add_row"), drag_type);
    auto *drag_cd = quantity(drag_model, ui::text("hpop_settings.drag_cd"), "hpopDragCd", Unit::one,
                             options.drag.cd, -100, 100);
    auto *drag_area =
        quantity(drag_model, ui::text("hpop_settings.drag_area"), "hpopDragAreaMassRatio",
                 Unit::square_metres_per_kilogram, options.drag.area_mass_ratio_m2_per_kg, 0, 9999);
    auto *drag_compatible_area = new QLabel;
    drag_compatible_area->setObjectName("hpopDragCompatibleArea");
    drag_compatible_area->setWordWrap(true);
    drag_model->addRow(ui::text("hpop_settings.drag_model.add_row_2"), drag_compatible_area);
    auto *drag_density =
        section(drag_page, ui::text("hpop_settings.drag_density"), "hpopDragDensitySection");
    auto *density_model = new QComboBox;
    density_model->setObjectName("hpopDragDensityModel");
    density_model->addItems({ui::text("density.jacchia_roberts"), ui::text("density.nrlmsise00")});
    density_model->setCurrentIndex(static_cast<int>(options.drag.atmosphere));
    drag_density->addRow(ui::text("hpop_settings.drag_density.add_row"), density_model);
    auto *drag_cutoff = new QLabel(ui::text("hpop_settings.drag_cutoff"));
    drag_cutoff->setObjectName("hpopDragHighAltitudeCutoff");
    drag_cutoff->setWordWrap(true);
    drag_density->addRow(ui::text("hpop_settings.drag_density.add_row_2"), drag_cutoff);
    auto *low_density = new QComboBox;
    low_density->setObjectName("hpopDragLowAltitudeModel");
    low_density->addItems({ui::text("hpop_settings.low_density"), ui::text("density.nrlmsise00")});
    low_density->setCurrentIndex(static_cast<int>(options.drag.low_altitude_model));
    drag_density->addRow(ui::text("hpop_settings.drag_density.add_row_3"), low_density);
    auto *drag_blend =
        quantity(drag_density, ui::text("hpop_settings.drag_blend"), "hpopDragBlendingRange",
                 Unit::metre, options.drag.blending_range_m, 0, 1000000);
    drag_blend->set_unit(Unit::kilometre);
    help(drag_density, ui::text("hpop_settings.label_20"));
    auto *drag_weather =
        section(drag_page, ui::text("hpop_settings.drag_weather"), "hpopDragWeatherSection");
    auto *drag_weather_source = new QComboBox;
    drag_weather_source->setObjectName("hpopDragWeatherSource");
    drag_weather_source->addItem(ui::text("hpop_settings.drag_weather_source.add_item"));
    drag_weather_source->addItem(ui::text("hpop_settings.drag_weather_source.add_item_2"));
    drag_weather_source->setCurrentIndex(options.drag.weather_model.empty() ? 0 : 1);
    drag_weather->addRow(ui::text("hpop_settings.drag_weather.add_row"), drag_weather_source);
    auto *drag_f107 = quantity(drag_weather, ui::text("hpop_settings.drag_f107"), "hpopDragF107",
                               Unit::one, options.drag.f107, 1e-9, 1000);
    auto *drag_average =
        quantity(drag_weather, ui::text("hpop_settings.drag_average"), "hpopDragF107Average",
                 Unit::one, options.drag.f107_average, 1e-9, 1000);
    auto *drag_kp = quantity(drag_weather, ui::text("hpop_settings.drag_kp"), "hpopDragKp",
                             Unit::one, options.drag.kp, 0, 9);
    auto *weather_file = new QComboBox;
    weather_file->setObjectName("hpopDragWeatherFile");
    std::function<void()> refresh_weather_policy;
    const auto refresh_weather_files = [&] {
        const auto selected = weather_file->currentData().toString();
        const auto id = selected.isEmpty()
                            ? QString::fromStdString(options.drag.weather_model.empty()
                                                         ? "celestrak"
                                                         : options.drag.weather_model)
                            : selected;
        const QSignalBlocker blocker(weather_file);
        weather_file->clear();
        if (data)
            for (const auto &entry : data->data().weather_sources()) {
                weather_file->addItem(QString::fromStdString(entry.selection),
                                      QString::fromStdString(entry.selection));
                weather_file->setItemData(weather_file->count() - 1, entry.info.cssi_predictions,
                                          Qt::UserRole + 1);
            }
        if (weather_file->findData(id) < 0)
            weather_file->addItem(id + ui::text("hpop_settings.weather_file.add_item"), id);
        weather_file->setCurrentIndex(weather_file->findData(id));
        if (refresh_weather_policy)
            refresh_weather_policy();
    };
    refresh_weather_files();
    drag_weather->addRow(ui::text("hpop_settings.drag_weather.add_row_2"), weather_file);
    auto *weather_update = new QComboBox;
    weather_update->setObjectName("hpopDragGeomagneticUpdate");
    weather_update->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    weather_update->setMinimumContentsLength(18);
    weather_update->addItems(
        {ui::text("hpop_settings.label_21"), ui::text("hpop_settings.label_22"),
         ui::text("hpop_settings.label_23"), ui::text("hpop_settings.label_24")});
    weather_update->setToolTip(ui::text("hpop_settings.weather_update.set_tool_tip"));
    weather_update->setCurrentIndex(static_cast<int>(options.drag.geomagnetic_update));
    drag_weather->addRow(ui::text("hpop_settings.drag_weather.add_row_3"), weather_update);
    auto *weather_source = new QComboBox;
    weather_source->setObjectName("hpopDragGeomagneticSource");
    weather_source->addItems({QStringLiteral("Ap"), QStringLiteral("Kp")});
    weather_source->setCurrentIndex(static_cast<int>(options.drag.geomagnetic_source));
    drag_weather->addRow(ui::text("hpop_settings.drag_weather.add_row_4"), weather_source);
    refresh_weather_policy = [&] {
        const bool enabled = drag_use->isChecked() && drag_weather_source->currentIndex() == 1;
        const bool ap_only = enabled && weather_file->currentData(Qt::UserRole + 1).toBool();
        if (ap_only)
            weather_source->setCurrentIndex(0);
        weather_source->setEnabled(enabled && !ap_only);
        weather_source->setToolTip(ap_only ? ui::text("hpop_settings.weather_source.set_tool_tip")
                                           : QString{});
    };
    auto *weather_manage = new QPushButton(ui::text("hpop_settings.weather_manage"));
    weather_manage->setObjectName("hpopDragManageWeather");
    drag_weather->addRow(QString{}, weather_manage);
    weather_manage->setVisible(data != nullptr);
    QObject::connect(weather_manage, &QPushButton::clicked, &dialog, [&] {
        if (data)
            show_data_center(&dialog, *data, DataProduct::space_weather);
        refresh_weather_files();
    });
    help(drag_weather, ui::text("hpop_settings.label_25"));
    auto *drag_sun = section(drag_page, ui::text("hpop_settings.drag_sun"), "hpopDragSunSection");
    auto *drag_approximate = new QCheckBox(ui::text("hpop_settings.drag_approximate"));
    drag_approximate->setObjectName("hpopDragApproximateAltitude");
    drag_approximate->setChecked(options.drag.use_approximate_altitude);
    drag_approximate->setToolTip(ui::text("hpop_settings.drag_approximate.set_tool_tip"));
    drag_sun->addRow(QString{}, drag_approximate);
    auto *drag_apparent = new QCheckBox(ui::text("hpop_settings.drag_apparent"));
    drag_apparent->setObjectName("hpopDragApparentSun");
    drag_apparent->setChecked(options.drag.use_apparent_sun_position);
    drag_sun->addRow(QString{}, drag_apparent);
    auto *drag_resource = new QLabel;
    drag_resource->setObjectName("hpopDragEphemerisStatus");
    drag_resource->setWordWrap(true);
    drag_sun->addRow(QString{}, drag_resource);
    const auto refresh_drag_resource = [&] {
        const bool available = std::any_of(
            installed_ephemerides.begin(), installed_ephemerides.end(),
            [&](const auto &entry) { return entry.selection == options.ephemeris_model; });
        if (auto *group = group_of(drag_sun)) {
            group->set_summary(available ? ui::text("hpop_settings.group.set_summary")
                                         : ui::text("hpop_settings.group.set_summary_2"));
            if (!available)
                group->set_expanded(true);
        }
        drag_resource->setText(available ? ui::text("hpop_settings.drag_resource.set_text")
                                               .arg(QString::fromStdString(options.ephemeris_model))
                                         : ui::text("hpop_settings.label_26"));
    };
    refresh_drag_resource();
    auto *drag_manage = new QPushButton(ui::text("hpop_settings.drag_manage"));
    drag_manage->setObjectName("hpopDragManageEphemerides");
    drag_sun->addRow(QString{}, drag_manage);
    drag_manage->setVisible(static_cast<bool>(manage_ephemerides));
    std::function<void()> refresh_srp_resource;
    QObject::connect(drag_manage, &QPushButton::clicked, &dialog, [&] {
        if (manage_ephemerides) {
            installed_ephemerides = manage_ephemerides();
            refresh_ephemeris();
            refresh_drag_resource();
            if (refresh_srp_resource)
                refresh_srp_resource();
        }
    });
    help(drag_sun, ui::text("hpop_settings.label_27"));
    if (auto *group = group_of(drag_sun))
        group->set_collapsible(true);
    const auto update_drag = [&] {
        const bool enabled = drag_use->isChecked();
        // A switched-off model folds away to one line instead of a page of inactive fields.
        for (auto *form : {drag_model, drag_density, drag_weather, drag_sun}) {
            form->parentWidget()->setEnabled(enabled);
            group_of(form)->setVisible(enabled);
        }
        drag_off->setVisible(!enabled);
        const bool file_weather = drag_weather_source->currentIndex() == 1;
        for (auto *input : {drag_f107, drag_average, drag_kp}) {
            input->setEnabled(enabled && !file_weather);
            drag_weather->setRowVisible(input, !file_weather);
        }
        for (auto *input : {weather_file, weather_update, weather_source}) {
            input->setEnabled(enabled && file_weather);
            drag_weather->setRowVisible(input, file_weather);
        }
        refresh_weather_policy();
        weather_manage->setEnabled(enabled && file_weather && data);
        drag_weather->setRowVisible(weather_manage, file_weather && data);
        const bool jr = density_model->currentIndex() == 0;
        low_density->setEnabled(enabled && jr);
        drag_blend->setEnabled(enabled && jr && low_density->currentIndex() == 1);
        refresh_drag_resource();
    };
    QObject::connect(drag_use, &QCheckBox::toggled, &dialog, update_drag);
    QObject::connect(density_model, &QComboBox::currentIndexChanged, &dialog, update_drag);
    QObject::connect(low_density, &QComboBox::currentIndexChanged, &dialog, update_drag);
    QObject::connect(drag_weather_source, &QComboBox::currentIndexChanged, &dialog, update_drag);
    QObject::connect(weather_file, &QComboBox::currentIndexChanged, &dialog, update_drag);
    update_drag();
    auto *srp_page = page(force_tabs, ui::text("hpop_settings.srp_page"), "hpopSrpPage");
    auto *srp_use = new QCheckBox(ui::text("hpop_settings.srp_use"));
    srp_use->setObjectName("hpopUseSrp");
    srp_use->setChecked(options.srp.enabled);
    make_switch(srp_use);
    srp_page->addRow(srp_use);
    auto *srp_off = new QLabel(ui::text("hpop_settings.srp_off"));
    srp_off->setObjectName("hint");
    srp_off->setWordWrap(true);
    srp_page->addRow(srp_off);
    auto *srp_model = section(srp_page, ui::text("hpop_settings.srp_model"), "hpopSrpModelSection");
    auto *srp_type = new QComboBox;
    srp_type->setObjectName("hpopSrpType");
    srp_type->addItem(ui::text("hpop_settings.drag_type.add_item"));
    srp_model->addRow(ui::text("hpop_settings.drag_model.add_row"), srp_type);
    auto *srp_cr = quantity(srp_model, ui::text("hpop_settings.srp_cr"), "hpopSrpCr", Unit::one,
                            options.srp.cr, -100, 100);
    auto *srp_area =
        quantity(srp_model, ui::text("hpop_settings.drag_area"), "hpopSrpAreaMassRatio",
                 Unit::square_metres_per_kilogram, options.srp.area_mass_ratio_m2_per_kg, 0, 9999);
    auto *srp_compatible_area = new QLabel;
    srp_compatible_area->setObjectName("hpopSrpCompatibleArea");
    srp_compatible_area->setWordWrap(true);
    srp_model->addRow(ui::text("hpop_settings.drag_model.add_row_2"), srp_compatible_area);
    auto *srp_shadow_section =
        section(srp_page, ui::text("hpop_settings.srp_shadow_section"), "hpopSrpShadowSection");
    auto *srp_shadow = new QComboBox;
    srp_shadow->setObjectName("hpopSrpShadowModel");
    srp_shadow->addItems({ui::text("hpop_settings.srp_shadow"),
                          ui::text("hpop_settings.srp_shadow_2"),
                          ui::text("hpop_settings.label_28")});
    srp_shadow->setCurrentIndex(static_cast<int>(options.srp.shadow_model));
    srp_shadow_section->addRow(ui::text("hpop_settings.srp_shadow_section.add_row"), srp_shadow);
    auto *srp_boundary = new QCheckBox(ui::text("hpop_settings.srp_boundary"));
    srp_boundary->setObjectName("hpopSrpBoundaryMitigation");
    srp_boundary->setChecked(options.srp.boundary_mitigation);
    srp_boundary->setToolTip(ui::text("hpop_settings.srp_boundary.set_tool_tip"));
    srp_shadow_section->addRow(QString{}, srp_boundary);
    auto srp_assignments = options.srp;
    auto *srp_assigned_summary = new QLabel;
    srp_assigned_summary->setObjectName("hpopSrpEclipsingSummary");
    srp_assigned_summary->setWordWrap(true);
    srp_shadow_section->addRow(ui::text("hpop_settings.srp_shadow_section.add_row_2"),
                               srp_assigned_summary);
    auto *srp_select_bodies = new QPushButton(ui::text("hpop_settings.srp_select_bodies"));
    srp_select_bodies->setObjectName("hpopSrpSelectBodies");
    srp_shadow_section->addRow(QString{}, srp_select_bodies);
    auto *srp_altitude = quantity(
        srp_shadow_section, ui::text("hpop_settings.srp_altitude"), "hpopSrpAtmosphericAltitude",
        Unit::metre, options.srp.atmospheric_altitude_m, 0, std::numeric_limits<double>::max());
    help(srp_shadow_section, ui::text("hpop_settings.label_29"));
    auto *srp_sun = section(srp_page, ui::text("hpop_settings.srp_sun"), "hpopSrpSunSection");
    auto *srp_sun_method = new QComboBox;
    srp_sun_method->setObjectName("hpopSrpSunMethod");
    srp_sun_method->addItems({ui::text("hpop_settings.srp_sun_method"),
                              ui::text("hpop_settings.label_30"),
                              ui::text("hpop_settings.label_31")});
    srp_sun_method->setItemData(0, ui::text("hpop.sun_method.true"), Qt::ToolTipRole);
    srp_sun_method->setItemData(1, ui::text("hpop.sun_method.apparent_to_true"), Qt::ToolTipRole);
    srp_sun_method->setItemData(2, ui::text("hpop.sun_method.apparent"), Qt::ToolTipRole);
    srp_sun_method->setMinimumContentsLength(12);
    srp_sun_method->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    srp_sun_method->setCurrentIndex(static_cast<int>(options.srp.sun_position_method));
    srp_sun_method->setToolTip(ui::text("hpop_settings.srp_sun_method.set_tool_tip"));
    srp_sun->addRow(ui::text("hpop_settings.srp_sun.add_row"), srp_sun_method);
    auto *srp_resource = new QLabel;
    srp_resource->setObjectName("hpopSrpEphemerisStatus");
    srp_resource->setWordWrap(true);
    srp_sun->addRow(QString{}, srp_resource);
    refresh_srp_resource = [&] {
        const bool ready = std::any_of(
            installed_ephemerides.begin(), installed_ephemerides.end(),
            [&](const auto &entry) { return entry.selection == options.ephemeris_model; });
        srp_resource->setText(ready ? ui::text("hpop_settings.drag_resource.set_text")
                                          .arg(QString::fromStdString(options.ephemeris_model))
                                    : ui::text("hpop_settings.label_32")
                                          .arg(QString::fromStdString(options.ephemeris_model)));
    };
    auto *srp_manage = new QPushButton(ui::text("hpop_settings.srp_manage"));
    srp_manage->setObjectName("hpopSrpManageEphemerides");
    srp_sun->addRow(QString{}, srp_manage);
    srp_manage->setVisible(static_cast<bool>(manage_ephemerides));
    QObject::connect(srp_manage, &QPushButton::clicked, manage, &QToolButton::click);
    QObject::connect(manage, &QToolButton::clicked, &dialog, refresh_srp_resource);
    QObject::connect(manage, &QToolButton::clicked, &dialog, refresh_drag_resource);
    refresh_srp_resource();
    help(srp_sun, ui::text("hpop_settings.label_33"));
    auto *srp_constants =
        section(srp_page, ui::text("hpop_settings.srp_constants"), "hpopSrpConstantsSection");
    auto *srp_pressure =
        quantity(srp_constants, ui::text("hpop_settings.srp_pressure"), "hpopSrpPressure",
                 Unit::pascal, options.srp.pressure_at_reference_pa,
                 std::numeric_limits<double>::min(), std::numeric_limits<double>::max());
    auto *srp_distance =
        quantity(srp_constants, ui::text("hpop_settings.srp_distance"), "hpopSrpReferenceDistance",
                 Unit::metre, options.srp.reference_distance_m, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    auto *srp_sun_radius =
        quantity(srp_constants, ui::text("hpop_settings.srp_sun_radius"), "hpopSrpSunRadius",
                 Unit::metre, options.srp.sun_radius_m, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    auto *srp_earth_radius =
        quantity(srp_constants, ui::text("hpop_settings.srp_earth_radius"), "hpopSrpEarthRadius",
                 Unit::metre, options.srp.earth_radius_m, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    auto *srp_moon_radius =
        quantity(srp_constants, ui::text("hpop_settings.srp_moon_radius"), "hpopSrpMoonRadius",
                 Unit::metre, options.srp.moon_radius_m, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    for (auto *field : {srp_distance, srp_sun_radius, srp_earth_radius, srp_moon_radius})
        field->set_unit(Unit::kilometre);
    // Physical constants rarely change: the group starts folded behind a summary.
    if (auto *group = group_of(srp_constants)) {
        group->set_summary(ui::text("hpop_settings.group.set_summary_3"));
        group->set_collapsible(false);
    }
    caution(srp_page, ui::text("hpop_settings.label_34"));
    const auto update_srp = [&] {
        const bool enabled = srp_use->isChecked(), shadows = srp_shadow->currentIndex() != 0;
        for (auto *form : {srp_model, srp_shadow_section, srp_sun, srp_constants}) {
            form->parentWidget()->setEnabled(enabled);
            group_of(form)->setVisible(enabled);
        }
        srp_off->setVisible(!enabled);
        srp_select_bodies->setEnabled(shadows);
        srp_boundary->setEnabled(shadows &&
                                 (srp_assignments.earth_shadow || srp_assignments.moon_shadow ||
                                  !srp_assignments.additional_eclipsing_bodies.empty()));
        srp_altitude->setEnabled(shadows && srp_assignments.earth_shadow);
        srp_earth_radius->setEnabled(shadows && srp_assignments.earth_shadow);
        srp_moon_radius->setEnabled(shadows && srp_assignments.moon_shadow);
        QStringList names;
        if (srp_assignments.earth_shadow)
            names << ui::text("body.earth_english");
        if (srp_assignments.moon_shadow)
            names << ui::text("body.moon_english");
        for (const auto &body : srp_assignments.additional_eclipsing_bodies)
            names << QString::fromUtf8(core::third_body_name(body.body).data());
        srp_assigned_summary->setText(names.isEmpty() ? ui::text("hpop_settings.solid_mode")
                                                      : names.join(QStringLiteral("、")));
    };
    QObject::connect(srp_use, &QCheckBox::toggled, &dialog, update_srp);
    QObject::connect(srp_shadow, &QComboBox::currentIndexChanged, &dialog, update_srp);
    QObject::connect(srp_select_bodies, &QPushButton::clicked, &dialog, [&] {
        QDialog selection(&dialog);
        selection.setObjectName("hpopSrpBodiesDialog");
        selection.setWindowTitle(ui::text("hpop_settings.label_35"));
        selection.resize(430, 470);
        auto *box = new QVBoxLayout(&selection);
        auto *filter = new QLineEdit;
        filter->setObjectName("hpopSrpBodySearch");
        filter->setPlaceholderText(ui::text("hpop_settings.filter.set_placeholder_text"));
        box->addWidget(filter);
        auto *lists = new QHBoxLayout;
        auto *available = new QListWidget;
        available->setObjectName("hpopSrpAvailableBodies");
        available->setAccessibleName(ui::text("hpop_settings.available.set_accessible_name"));
        auto *assigned = new QListWidget;
        assigned->setObjectName("hpopSrpAssignedBodies");
        assigned->setAccessibleName(ui::text("hpop_settings.assigned.set_accessible_name"));
        for (auto *list : {available, assigned}) {
            list->setMinimumWidth(100);
            list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        }
        auto *available_column = new QVBoxLayout;
        available_column->addWidget(
            new QLabel(ui::text("hpop_settings.available_column.add_widget")));
        available_column->addWidget(available);
        auto *assigned_column = new QVBoxLayout;
        assigned_column->addWidget(
            new QLabel(ui::text("hpop_settings.assigned_column.add_widget")));
        assigned_column->addWidget(assigned);
        lists->addLayout(available_column, 1);
        auto *actions = new QVBoxLayout;
        auto *add = new QToolButton;
        add->setObjectName("hpopSrpAddBody");
        add->setIcon(workbench_icon(WorkbenchIcon::arrow_right));
        add->setAccessibleName(ui::text("hpop.assign_body"));
        auto *remove = new QToolButton;
        remove->setObjectName("hpopSrpRemoveBody");
        remove->setIcon(workbench_icon(WorkbenchIcon::arrow_left));
        remove->setAccessibleName(ui::text("hpop.remove_body"));
        for (auto *button : {add, remove}) {
            button->setProperty("iconOnly", true);
            button->setIconSize(QSize(16, 16));
            button->setFixedSize(28, 28);
            button->setToolTip(button->accessibleName());
        }
        actions->addStretch();
        actions->addWidget(add);
        actions->addWidget(remove);
        actions->addStretch();
        lists->addLayout(actions);
        lists->addLayout(assigned_column, 1);
        box->addLayout(lists, 1);
        auto *radii = new QStackedWidget;
        auto *hint = new QLabel(ui::text("hpop_settings.hint"));
        hint->setWordWrap(true);
        radii->addWidget(hint);
        std::map<int, QuantityEditor *> editors;
        std::map<int, int> radius_pages;
        const auto append = [&](int id, const QString &name, bool selected) {
            auto *item = new QListWidgetItem(name, selected ? assigned : available);
            item->setData(Qt::UserRole, id);
        };
        append(-1, ui::text("hpop_settings.label_36"), srp_assignments.earth_shadow);
        append(1, ui::text("hpop_settings.label_37"), srp_assignments.moon_shadow);
        for (auto body : core::third_body_list) {
            if (body == core::ThirdBody::sun || body == core::ThirdBody::moon)
                continue;
            const auto found = std::ranges::find(srp_assignments.additional_eclipsing_bodies, body,
                                                 &core::SrpEclipsingBody::body);
            const auto value = found == srp_assignments.additional_eclipsing_bodies.end()
                                   ? core::default_srp_eclipsing_body(body)
                                   : *found;
            const auto id = static_cast<int>(body);
            append(id, QString::fromUtf8(core::third_body_name(body).data()),
                   found != srp_assignments.additional_eclipsing_bodies.end());
            auto *radius_page = new QWidget;
            auto *form = new QFormLayout(radius_page);
            form->setContentsMargins(0, 0, 0, 0);
            form->setRowWrapPolicy(QFormLayout::WrapLongRows);
            const auto object_name = "hpopSrpRadius" + std::to_string(id);
            auto *editor = quantity(form, ui::text("hpop_settings.editor"), object_name.c_str(),
                                    Unit::metre, value.radius_m, std::numeric_limits<double>::min(),
                                    std::numeric_limits<double>::max());
            editor->set_unit(Unit::kilometre);
            editors[id] = editor;
            radius_pages[id] = radii->addWidget(radius_page);
        }
        box->addWidget(radii);
        auto *coverage = new QLabel(ui::text("hpop_settings.coverage"));
        coverage->setObjectName("hint");
        coverage->setWordWrap(true);
        box->addWidget(coverage);
        auto *error = new QLabel;
        error->setObjectName("hpopSrpBodiesError");
        error->setWordWrap(true);
        box->addWidget(error);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        box->addWidget(buttons);
        const auto filter_available = [&] {
            for (int i = 0; i < available->count(); ++i) {
                auto *item = available->item(i);
                const bool hidden = !item->text().contains(filter->text(), Qt::CaseInsensitive);
                item->setHidden(hidden);
                if (hidden)
                    item->setSelected(false);
            }
        };
        const auto move = [&](QListWidget *from, QListWidget *to) {
            for (auto *item : from->selectedItems()) {
                from->takeItem(from->row(item));
                to->addItem(item);
                item->setHidden(false);
                to->setCurrentItem(item);
            }
            filter_available();
        };
        QObject::connect(filter, &QLineEdit::textChanged, &selection, filter_available);
        QObject::connect(add, &QToolButton::clicked, &selection,
                         [&] { move(available, assigned); });
        QObject::connect(remove, &QToolButton::clicked, &selection,
                         [&] { move(assigned, available); });
        QObject::connect(available, &QListWidget::itemDoubleClicked, &selection,
                         [&](QListWidgetItem *) { move(available, assigned); });
        QObject::connect(assigned, &QListWidget::itemDoubleClicked, &selection,
                         [&](QListWidgetItem *) { move(assigned, available); });
        QObject::connect(
            assigned, &QListWidget::currentItemChanged, &selection, [&](QListWidgetItem *item) {
                const auto id = item ? item->data(Qt::UserRole).toInt() : -1;
                radii->setCurrentIndex(radius_pages.contains(id) ? radius_pages.at(id) : 0);
            });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &selection, &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &selection, [&] {
            try {
                auto candidate = srp_assignments;
                candidate.earth_shadow = candidate.moon_shadow = false;
                candidate.additional_eclipsing_bodies.clear();
                for (int i = 0; i < assigned->count(); ++i) {
                    auto *item = assigned->item(i);
                    const auto id = item->data(Qt::UserRole).toInt();
                    if (id == -1)
                        candidate.earth_shadow = true;
                    else if (id == 1)
                        candidate.moon_shadow = true;
                    else {
                        assigned->setCurrentItem(item);
                        candidate.additional_eclipsing_bodies.push_back(
                            {static_cast<core::ThirdBody>(id), editors.at(id)->value(Unit::metre)});
                    }
                }
                core::validate_srp_options(candidate);
                srp_assignments = std::move(candidate);
                selection.accept();
            } catch (const std::exception &e) {
                error->setText(QString::fromUtf8(e.what()));
            }
        });
        if (selection.exec() == QDialog::Accepted)
            update_srp();
    });
    update_srp();
    auto *additional_page =
        page(force_tabs, ui::text("hpop_settings.additional_page"), "hpopAdditionalPage");
    auto *mass_section =
        section(additional_page, ui::text("hpop_settings.mass_section"), "hpopMassSection");
    auto *satellite_mass =
        quantity(mass_section, ui::text("hpop_settings.satellite_mass"), "hpopSatelliteMass",
                 Unit::kilogram, options.satellite_mass_kg,
                 std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max());
    help(mass_section, ui::text("hpop_settings.label_38"));
    const auto refresh_compatible_areas = [&] {
        const auto refresh = [&](QuantityEditor *ratio, QLabel *label) {
            try {
                core::HpopOptions value;
                value.satellite_mass_kg = satellite_mass->value(Unit::kilogram);
                value.drag.area_mass_ratio_m2_per_kg =
                    ratio->value(Unit::square_metres_per_kilogram);
                label->setText(QStringLiteral("%1 m²").arg(value.drag_area_m2(), 0, 'g', 12));
            } catch (const std::exception &) {
                label->setText(ui::text("hpop_settings.label.set_text"));
            }
        };
        refresh(drag_area, drag_compatible_area);
        refresh(srp_area, srp_compatible_area);
    };
    for (auto *field : {satellite_mass, drag_area, srp_area}) {
        QObject::connect(field->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged,
                         &dialog, refresh_compatible_areas);
        QObject::connect(field, &QuantityEditor::unitChanged, &dialog, refresh_compatible_areas);
    }
    refresh_compatible_areas();
    auto *relativity_section = section(
        additional_page, ui::text("hpop_settings.relativity_section"), "hpopRelativitySection");
    auto *relativity = new QCheckBox(ui::text("hpop_settings.relativity"));
    relativity->setObjectName("hpopRelativisticAccelerations");
    relativity->setChecked(options.include_relativistic_accelerations);
    relativity_section->addRow(relativity);
    help(relativity_section, ui::text("hpop_settings.label_39"));
    auto *additional_pending =
        section(additional_page, ui::text("hpop_settings.additional_pending"),
                "hpopAdditionalPendingSection");
    caution(additional_pending, ui::text("hpop_settings.label_40"));
    auto *integration_page =
        page(tabs, ui::text("hpop_settings.integration_page"), "hpopIntegratorPage");
    auto *integration = section(integration_page, ui::text("hpop_settings.integration"),
                                "hpopIntegratorMethodSection");
    auto *method = new QComboBox;
    method->setObjectName("hpopIntegrator");
    method->addItems({ui::text("integrator.rkf78"), ui::text("integrator.rk4"),
                      ui::text("integrator.rkv89"), ui::text("integrator.bulirsch_stoer"),
                      ui::text("integrator.gauss_jackson")});
    method->setCurrentIndex(static_cast<int>(options.integrator));
    integration->addRow(ui::text("hpop_settings.integration_page"), method);
    auto *vop = new QCheckBox(ui::text("hpop_settings.vop"));
    vop->setObjectName("hpopVop");
    vop->setChecked(options.vop);
    vop->setToolTip(ui::text("hpop_settings.vop.set_tool_tip"));
    integration->addRow(vop);
    auto *step_control = new QComboBox;
    step_control->setObjectName("hpopStepControl");
    step_control->addItems(
        {ui::text("hpop_settings.step_control"), ui::text("hpop_settings.label_41")});
    int step_control_draft = static_cast<int>(options.step_control);
    integration->addRow(ui::text("hpop_settings.integration.add_row"), step_control);

    auto *adaptive_widget = new QWidget;
    auto *adaptive = new QFormLayout(adaptive_widget);
    adaptive->setContentsMargins(0, 0, 0, 0);
    adaptive->setVerticalSpacing(10);
    adaptive->setHorizontalSpacing(12);
    adaptive->setRowWrapPolicy(QFormLayout::WrapLongRows);
    adaptive->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *minimum =
        quantity(adaptive, ui::text("hpop_settings.minimum"), "hpopMinStep", Unit::second,
                 options.min_step_seconds, 1e-6, core::hpop_max_step_seconds);
    auto *maximum =
        quantity(adaptive, ui::text("hpop_settings.maximum"), "hpopMaxStep", Unit::second,
                 options.max_step_seconds, 1e-6, core::hpop_max_step_seconds);
    auto *relative = quantity(adaptive, ui::text("hpop_settings.relative"), "hpopRelativeTolerance",
                              Unit::one, options.relative_tolerance, 1e-14, 1e-3);
    auto *position =
        quantity(adaptive, ui::text("hpop_settings.position"), "hpopPositionTolerance", Unit::metre,
                 options.position_tolerance_m, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    auto *velocity =
        quantity(adaptive, ui::text("hpop_settings.velocity"), "hpopVelocityTolerance",
                 Unit::metres_per_second, options.velocity_tolerance_mps,
                 std::numeric_limits<double>::min(), std::numeric_limits<double>::max());
    integration->addRow(adaptive_widget);
    auto *fixed =
        quantity(integration, ui::text("hpop_settings.fixed"), "hpopFixedStep", Unit::second,
                 options.fixed_step_seconds, 1e-6, core::hpop_max_step_seconds);
    auto *correction = new QComboBox;
    correction->setObjectName("hpopCorrection");
    correction->setToolTip(ui::text("hpop_settings.correction.set_tool_tip"));
    correction->addItems({ui::text("hpop_settings.label_42"), ui::text("hpop_settings.label_43")});
    correction->setCurrentIndex(static_cast<int>(options.correction));
    integration->addRow(ui::text("hpop_settings.integration.add_row_2"), correction);
    auto *regularization_form =
        section(integration_page, ui::text("hpop_settings.regularization_form"),
                "hpopRegularizationSection");
    auto *regularization = new QCheckBox(ui::text("hpop_settings.regularization"));
    regularization->setObjectName("hpopTimeRegularization");
    regularization->setChecked(options.time_regularization);
    regularization_form->addRow(regularization);
    auto *exponent = quantity(
        regularization_form, ui::text("hpop_settings.exponent"), "hpopRegularizationExponent",
        Unit::one, options.regularization_exponent, std::numeric_limits<double>::min(), 10);
    auto *steps_per_orbit = new QSpinBox;
    steps_per_orbit->setObjectName("hpopStepsPerOrbit");
    steps_per_orbit->setRange(1, std::numeric_limits<int>::max());
    steps_per_orbit->setValue(options.steps_per_orbit);
    regularization_form->addRow(ui::text("hpop_settings.regularization_form.add_row"),
                                steps_per_orbit);
    help(regularization_form, ui::text("hpop_settings.label_44"));
    const auto update_step_control = [=] {
        const bool is_fixed = step_control->currentIndex() == 1;
        adaptive_widget->setVisible(!is_fixed);
        integration->setRowVisible(fixed, is_fixed && !regularization->isChecked());
        exponent->setEnabled(regularization->isChecked());
        steps_per_orbit->setEnabled(regularization->isChecked());
        integration->setRowVisible(correction, method->currentIndex() == 4);
    };
    const auto update_method = [=, &step_control_draft] {
        vop->setEnabled(method->currentIndex() == 0 || method->currentIndex() == 3);
        const bool forced = method->currentIndex() == 1 || method->currentIndex() == 4;
        const QSignalBlocker blocker(step_control);
        step_control->setCurrentIndex(forced ? 1 : step_control_draft);
        step_control->setEnabled(!forced);
        update_step_control();
    };
    QObject::connect(method, &QComboBox::currentIndexChanged, &dialog, update_method);
    QObject::connect(regularization, &QCheckBox::toggled, &dialog, update_step_control);
    QObject::connect(step_control, &QComboBox::currentIndexChanged, &dialog,
                     [&, update_step_control](int index) {
                         step_control_draft = index;
                         update_step_control();
                     });
    update_method();
    help(integration, ui::text("hpop_settings.label_45"));
    auto *interpolation_form = section(
        integration_page, ui::text("hpop_settings.interpolation_form"), "hpopInterpolationSection");
    auto *interpolation = new QComboBox;
    interpolation->setObjectName("hpopInterpolation");
    interpolation->addItems({ui::text("hpop_settings.interpolation"),
                             ui::text("hpop_settings.label_46"),
                             ui::text("hpop_settings.label_47")});
    interpolation->setCurrentIndex(static_cast<int>(options.interpolation.method));
    interpolation_form->addRow(ui::text("hpop_settings.interpolation_form.add_row"), interpolation);
    auto *interpolation_order = new QSpinBox;
    interpolation_order->setObjectName("hpopInterpolationOrder");
    interpolation_form->addRow(ui::text("hpop_settings.interpolation_form.add_row_2"),
                               interpolation_order);
    auto *interpolation_mu = quantity(
        interpolation_form, ui::text("hpop_settings.interpolation_mu"), "hpopInterpolationMu",
        Unit::metres_cubed_per_second_squared, options.interpolation.vop_mu,
        std::numeric_limits<double>::min(), std::numeric_limits<double>::max());
    interpolation_mu->set_unit(Unit::kilometres_cubed_per_second_squared);
    std::array<int, 3> interpolation_drafts{7, 7, 7};
    interpolation_drafts[static_cast<std::size_t>(interpolation->currentIndex())] =
        options.interpolation.order;
    const auto update_interpolation = [=, &interpolation_drafts] {
        const QSignalBlocker blocker(interpolation_order);
        const bool hermite = interpolation->currentIndex() == 1;
        interpolation_mu->setEnabled(interpolation->currentIndex() == 2);
        interpolation_order->setRange(hermite ? 3 : 0, 29);
        interpolation_order->setSingleStep(hermite ? 2 : 1);
        interpolation_order->setValue(
            interpolation_drafts[static_cast<std::size_t>(interpolation->currentIndex())]);
    };
    QObject::connect(interpolation, &QComboBox::currentIndexChanged, &dialog, update_interpolation);
    QObject::connect(interpolation_order, &QSpinBox::valueChanged, &dialog, [&](int value) {
        interpolation_drafts[static_cast<std::size_t>(interpolation->currentIndex())] = value;
    });
    update_interpolation();
    auto *fixed_ephemeris = new QCheckBox(ui::text("hpop_settings.fixed_ephemeris"));
    fixed_ephemeris->setObjectName("hpopFixedEphemeris");
    fixed_ephemeris->setChecked(options.report_ephemeris_on_fixed_step);
    interpolation_form->addRow(fixed_ephemeris);
    help(interpolation_form, ui::text("hpop_settings.label_48"));
    auto *stop = section(integration_page, ui::text("hpop_settings.stop"), "hpopStopSection");
    auto *altitude =
        quantity(stop, ui::text("hpop_settings.altitude"), "hpopMinimumAltitude", Unit::metre,
                 options.minimum_altitude_m, 0, std::numeric_limits<double>::max());
    help(stop, ui::text("hpop_settings.label_49"));
    auto *covariance_page =
        page(tabs, ui::text("hpop_settings.covariance_page"), "hpopCovariancePage");
    auto *covariance_editor = new HpopCovarianceEditor(
        covariance ? *covariance : application::HpopCovarianceSettings{}, covariance != nullptr);
    covariance_page->addRow(covariance_editor);
    // Navigation: force models and numerics, each with a live summary of the current draft so
    // the whole configuration reads at a glance.
    struct Destination {
        QListWidgetItem *item;
        int outer, inner;
    };
    std::vector<Destination> destinations;
    const auto heading_item = [&](const QString &text) {
        auto *item = new QListWidgetItem(text, navigation);
        item->setData(heading_role, true);
        item->setFlags(Qt::NoItemFlags);
    };
    const auto entry = [&](const QString &text, int outer, int inner) {
        auto *item = new QListWidgetItem(text, navigation);
        destinations.push_back({item, outer, inner});
        return item;
    };
    heading_item(ui::text("hpop_settings.tabs.add_tab"));
    auto *gravity_item = entry(ui::text("hpop_settings.gravity_page"), 0, 0);
    auto *drag_item = entry(ui::text("hpop_settings.drag_page"), 0, 1);
    auto *srp_item = entry(ui::text("hpop_settings.srp_page"), 0, 2);
    auto *other_item = entry(ui::text("hpop_settings.additional_page"), 0, 3);
    heading_item(ui::text("hpop_settings.other_item"));
    auto *integrator_item = entry(ui::text("hpop_settings.integration_page"), 1, -1);
    auto *covariance_item = entry(ui::text("hpop_settings.covariance_page"), 2, -1);
    const auto value_text = [](QuantityEditor *field) {
        const auto *input = field->findChild<QLineEdit *>("quantityValue");
        return input ? input->text() : QString{};
    };
    const auto part = [](const QComboBox *box, int index) {
        const auto parts = box->currentText().split(QStringLiteral(" · "));
        return parts.value(std::min<int>(index, static_cast<int>(parts.size()) - 1));
    };
    const auto refresh_summaries = [&] {
        auto gravity_text =
            model->currentIndex() == 1
                ? ui::text("hpop_settings.gravity_text")
                : part(model, 0) +
                      QStringLiteral(" · %1 × %2").arg(degree->value()).arg(order->value());
        const auto bodies = std::count_if(fields.begin(), fields.end(),
                                          [](const auto &row) { return row.enabled->isChecked(); });
        if (bodies > 0)
            gravity_text += ui::text("hpop_settings.label_50").arg(bodies);
        if (secular_use->isChecked() && model->currentIndex() != 1)
            gravity_text += ui::text("hpop_settings.label_51");
        if (solid_mode->currentIndex() != 0 &&
            (model->currentIndex() != 1 || !solid_truncate->isChecked()))
            gravity_text += QStringLiteral(" · ") + solid_mode->currentText();
        gravity_item->setData(summary_role, gravity_text);
        drag_item->setData(summary_role, drag_use->isChecked()
                                             ? density_model->currentText() +
                                                   QStringLiteral(" · Cd ") + value_text(drag_cd)
                                             : ui::text("common.disabled"));
        srp_item->setData(summary_role,
                          srp_use->isChecked()
                              ? part(srp_shadow, 1) + QStringLiteral(" · Cr ") + value_text(srp_cr)
                              : ui::text("common.disabled"));
        other_item->setData(
            summary_role,
            value_text(satellite_mass) + QStringLiteral(" ") + unit_symbol(satellite_mass->unit()) +
                (relativity->isChecked() ? ui::text("hpop_settings.label_52") : QString{}));
        const QString integration_name =
            part(method, 0) +
            (vop->isEnabled() && vop->isChecked() ? QStringLiteral(" · VOP") : QString{});
        integrator_item->setData(
            summary_role,
            regularization->isChecked()
                ? integration_name +
                      ui::text("hpop_settings.ator_item.set_data").arg(steps_per_orbit->value())
                : (step_control->currentIndex() == 0
                       ? integration_name + ui::text("hpop_settings.label_53") +
                             value_text(relative)
                       : integration_name + ui::text("hpop_settings.label_54") + value_text(fixed) +
                             QStringLiteral(" s")));
        covariance_item->setData(summary_role, covariance_editor->summary());
    };
    QObject::connect(secular_use, &QCheckBox::toggled, &dialog, refresh_summaries);
    QObject::connect(covariance_editor->findChild<QCheckBox *>("hpopComputeCovariance"),
                     &QCheckBox::toggled, &dialog, refresh_summaries);
    QObject::connect(covariance_editor->findChild<QComboBox *>("hpopCovarianceFrame"),
                     &QComboBox::currentIndexChanged, &dialog, refresh_summaries);
    for (auto *box : {model, density_model, srp_shadow, method, step_control, solid_mode})
        QObject::connect(box, &QComboBox::currentIndexChanged, &dialog, refresh_summaries);
    for (auto *spin : {degree, order, steps_per_orbit})
        QObject::connect(spin, &QSpinBox::valueChanged, &dialog, refresh_summaries);
    for (auto *check : {drag_use, srp_use, solid_truncate, relativity, regularization, vop})
        QObject::connect(check, &QCheckBox::toggled, &dialog, refresh_summaries);
    for (auto &row : fields)
        QObject::connect(row.enabled, &QCheckBox::toggled, &dialog, refresh_summaries);
    for (auto *field : {drag_cd, srp_cr, relative, fixed, satellite_mass})
        QObject::connect(field->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged,
                         &dialog, refresh_summaries);
    QObject::connect(satellite_mass, &QuantityEditor::unitChanged, &dialog, refresh_summaries);
    refresh_summaries();
    QObject::connect(navigation, &QListWidget::currentItemChanged, &dialog,
                     [&](QListWidgetItem *item) {
                         for (const auto &destination : destinations)
                             if (destination.item == item) {
                                 tabs->setCurrentIndex(destination.outer);
                                 if (destination.inner >= 0)
                                     force_tabs->setCurrentIndex(destination.inner);
                             }
                     });
    const auto sync_navigation = [&] {
        // Values set programmatically do not emit edits; refresh whenever the page changes.
        refresh_summaries();
        for (const auto &destination : destinations)
            if (destination.outer == tabs->currentIndex() &&
                (destination.inner < 0 || destination.inner == force_tabs->currentIndex())) {
                const QSignalBlocker block(navigation);
                navigation->setCurrentItem(destination.item);
            }
    };
    QObject::connect(tabs, &QTabWidget::currentChanged, &dialog, sync_navigation);
    QObject::connect(force_tabs, &QTabWidget::currentChanged, &dialog, sync_navigation);
    sync_navigation();
    // Narrow windows give the width to the fields and fall back to the tab bars.
    new WidthObserver(&dialog,
                      [navigation, tabs, force_tabs, mode = WidthMode{}](int width) mutable {
                          const bool narrow = mode.narrow(width, 620);
                          navigation->setVisible(!narrow);
                          tabs->tabBar()->setVisible(narrow);
                          force_tabs->tabBar()->setVisible(narrow);
                      });
    align_form_labels(&dialog);
    auto *error = new QLabel;
    error->setObjectName("errorText");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(ui::text("common.ok"));
    buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(ui::text("common.cancel"));
    layout->addWidget(buttons);
    std::optional<DataProduct> queued_gravity, owned_gravity;
    bool queued_ocean = false, owned_ocean = false;
    QString ocean_message;
    QString gravity_message;
    auto catalog_revision = data ? data->revision() : 0;
    const auto selected_product = [&]() -> std::optional<DataProduct> {
        const auto id = model->currentData().toString();
        if (id == "egm2008")
            return DataProduct::egm2008;
        if (id == "egm96")
            return DataProduct::egm96;
        return {};
    };
    const auto selected_model = [&] {
        return std::find_if(installed_models.begin(), installed_models.end(), [&](const auto &v) {
            return v.selection == model->currentData().toString().toStdString();
        });
    };
    const auto refresh_gravity_status = [&] {
        const auto product = selected_product();
        const auto found = selected_model();
        const bool secular_ready =
            found != installed_models.end() && found->info.secular_variations.has_value();
        secular_use->setEnabled(model->currentIndex() != 1 &&
                                (secular_ready || secular_use->isChecked()));
        if (model->currentIndex() == 1) {
            secular_status->setText(ui::text("hpop_settings.secular_status.set_text"));
        } else if (secular_ready) {
            const auto &variation = *found->info.secular_variations;
            const double jd =
                2451545.0 +
                (variation.reference_epoch.seconds_since(core::Epoch{0}) + 32.184) / 86400;
            secular_status->setText(ui::text("hpop_settings.secular_status.set_text_2")
                                        .arg(jd, 0, 'f', 9)
                                        .arg(variation.rates_per_year.size()));
        } else {
            secular_status->setText(ui::text("hpop_settings.secular_status.set_text_3"));
        }
        solid_k20->setText(
            found != installed_models.end() ? QString::number(found->info.solid_tide_k20, 'g', 15)
            : model->currentData().toString().isEmpty() ? QStringLiteral("0.30190")
                                                        : ui::text("hpop_settings.label_55"));
        const bool ready =
            model->currentData().toString().isEmpty() || found != installed_models.end();
        const bool running =
            data && product && data->downloading() && data->active_product() == *product;
        const bool waiting = product && queued_gravity == product;
        gravity_progress->setVisible(running);
        if (running) {
            const auto received = data->received_bytes(), total = data->total_bytes();
            // Once all bytes arrive, validation still has to finish before publication.
            if (total > 0 && received < total) {
                gravity_progress->setRange(0, 1000);
                gravity_progress->setValue(static_cast<int>(1000.0 * received / total));
            } else {
                gravity_progress->setRange(0, 0);
            }
            gravity_status->setText(
                ui::text("hpop_settings.gravity_status.set_text")
                    .arg(data->status())
                    .arg(received / (1024.0 * 1024), 0, 'f', 1)
                    .arg(total > 0
                             ? QStringLiteral(" / %1 MiB").arg(total / (1024.0 * 1024), 0, 'f', 1)
                             : QString{}));
        } else if (waiting) {
            gravity_status->setText(ui::text("hpop_settings.gravity_status.set_text_2"));
        } else if (found != installed_models.end()) {
            gravity_status->setText(ui::text("hpop_settings.gravity_status.set_text_3")
                                        .arg(found->info.degree)
                                        .arg(found->info.order));
        } else if (model->currentIndex() == 0) {
            gravity_status->setText(ui::text("hpop_settings.gravity_status.set_text_4"));
        } else if (model->currentIndex() == 1) {
            gravity_status->setText(ui::text("hpop_settings.gravity_status.set_text_5"));
        } else {
            const auto message = !gravity_message.isEmpty() ? gravity_message
                                 : data && product          ? data->update_status(*product).message
                                                            : QString{};
            gravity_status->setText(
                message.isEmpty() ? (product ? ui::text("hpop_settings.gravity_status.set_text_6")
                                             : ui::text("hpop_settings.label_56"))
                                  : message + ui::text("hpop_settings.label_57"));
        }
        download_gravity->setVisible(data && product && !ready && !running && !waiting);
        download_gravity->setText(ui::text("hpop_settings.ocean_download"));
        cancel_gravity->setVisible(queued_gravity.has_value() ||
                                   (data && owned_gravity && data->downloading() &&
                                    data->active_product() == *owned_gravity));
        gravity_actions->setVisible(!download_gravity->isHidden() || !cancel_gravity->isHidden());
        const bool ocean_missing =
            std::none_of(installed_oceans.begin(), installed_oceans.end(), [&](const auto &entry) {
                return QString::fromStdString(entry.selection) == ocean_model->currentData();
            });
        const bool ocean_waiting =
            ocean_use->isChecked() && ocean_degree->value() >= 2 && ocean_missing &&
            ocean_model->currentData().toString() == "fes2014bv1" &&
            (queued_ocean ||
             (data && data->downloading() && data->active_product() == DataProduct::fes2014bv1));
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled((ready || (!running && !waiting)) && !ocean_waiting);
    };
    const auto request_gravity = [&] {
        const auto product = selected_product();
        queued_gravity.reset();
        gravity_message.clear();
        if (!data || !product || selected_model() != installed_models.end()) {
            refresh_gravity_status();
            return;
        }
        if (data->downloading()) {
            if (data->active_product() != *product)
                queued_gravity = product;
        } else {
            data->start_download({}, *product);
            if (data->downloading())
                owned_gravity = product;
        }
        refresh_gravity_status();
    };
    QObject::connect(model, &QComboBox::currentIndexChanged, &dialog, request_gravity);
    QObject::connect(download_gravity, &QPushButton::clicked, &dialog, request_gravity);
    QObject::connect(secular_use, &QCheckBox::toggled, &dialog, refresh_gravity_status);
    QObject::connect(cancel_gravity, &QPushButton::clicked, &dialog, [&] {
        queued_gravity.reset();
        gravity_message = ui::text("hpop_settings.label_58");
        if (data && owned_gravity && data->downloading() &&
            data->active_product() == *owned_gravity)
            data->cancel_download();
        refresh_gravity_status();
    });
    const auto selected_ocean = [&] {
        return std::find_if(installed_oceans.begin(), installed_oceans.end(), [&](const auto &v) {
            return v.selection == ocean_model->currentData().toString().toStdString();
        });
    };
    const auto refresh_ocean_status = [&] {
        const bool enabled = ocean_use->isChecked(), active = enabled && ocean_degree->value() >= 2;
        ocean_model->setEnabled(enabled);
        ocean_degree->setEnabled(enabled);
        ocean_order->setEnabled(enabled);
        const bool running = ocean_model->currentData().toString() == "fes2014bv1" && data &&
                             data->downloading() &&
                             data->active_product() == DataProduct::fes2014bv1;
        const auto found = selected_ocean();
        const bool ready = found != installed_oceans.end();
        ocean_status->setVisible(enabled);
        ocean_status->setText(!active ? ui::text("hpop_settings.ocean_status.set_text")
                              : ready ? ui::text("hpop_settings.ocean_status.set_text_2")
                                            .arg(found->info.degree)
                                            .arg(found->info.order)
                                            .arg(found->info.constituents)
                              : queued_ocean             ? ui::text("hpop_settings.label_59")
                              : running                  ? data->status()
                              : !ocean_message.isEmpty() ? ocean_message
                                                         : ui::text("hpop_settings.label_60"));
        ocean_progress->setVisible(active && running);
        if (running && data->total_bytes() > 0) {
            ocean_progress->setRange(0, 1000);
            ocean_progress->setValue(static_cast<int>(
                std::min<qint64>(1000, data->received_bytes() * 1000 / data->total_bytes())));
        } else {
            ocean_progress->setRange(0, 0);
        }
        ocean_download->setVisible(data && active && !ready && !running && !queued_ocean &&
                                   ocean_model->currentData().toString() == "fes2014bv1");
        ocean_cancel->setVisible(queued_ocean || (owned_ocean && running));
        refresh_gravity_status();
    };
    const auto request_ocean = [&] {
        queued_ocean = false;
        ocean_message.clear();
        const bool needs_download = ocean_use->isChecked() && ocean_degree->value() >= 2 &&
                                    ocean_model->currentData().toString() == "fes2014bv1" &&
                                    selected_ocean() == installed_oceans.end();
        if (!needs_download && data && owned_ocean && data->downloading() &&
            data->active_product() == DataProduct::fes2014bv1)
            data->cancel_download();
        if (needs_download && data) {
            if (data->downloading()) {
                queued_ocean = data->active_product() != DataProduct::fes2014bv1;
            } else {
                data->start_download({}, DataProduct::fes2014bv1);
                owned_ocean = data->downloading();
            }
        }
        refresh_ocean_status();
    };
    QObject::connect(ocean_use, &QCheckBox::toggled, &dialog, request_ocean);
    QObject::connect(ocean_model, &QComboBox::currentIndexChanged, &dialog, request_ocean);
    QObject::connect(ocean_degree, &QSpinBox::valueChanged, &dialog, request_ocean);
    QObject::connect(ocean_download, &QPushButton::clicked, &dialog, request_ocean);
    QObject::connect(ocean_cancel, &QPushButton::clicked, &dialog, [&] {
        queued_ocean = false;
        ocean_message = ui::text("hpop_settings.label_61");
        if (data && owned_ocean && data->downloading() &&
            data->active_product() == DataProduct::fes2014bv1)
            data->cancel_download();
        refresh_ocean_status();
    });
    QObject::connect(ocean_manage, &QPushButton::clicked, &dialog, [&] {
        if (data)
            show_data_center(&dialog, *data,
                             ocean_model->currentData().toString() == "fes2014bv1"
                                 ? DataProduct::fes2014bv1
                                 : DataProduct::custom_ocean_tides);
    });
    QTimer gravity_poll;
    QObject::connect(&gravity_poll, &QTimer::timeout, &dialog, [&] {
        if (data && data->revision() != catalog_revision) {
            catalog_revision = data->revision();
            installed_models = data->data().gravity_models();
            installed_ephemerides = data->data().ephemerides();
            installed_oceans = data->data().ocean_tide_models();
            refresh_ocean_models();
            refresh_ephemeris();
            refresh_srp_resource();
            refresh_drag_resource();
            refresh_models();
            update_gravity();
        }
        if (data && !data->downloading()) {
            owned_gravity.reset();
            if (owned_ocean) {
                owned_ocean = false;
                if (selected_ocean() == installed_oceans.end())
                    ocean_message = data->update_status(DataProduct::fes2014bv1).message;
            }
            if (queued_gravity)
                request_gravity();
            if (queued_ocean && !data->downloading())
                request_ocean();
        }
        refresh_ocean_status();
    });
    if (data)
        gravity_poll.start(100);
    refresh_gravity_status();
    QTimer::singleShot(0, &dialog, request_gravity);
    refresh_ocean_status();
    QTimer::singleShot(0, &dialog, request_ocean);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        int validation_page = 0;
        int validation_force_page = 0;
        std::optional<core::ThirdBody> validation_body;
        QWidget *validation_field = model;
        const auto read = [&](QuantityEditor *field, Unit unit) {
            validation_field = field;
            return field->value(unit);
        };
        const auto read_draft = [&](QuantityEditor *field, Unit unit, auto &value, bool active) {
            if (active)
                value = read(field, unit);
            else {
                try {
                    value = field->value(unit);
                } catch (const std::exception &) {
                    // An unfinished inactive input must not replace the saved valid draft.
                }
            }
        };
        try {
            auto candidate = options;
            for (auto body : core::third_body_list) {
                const auto &field = fields[static_cast<std::size_t>(body)];
                validation_body = body;
                validation_field = field.source;
                auto &setting = candidate.third_body(body);
                setting.enabled = field.enabled->isChecked();
                setting.source =
                    static_cast<core::ThirdBodyGravitySource>(field.source->currentIndex());
                setting.cb = *field.cb;
                if (setting.enabled && setting.source == core::ThirdBodyGravitySource::cb_file &&
                    !setting.cb)
                    throw std::runtime_error(std::string(core::third_body_name(body)) +
                                             ui::utf8("hpop_settings.validation"));
                if (setting.source == core::ThirdBodyGravitySource::user_specified)
                    read_draft(field.mu, Unit::metres_cubed_per_second_squared, setting.user_mu,
                               setting.enabled);
                else if (field.manual->si &&
                         (setting.user_mu || field.manual->text != field.default_manual))
                    setting.user_mu = field.manual->si;
            }
            validation_body.reset();
            validation_field = model;
            candidate.gravity_model = model->currentData().toString().toStdString();
            candidate.gravity_mu = core::egm96_mu;
            if (!candidate.gravity_model.empty()) {
                const auto found = std::find_if(
                    installed_models.begin(), installed_models.end(),
                    [&](const auto &entry) { return entry.selection == candidate.gravity_model; });
                if (found == installed_models.end())
                    throw std::runtime_error(ui::utf8("hpop_settings.validation_2"));
                candidate.gravity_mu = found->info.mu;
                if (order->value() > found->info.order) {
                    validation_field = order;
                    throw std::runtime_error(ui::utf8("hpop_settings.validation_3"));
                }
            }
            candidate.gravity_degree = model->currentIndex() != 1 ? degree->value() : 0;
            candidate.gravity_order = model->currentIndex() != 1 ? order->value() : 0;
            candidate.include_secular_variations = secular_use->isChecked();
            validation_field = secular_use;
            if (candidate.uses_secular_gravity()) {
                const auto found = selected_model();
                if (found == installed_models.end() || !found->info.secular_variations)
                    throw std::runtime_error(ui::utf8("hpop_settings.validation_4"));
            }
            candidate.solid_tides.mode =
                static_cast<core::SolidTideMode>(solid_mode->currentIndex());
            candidate.solid_tides.truncate_to_gravity = solid_truncate->isChecked();
            candidate.solid_tides.include_time_dependent_terms = solid_time->isChecked();
            candidate.ocean_tides = {ocean_use->isChecked(),
                                     ocean_model->currentData().toString().toStdString(),
                                     ocean_degree->value(), ocean_order->value()};
            validation_field = ocean_model;
            if (candidate.ocean_tides.active()) {
                const auto found = selected_ocean();
                if (found == installed_oceans.end())
                    throw std::runtime_error(ui::utf8("hpop_settings.found"));
                validation_field = ocean_degree;
                if (candidate.ocean_tides.degree > found->info.degree ||
                    candidate.ocean_tides.order > found->info.order)
                    throw std::runtime_error(ui::utf8("hpop_settings.validation_5"));
            }
            validation_field = solid_mode;
            if (candidate.uses_full_solid_tides() &&
                std::none_of(installed_ephemerides.begin(), installed_ephemerides.end(),
                             [&](const auto &entry) {
                                 return entry.selection == candidate.ephemeris_model;
                             }))
                throw std::runtime_error(ui::utf8("hpop_settings.validation_6"));
            if (candidate.uses_earth_fixed_gravity()) {
                for (const auto &entry : installed_models)
                    if (entry.selection == candidate.gravity_model &&
                        entry.info.tide_system == "mean_tide")
                        throw std::runtime_error(ui::utf8("hpop_settings.validation_7"));
            }
            validation_force_page = 1;
            auto &drag = candidate.drag;
            drag.enabled = drag_use->isChecked();
            drag.atmosphere = static_cast<core::AtmosphereModel>(density_model->currentIndex());
            drag.low_altitude_model =
                static_cast<core::LowAltitudeDensityModel>(low_density->currentIndex());
            drag.use_apparent_sun_position = drag_apparent->isChecked();
            drag.use_approximate_altitude = drag_approximate->isChecked();
            drag.weather_model = drag_weather_source->currentIndex() == 1
                                     ? weather_file->currentData().toString().toStdString()
                                     : std::string{};
            drag.geomagnetic_update =
                static_cast<core::GeomagneticUpdate>(weather_update->currentIndex());
            drag.geomagnetic_source =
                static_cast<core::GeomagneticSource>(weather_source->currentIndex());
            const auto drag_value = [&](QuantityEditor *field, Unit unit, double &value,
                                        bool used) {
                if (used)
                    value = read(field, unit);
                else {
                    try {
                        value = field->value(unit);
                    } catch (const std::exception &) {
                    }
                }
            };
            drag_value(drag_cd, Unit::one, drag.cd, drag.enabled);
            drag_value(drag_area, Unit::square_metres_per_kilogram, drag.area_mass_ratio_m2_per_kg,
                       drag.enabled);
            drag_value(drag_f107, Unit::one, drag.f107, drag.enabled && drag.weather_model.empty());
            drag_value(drag_average, Unit::one, drag.f107_average,
                       drag.enabled && drag.weather_model.empty());
            drag_value(drag_kp, Unit::one, drag.kp, drag.enabled && drag.weather_model.empty());
            const bool blend = drag.enabled &&
                               drag.atmosphere == core::AtmosphereModel::jacchia_roberts &&
                               drag.low_altitude_model == core::LowAltitudeDensityModel::nrlmsise00;
            drag_value(drag_blend, Unit::metre, drag.blending_range_m, blend);
            validation_field = blend ? static_cast<QWidget *>(drag_blend) : drag_use;
            core::validate_drag_options(drag);
            if (drag.enabled && !std::any_of(installed_ephemerides.begin(),
                                             installed_ephemerides.end(), [&](const auto &entry) {
                                                 return entry.selection ==
                                                        candidate.ephemeris_model;
                                             }))
                throw std::runtime_error(ui::utf8("hpop_settings.validation_8"));
            if (drag.enabled && !drag.weather_model.empty()) {
                validation_field = weather_file;
                const auto available = data ? data->data().weather_sources()
                                            : std::vector<runtime::CatalogSpaceWeather>{};
                if (std::none_of(available.begin(), available.end(), [&](const auto &entry) {
                        return entry.selection == drag.weather_model;
                    }))
                    throw std::runtime_error(ui::utf8("hpop_settings.validation_9"));
            }
            validation_force_page = 2;
            auto &srp = candidate.srp;
            srp.enabled = srp_use->isChecked();
            srp.boundary_mitigation = srp_boundary->isChecked();
            srp.shadow_model = static_cast<core::SrpShadowModel>(srp_shadow->currentIndex());
            srp.sun_position_method =
                static_cast<core::SrpSunPositionMethod>(srp_sun_method->currentIndex());
            srp.earth_shadow = srp_assignments.earth_shadow;
            srp.moon_shadow = srp_assignments.moon_shadow;
            srp.additional_eclipsing_bodies = srp_assignments.additional_eclipsing_bodies;
            const auto srp_value = [&](QuantityEditor *field, Unit unit, double &value, bool used) {
                if (used)
                    value = read(field, unit);
                else {
                    try {
                        value = field->value(unit);
                    } catch (const std::exception &) {
                    }
                }
            };
            srp_value(srp_cr, Unit::one, srp.cr, srp.enabled);
            srp_value(srp_area, Unit::square_metres_per_kilogram, srp.area_mass_ratio_m2_per_kg,
                      srp.enabled);
            srp_value(srp_pressure, Unit::pascal, srp.pressure_at_reference_pa, srp.enabled);
            srp_value(srp_distance, Unit::metre, srp.reference_distance_m, srp.enabled);
            srp_value(srp_sun_radius, Unit::metre, srp.sun_radius_m, srp.enabled);
            const bool shadows = srp.enabled && srp.shadow_model != core::SrpShadowModel::none;
            srp_value(srp_altitude, Unit::metre, srp.atmospheric_altitude_m,
                      shadows && srp.earth_shadow);
            srp_value(srp_earth_radius, Unit::metre, srp.earth_radius_m,
                      shadows && srp.earth_shadow);
            srp_value(srp_moon_radius, Unit::metre, srp.moon_radius_m, shadows && srp.moon_shadow);
            validation_field = srp_use;
            core::validate_srp_options(srp);
            if (srp.enabled && !std::any_of(installed_ephemerides.begin(),
                                            installed_ephemerides.end(), [&](const auto &entry) {
                                                return entry.selection == candidate.ephemeris_model;
                                            }))
                throw std::runtime_error(ui::utf8("hpop_settings.validation_10"));
            validation_force_page = 3;
            validation_field = satellite_mass;
            candidate.satellite_mass_kg = read(satellite_mass, Unit::kilogram);
            (void)candidate.drag_area_m2();
            (void)candidate.srp_area_m2();
            validation_field = relativity;
            candidate.include_relativistic_accelerations = relativity->isChecked();
            if (candidate.include_relativistic_accelerations &&
                std::none_of(installed_ephemerides.begin(), installed_ephemerides.end(),
                             [&](const auto &entry) {
                                 return entry.selection == candidate.ephemeris_model;
                             }))
                throw std::runtime_error(ui::utf8("hpop_settings.validation_11"));
            candidate.integrator = static_cast<core::HpopIntegrator>(method->currentIndex());
            candidate.vop = vop->isChecked();
            candidate.correction = static_cast<core::HpopCorrection>(correction->currentIndex());
            candidate.step_control = static_cast<core::HpopStepControl>(step_control_draft);
            validation_page = 1;
            candidate.time_regularization = regularization->isChecked();
            read_draft(exponent, Unit::one, candidate.regularization_exponent,
                       candidate.time_regularization);
            candidate.steps_per_orbit = steps_per_orbit->value();
            candidate.minimum_altitude_m = read(altitude, Unit::metre);
            const bool adaptive = !candidate.uses_fixed_step();
            const auto read_bounds = [&] {
                const double low = read(minimum, Unit::second);
                const double high = read(maximum, Unit::second);
                if (low > high)
                    throw std::runtime_error(ui::utf8("hpop_settings.validation_12"));
                // Bounds form one draft: never save only one side of an invalid pair.
                candidate.min_step_seconds = low;
                candidate.max_step_seconds = high;
            };
            if (adaptive)
                read_bounds();
            else {
                try {
                    read_bounds();
                } catch (const std::exception &) {
                }
            }
            read_draft(relative, Unit::one, candidate.relative_tolerance, adaptive);
            read_draft(position, Unit::metre, candidate.position_tolerance_m, adaptive);
            read_draft(velocity, Unit::metres_per_second, candidate.velocity_tolerance_mps,
                       adaptive);
            read_draft(fixed, Unit::second, candidate.fixed_step_seconds,
                       !adaptive && !candidate.time_regularization);
            read_draft(interpolation_mu, Unit::metres_cubed_per_second_squared,
                       candidate.interpolation.vop_mu, interpolation->currentIndex() == 2);
            validation_field = interpolation_order;
            candidate.interpolation = {
                static_cast<core::OrbitInterpolationMethod>(interpolation->currentIndex()),
                interpolation_order->value(), candidate.interpolation.vop_mu};
            candidate.report_ephemeris_on_fixed_step = fixed_ephemeris->isChecked();
            core::validate_hpop_options(candidate);
            validation_page = 2;
            validation_field = covariance_editor->findChild<QTableWidget *>("hpopCovarianceMatrix");
            const auto covariance_candidate = covariance_editor->value();
            if (covariance) {
                application::validate_hpop_covariance_settings(covariance_candidate, candidate);
                if (covariance_candidate.enabled && covariance_candidate.independent_gravity) {
                    const auto [max_degree, max_order] = gravity_limits();
                    validation_field =
                        covariance_editor->findChild<QSpinBox *>("hpopCovarianceDegree");
                    if (covariance_candidate.gravity.degree > max_degree)
                        throw std::runtime_error(ui::utf8("hpop_settings.validation_13"));
                    validation_field =
                        covariance_editor->findChild<QSpinBox *>("hpopCovarianceOrder");
                    if (covariance_candidate.gravity.order > max_order)
                        throw std::runtime_error(ui::utf8("hpop_settings.validation_14"));
                }
            }
            options = candidate;
            if (covariance)
                *covariance = covariance_candidate;
            dialog.accept();
        } catch (const std::exception &e) {
            tabs->setCurrentIndex(validation_page);
            auto *scroll = dialog.findChild<QScrollArea *>(
                validation_page == 0   ? (validation_force_page == 3   ? "hpopAdditionalPage"
                                          : validation_force_page == 2 ? "hpopSrpPage"
                                          : validation_force_page == 1 ? "hpopDragPage"
                                                                       : "hpopGravityPage")
                : validation_page == 2 ? "hpopCovariancePage"
                                       : "hpopIntegratorPage");
            if (validation_page == 0) {
                force_tabs->setCurrentIndex(validation_force_page);
                if (validation_body) {
                    search->clear();
                    const auto row = static_cast<int>(*validation_body);
                    table->selectRow(row);
                    table->scrollToItem(table->item(row, 0));
                }
            }
            error->setText(QString::fromUtf8(e.what()));
            error->show();
            reveal(validation_field);
            if (scroll)
                scroll->ensureWidgetVisible(validation_field);
            validation_field->setFocus(Qt::OtherFocusReason);
        }
    });
    const auto available = dialog.screen()->availableGeometry().size();
    dialog.resize(std::min(720, available.width() - 32), std::min(700, available.height() - 64));
    const bool accepted = dialog.exec() == QDialog::Accepted;
    gravity_poll.stop();
    // Cancel only work this dialog started; shared data-center tasks keep their owner.
    if (data && owned_gravity && data->downloading() && data->active_product() == *owned_gravity)
        data->cancel_download();
    if (data && owned_ocean && data->downloading() &&
        data->active_product() == DataProduct::fes2014bv1)
        data->cancel_download();
    return accepted;
}
} // namespace orbitforge::desktop
