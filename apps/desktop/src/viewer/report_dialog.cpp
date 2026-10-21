#include "report_dialog.hpp"
#include "../ui_text.hpp"
#include "coverage_map.hpp"
#include "epoch_editor.hpp"
#include "object_selection.hpp"
#include "report_plot.hpp"
#include "report_selection.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QAbstractTableModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTabBar>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <future>
#include <map>
#include <numbers>
#include <orbitforge/project.hpp>

namespace orbitforge::desktop {
namespace {
using namespace application;
bool same_time(const std::shared_ptr<const core::ITimeProvider> &a,
               const std::shared_ptr<const core::ITimeProvider> &b) {
    return a && b ? time_data_lock(a->data_info()) == time_data_lock(b->data_info())
                  : bool(a) == bool(b);
}
auto analysis_reuse_key(application::Project project) {
    // Keep all inputs and result labels, including mission segment names. Only
    // navigation, saved report templates and appearance are irrelevant to queries.
    project.current = project.first;
    project.reports.clear();
    for (auto &satellite : project.satellites) {
        satellite.display = {};
        satellite.model_kind = 0;
        satellite.model_path.clear();
        satellite.model_diameter_m = 1'000'000;
        satellite.orbit.input_frame = core::FrameId::gcrf;
    }
    return application::encode_project(project);
}
constexpr auto builtin_count = static_cast<int>(report_kinds.size());
std::vector<ReportColumnSpec> default_columns(ReportKind kind) {
    std::vector<ReportColumnSpec> selected;
    for (const auto &column : report_columns(kind)) {
        const auto &id = column.id;
        if (id == "object.id" || id == "lighting.start_clipped" || id == "lighting.stop_clipped" ||
            ((kind == ReportKind::access_intervals || kind == ReportKind::intersatellite_access) &&
             (id == "object.name" || id == "access.start_clipped" || id == "access.stop_clipped" ||
              id == "access.max_elevation_time" || id == "access.min_range_time")))
            continue;
        selected.push_back({id, {}, {}});
    }
    return selected;
}
std::shared_ptr<runtime::ReportResult>
complete_plot_result(const runtime::ReportResult &result,
                     const std::map<std::string, ReportColumnSpec> &retained_specs) {
    // Table storage is immutable and shared. Plotting keeps all source fields without
    // duplicating rows, statistics or numerical sessions when the table hides columns.
    auto complete = std::make_shared<runtime::ReportResult>(
        runtime::ReportResult{.definition = result.definition,
                              .table = result.source_table ? *result.source_table : result.table,
                              .first = result.first,
                              .last = result.last,
                              .time = result.time,
                              .source_table = result.source_table});
    for (const auto &[field, spec] : retained_specs)
        if (std::ranges::find(complete->definition.format.columns, field,
                              &ReportColumnSpec::field) ==
            complete->definition.format.columns.end())
            complete->definition.format.columns.push_back(spec);
    return complete;
}
class ReportSelectionButton final : public QPushButton {
  public:
    using QPushButton::QPushButton;
    QSize sizeHint() const override { return {240, QPushButton::sizeHint().height()}; }
    QSize minimumSizeHint() const override {
        return {120, QPushButton::minimumSizeHint().height()};
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QStyleOptionButton option;
        initStyleOption(&option);
        option.text = fontMetrics().elidedText(text(), Qt::ElideRight, std::max(0, width() - 70));
        QStylePainter painter(this);
        painter.drawControl(QStyle::CE_PushButton, option);
        workbench_icon(WorkbenchIcon::more)
            .paint(&painter, QRect(width() - 26, (height() - 16) / 2, 16, 16), Qt::AlignCenter,
                   isEnabled() ? QIcon::Normal : QIcon::Disabled);
    }
};
class ReportTitle final : public QLabel {
  public:
    using QLabel::QLabel;

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};
class StepSpinBox final : public QDoubleSpinBox {
  public:
    using QDoubleSpinBox::QDoubleSpinBox;

  protected:
    QString textFromValue(double value) const override {
        auto display = locale();
        display.setNumberOptions(QLocale::OmitGroupSeparator);
        auto value_text = display.toString(value, 'f', 9);
        while (value_text.endsWith(display.zeroDigit()))
            value_text.chop(display.zeroDigit().size());
        if (value_text.endsWith(display.decimalPoint()))
            value_text.chop(display.decimalPoint().size());
        return value_text;
    }
};
QString text(const std::string &value) { return QString::fromStdString(value); }
bool pair_report(ReportKind kind) {
    return report_is_access(kind) || report_is_intersatellite(kind);
}
QString kind_name(ReportKind kind) {
    switch (kind) {
    case ReportKind::scenario_summary:
        return ui::text("report_dialog.label");
    case ReportKind::object_summary:
        return ui::text("report_dialog.label_2");
    case ReportKind::cartesian_state:
        return ui::text("report_dialog.label_3");
    case ReportKind::ground_stations:
        return ui::text("report_dialog.label_4");
    case ReportKind::classical_elements:
        return ui::text("report_dialog.label_5");
    case ReportKind::geodetic_position:
        return ui::text("report_dialog.label_6");
    case ReportKind::orbital_motion:
        return ui::text("report_dialog.label_7");
    case ReportKind::access_intervals:
        return ui::text("report_dialog.label_8");
    case ReportKind::aer:
        return ui::text("report_dialog.label_9");
    case ReportKind::lighting_intervals:
        return ui::text("report_dialog.label_10");
    case ReportKind::lighting_state:
        return ui::text("report_dialog.label_11");
    case ReportKind::orbital_events:
        return ui::text("report_dialog.label_12");
    case ReportKind::relative_motion:
        return ui::text("report_dialog.label_13");
    case ReportKind::intersatellite_access:
        return ui::text("report_dialog.label_14");
    case ReportKind::coverage_grid:
        return ui::text("report_dialog.label_15");
    case ReportKind::coverage_time:
        return ui::text("report_dialog.label_16");
    case ReportKind::state_covariance:
        return ui::text("report_dialog.label_17");
    }
    return {};
}
QString localized(QString value) {
    static const std::map<QString, QString> labels{
        {"Latitude", ui::text("report_dialog.label_18")},
        {"Cell area", ui::text("report_dialog.label_19")},
        {"Time covered", ui::text("report_dialog.label_20")},
        {"Time covered fraction", ui::text("report_dialog.label_21")},
        {"Coverage intervals", ui::text("report_dialog.label_22")},
        {"Maximum revisit time", ui::text("report_dialog.label_23")},
        {"Mean revisit time", ui::text("report_dialog.label_24")},
        {"Area covered", ui::text("report_dialog.label_25")},
        {"Area covered fraction", ui::text("report_dialog.label_26")},
        {"Cumulative area covered", ui::text("report_dialog.label_27")},
        {"Cumulative area covered fraction", ui::text("report_dialog.label_28")},
        {"Property", ui::text("common.properties")},
        {"Value", ui::text("report_dialog.label_29")},
        {"Object ID", ui::text("report_dialog.label_30")},
        {"Object", ui::text("common.objects")},
        {"Type", ui::text("report_dialog.label_31")},
        {"Model", ui::text("report_dialog.label_32")},
        {"Time", ui::text("common.time")},
        {"Frame", ui::text("report_dialog.label_33")},
        {"Longitude", ui::text("report_dialog.label_34")},
        {"Geodetic latitude", ui::text("report_dialog.label_18")},
        {"Ellipsoid height", ui::text("report_dialog.label_35")},
        {"Semi-major axis", ui::text("report_dialog.label_36")},
        {"Eccentricity", ui::text("report_dialog.label_37")},
        {"Inclination", ui::text("report_dialog.label_38")},
        {"RAAN", ui::text("report_dialog.label_39")},
        {"Argument of periapsis", ui::text("report_dialog.label_40")},
        {"True anomaly", ui::text("report_dialog.label_41")},
        {"Geocentric distance", ui::text("report_dialog.label_42")},
        {"Speed", ui::text("report_dialog.label_43")},
        {"Radial speed", ui::text("report_dialog.label_44")},
        {"Transverse speed", ui::text("report_dialog.label_45")},
        {"Flight path angle", ui::text("report_dialog.label_46")},
        {"Scenario", ui::text("common.scenario")},
        {"Start", ui::text("common.start")},
        {"Stop", ui::text("common.end")},
        {"Time scale", ui::text("report_dialog.label_47")},
        {"Satellites", ui::text("report_dialog.label_48")},
        {"Ground stations", ui::text("report_dialog.label_49")},
        {"Central body", ui::text("report_dialog.label_50")},
        {"Earth", ui::text("body.earth")},
        {"Satellite", ui::text("report_dialog.label_51")},
        {"Ground station", ui::text("report_dialog.label_52")},
        {"Duration", ui::text("report_dialog.label_53")},
        {"Event", ui::text("report_dialog.label_54")},
        {"Periapsis", ui::text("report_dialog.label_55")},
        {"Apoapsis", ui::text("report_dialog.label_56")},
        {"Ascending node", ui::text("report_dialog.label_57")},
        {"Descending node", ui::text("report_dialog.label_58")},
        {"Lighting", ui::text("report_dialog.label_59")},
        {"Solar disk illumination", ui::text("report_dialog.label_11")},
        {"Sunlight", ui::text("report_dialog.label_60")},
        {"Penumbra", ui::text("report_dialog.label_61")},
        {"Umbra", ui::text("report_dialog.label_62")},
        {"Start clipped", ui::text("report_dialog.label_63")},
        {"Stop clipped", ui::text("report_dialog.label_64")},
        {"Azimuth", ui::text("report_dialog.label_65")},
        {"Elevation", ui::text("report_dialog.label_66")},
        {"Range", ui::text("report_dialog.label_67")},
        {"Maximum elevation", ui::text("report_dialog.label_68")},
        {"Maximum elevation time", ui::text("report_dialog.label_69")},
        {"Minimum range", ui::text("report_dialog.label_70")},
        {"Minimum range time", ui::text("report_dialog.label_71")},
        {"Inertial relative speed", ui::text("report_dialog.label_72")},
        {"RIC radial", ui::text("report_dialog.label_73")},
        {"RIC in-track", ui::text("report_dialog.label_74")},
        {"RIC cross-track", ui::text("report_dialog.label_75")},
        {"Range rate", ui::text("report_dialog.label_76")},
        {"Visible", ui::text("report_dialog.label_77")},
        {"Off boresight", ui::text("report_dialog.label_78")},
        {"Two body", ui::text("report_dialog.label_79")},
        {"J2 secular", ui::text("report_dialog.label_80")},
        {"WGS84 fixed", ui::text("report_dialog.label_81")}};
    const auto match = labels.find(value);
    return match == labels.end() ? value : match->second;
}
class ReportModel final : public QAbstractTableModel {
  public:
    explicit ReportModel(QObject *parent) : QAbstractTableModel(parent) {}
    std::shared_ptr<const runtime::ReportResult> result;
    int digits = 9;
    void set_digits(int value) {
        digits = value;
        if (rowCount() && columnCount())
            emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
                             {Qt::DisplayRole});
    }
    void publish(std::shared_ptr<const runtime::ReportResult> next) {
        beginResetModel();
        result = std::move(next);
        endResetModel();
    }
    int rowCount(const QModelIndex &parent = {}) const override {
        return !parent.isValid() && result ? static_cast<int>(result->table.rows().size()) : 0;
    }
    int columnCount(const QModelIndex &parent = {}) const override {
        return !parent.isValid() && result ? static_cast<int>(result->table.columns().size()) : 0;
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!result || !index.isValid())
            return {};
        const auto &cell = result->table.rows().at(index.row()).at(index.column());
        if (role == Qt::UserRole) {
            if (const auto *value = std::get_if<double>(&cell))
                return *value;
            if (const auto *value = std::get_if<std::int64_t>(&cell))
                return QVariant::fromValue<qlonglong>(*value);
            if (const auto *value = std::get_if<core::Epoch>(&cell))
                return QVariant::fromValue<qlonglong>(value->tai_nanoseconds());
            if (const auto *value = std::get_if<std::string>(&cell))
                return text(*value);
            return {};
        }
        if (role == Qt::FontRole &&
            (std::holds_alternative<double>(cell) || std::holds_alternative<std::int64_t>(cell) ||
             std::holds_alternative<core::Epoch>(cell))) {
            auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            font.setFamilies(
                {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), font.family()});
            font.setPixelSize(12);
            return font;
        }
        if (role == Qt::TextAlignmentRole &&
            (std::holds_alternative<double>(cell) || std::holds_alternative<std::int64_t>(cell)))
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        if (role == Qt::ForegroundRole &&
            result->table.columns().at(index.column()).id == "lighting.phase") {
            const auto phase = std::get<std::string>(cell);
            return QColor(phase == "Sunlight"   ? "#edbd67"
                          : phase == "Penumbra" ? "#b5a4ef"
                                                : "#8db6e7");
        }
        if (role == Qt::ForegroundRole &&
            result->table.columns().at(index.column()).id == "event.kind") {
            const auto kind = std::get<std::string>(cell);
            return QColor(kind == "Periapsis"        ? "#edbd67"
                          : kind == "Apoapsis"       ? "#b5a4ef"
                          : kind == "Ascending node" ? "#7ccdb7"
                                                     : "#8db6e7");
        }
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
            return {};
        // Format the converted number once. Going through full-precision text and
        // parsing it back for every visible cell adds stream/locale work on scroll.
        if (role == Qt::DisplayRole && std::holds_alternative<double>(cell)) {
            const auto value = *result->numeric_value(index.row(), index.column());
            return digits == 9 && report_uses_event_search(result->definition.query.kind)
                       ? QString::number(value, 'f', 3)
                       : QString::number(value, 'g', digits);
        }
        auto value = text(result->cell_text(index.row(), index.column()));
        const auto &field_id = result->table.columns().at(index.column()).id;
        bool clipped = false;
        if (report_is_interval(result->definition.query.kind) &&
            (field_id == "time.epoch" || field_id == "access.stop" ||
             field_id == "lighting.stop")) {
            const auto &source = result->source_table ? *result->source_table : result->table;
            const auto flag = std::ranges::find(
                source.columns(),
                (result->definition.query.kind == ReportKind::lighting_intervals
                     ? (field_id == "time.epoch" ? "lighting.start_clipped"
                                                 : "lighting.stop_clipped")
                     : (field_id == "time.epoch" ? "access.start_clipped" : "access.stop_clipped")),
                &results::Column::id);
            if (flag != source.columns().end())
                clipped = std::get<std::int64_t>(
                              source.rows()[index.row()][flag - source.columns().begin()]) != 0;
        }
        if (role == Qt::ToolTipRole && clipped)
            return value + ui::text("report_dialog.label_82");
        if (role == Qt::DisplayRole && std::holds_alternative<core::Epoch>(cell)) {
            if (value.endsWith('Z'))
                value.chop(1);
            while (value.endsWith('0'))
                value.chop(1);
            if (value.endsWith('.'))
                value.chop(1);
            if (digits != 17 && report_uses_event_search(result->definition.query.kind)) {
                const auto dot = value.indexOf('.');
                if (dot >= 0 && value.size() > dot + 4)
                    value.truncate(dot + 4);
            }
            return value.replace('T', ' ') + (clipped ? QStringLiteral(" *") : QString{});
        }
        const auto &id = result->table.columns().at(index.column()).id;
        if (id == "event.kind" || id == "lighting.phase" || id == "object.type" ||
            id == "object.model" || id == "summary.field" ||
            (id == "summary.value" && index.row() == 6))
            return localized(value);
        return value;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (!result)
            return {};
        if (orientation == Qt::Horizontal && role == Qt::UserRole)
            return text(result->table.columns().at(section).id);
        if (role == Qt::TextAlignmentRole && orientation == Qt::Horizontal) {
            const bool numeric =
                !result->table.rows().empty() &&
                (std::holds_alternative<double>(result->table.rows().front()[section]) ||
                 std::holds_alternative<std::int64_t>(result->table.rows().front()[section]));
            return static_cast<int>((numeric ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
        }
        if (role != Qt::DisplayRole)
            return {};
        if (orientation == Qt::Vertical)
            return section + 1;
        const auto title = text(result->table.columns().at(section).title);
        return localized(title) + text(result->column_title(section)).mid(title.size());
    }
};
QLabel *note(const QString &value, QWidget *parent) {
    auto *label = new QLabel(value, parent);
    label->setWordWrap(true);
    label->setObjectName("hint");
    return label;
}
} // namespace

struct ReportDialog::Impl {
    ReportDialog *owner;
    application::Project project;
    Prepare prepare;
    Save save;
    std::function<void(core::Epoch)> seek;
    std::shared_ptr<const core::ITimeProvider> time;
    QPushButton *library;
    QPointer<ReportSelectionDialog> report_chooser;
    QWidget *settings, *sampling, *custom_times, *settings_panel, *result_panel;
    QWidget *units_field, *scale_field, *frame_field;
    QGridLayout *plot_tools;
    QToolButton *reset_plot;
    QSplitter *split;
    QStackedWidget *result_stack;
    QCheckBox *follow;
    QComboBox *frame, *units, *scale;
    ObjectSelectionField *access_station, *access_target;
    QWidget *access_pair, *access_options, *pass_summary;
    QComboBox *access_mode;
    StepSpinBox *access_tolerance;
    std::array<QLabel *, 4> pass_values{}, pass_captions{};
    EpochEditor *first, *last;
    QDoubleSpinBox *step;
    QLabel *status, *estimate, *result_label, *result_title, *scope_label, *scope_detail;
    QLabel *interval_label, *template_description, *format_title, *step_label, *event_notice;
    QPushButton *generate, *export_csv, *cancel, *scope_button, *view_button;
    QToolButton *refresh_button, *config_menu, *result_more;
    QAction *source_button, *copy_button, *store, *update_template, *remove;
    QAction *choose_curve_action, *choose_columns_action;
    QProgressBar *progress;
    QTableView *table;
    QWidget *table_page;
    QSortFilterProxyModel *table_proxy;
    QTabBar *views;
    QWidget *coverage_settings, *coverage_page;
    std::array<QDoubleSpinBox *, 6> coverage_fields{};
    QCheckBox *coverage_ends;
    CoverageMap *coverage_map;
    QComboBox *coverage_metric, *coverage_windows;
    QLabel *coverage_point;
    QPushButton *coverage_jump;
    std::vector<core::Epoch> coverage_starts;
    QWidget *plot_page;
    ReportPlot *plot;
    QPushButton *plot_field;
    std::vector<ReportCurveField> curve_choices;
    QStringList curve_ids;
    std::map<ReportKind, QStringList> curve_preferences;
    std::map<std::string, ReportColumnSpec> retained_column_specs;
    std::map<std::string, int> column_widths;
    ObjectSelectionField *plot_object;
    QLabel *plot_note;
    ReportModel *model;
    ReportDefinition draft;
    std::vector<std::string> scope_ids;
    bool all_objects = true;
    struct GeneratedReport {
        runtime::Analysis analysis;
        runtime::ReportResult report;
    };
    std::future<GeneratedReport> job;
    std::optional<runtime::Analysis> result_analysis;
    std::uint64_t input_generation = 0, job_generation = 0;
    std::future<void> export_job;
    std::shared_ptr<std::atomic_bool> cancellation;
    std::shared_ptr<std::atomic_size_t> completed;
    bool waiting = false, loading = false, changed = false;
    bool compact = false, wide_settings = true, narrow_settings = true;
    WidthMode panel_mode;
    QList<int> wide_sizes{330, 790};
    std::size_t expected = 0;
    std::uint64_t request = 0;

    Impl(ReportDialog *parent, application::Project input,
         std::shared_ptr<const core::ITimeProvider> clock, Prepare preflight, Save persist,
         std::function<void(core::Epoch)> jump)
        : owner(parent), project(std::move(input)), prepare(std::move(preflight)),
          save(std::move(persist)), seek(std::move(jump)), time(std::move(clock)) {
        owner->setObjectName("reportDialog");
        owner->setWindowTitle(ui::text("common.report"));
        owner->setWindowIcon(workbench_icon(WorkbenchIcon::properties));
        owner->setMinimumSize(320, 320);
        owner->resize(1120, 740);
        auto *root = new QVBoxLayout(owner);
        root->setSizeConstraint(QLayout::SetNoConstraint);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        auto *heading = new QWidget(owner);
        heading->setObjectName("reportHeading");
        auto *heading_layout = new QHBoxLayout(heading);
        heading_layout->setContentsMargins(8, 3, 8, 3);
        heading_layout->setSpacing(6);
        view_button = new QPushButton(ui::text("report_dialog.label_83"), heading);
        view_button->setObjectName("reportToggleSettings");
        heading_layout->addWidget(view_button);
        root->addWidget(heading);
        split = new QSplitter(owner);
        split->setObjectName("reportSplit");
        split->setChildrenCollapsible(false);
        split->setHandleWidth(4);
        root->addWidget(split, 1);
        settings_panel = new QWidget(split);
        settings_panel->setObjectName("reportSettingsPanel");
        auto *panel_layout = new QVBoxLayout(settings_panel);
        panel_layout->setContentsMargins(0, 0, 0, 0);
        panel_layout->setSpacing(0);
        auto *scroll = new QScrollArea(settings_panel);
        scroll->setObjectName("reportSettingsScroll");
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFrameShape(QFrame::NoFrame);
        panel_layout->addWidget(scroll, 1);
        settings = new QWidget(scroll);
        settings->setObjectName("reportSettings");
        scroll->setWidget(settings);
        auto *config = new QVBoxLayout(settings);
        config->setContentsMargins(12, 10, 12, 12);
        config->setSpacing(6);
        const auto section = [&](const QString &label) {
            if (config->count())
                config->addSpacing(8);
            auto *caption = new QLabel(label, settings);
            caption->setObjectName("reportSectionTitle");
            config->addWidget(caption);
        };
        section(ui::text("report_dialog.label_84"));
        auto *scope_row = new QHBoxLayout;
        scope_label = new QLabel(settings);
        scope_label->setObjectName("reportScopeLabel");
        scope_label->setWordWrap(true);
        scope_label->setTextFormat(Qt::PlainText);
        scope_row->addWidget(scope_label, 1);
        scope_button = new QPushButton(ui::text("report_dialog.label_85"), settings);
        scope_button->setObjectName("reportChangeScope");
        scope_row->addWidget(scope_button);
        config->addLayout(scope_row);
        scope_detail = note({}, settings);
        scope_detail->setObjectName("reportScopeDetail");
        config->addWidget(scope_detail);
        access_pair = new QWidget(settings);
        auto *pair_form = new QFormLayout(access_pair);
        pair_form->setContentsMargins(0, 0, 0, 0);
        pair_form->setVerticalSpacing(12);
        pair_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        access_station = new ObjectSelectionField(access_pair);
        access_station->set_options({ui::text("report_dialog.label_86"), false, {}});
        access_station->setAccessibleName(
            ui::text("report_dialog.access_station.set_accessible_name"));
        access_station->setObjectName("accessStation");
        access_target = new ObjectSelectionField(access_pair);
        access_target->set_options({ui::text("report_dialog.label_87"), false, {}});
        access_target->setAccessibleName(
            ui::text("report_dialog.access_target.set_accessible_name"));
        access_target->setObjectName("accessTarget");
        pair_form->addRow(ui::text("report_dialog.label_52"), access_station);
        pair_form->addRow(ui::text("report_dialog.pair_form.add_row"), access_target);
        config->addWidget(access_pair);
        auto *template_heading = new QHBoxLayout;
        auto *template_title = new QLabel(ui::text("report_dialog.template_title"), settings);
        template_title->setObjectName("reportSectionTitle");
        template_heading->addWidget(template_title);
        template_heading->addStretch();
        config_menu = new QToolButton(settings);
        config_menu->setObjectName("reportConfigMenu");
        config_menu->setText(ui::text("report_dialog.config_menu.set_text"));
        config_menu->setToolTip(ui::text("report_dialog.config_menu.set_tool_tip"));
        config_menu->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(config_menu);
        store = menu->addAction(ui::text("report_dialog.menu.add_action"));
        store->setObjectName("saveReport");
        update_template = menu->addAction(ui::text("report_dialog.menu.add_action_2"));
        update_template->setObjectName("updateReport");
        menu->addSeparator();
        remove = menu->addAction(ui::text("report_dialog.menu.add_action_3"));
        remove->setObjectName("deleteReport");
        config_menu->setMenu(menu);
        template_heading->addWidget(config_menu);
        config->addSpacing(8);
        config->addLayout(template_heading);
        library = new ReportSelectionButton(settings);
        library->setObjectName("reportLibrary");
        library->setProperty("objectSelector", true);
        library->setAutoDefault(false);
        library->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        library->setAccessibleName(ui::text("report_dialog.library.set_accessible_name"));
        config->addWidget(library);
        template_description = note({}, settings);
        template_description->setObjectName("reportTemplateDescription");
        config->addWidget(template_description);
        coverage_settings = new QWidget(settings);
        coverage_settings->setObjectName("coverageSettings");
        auto *coverage_form = new QFormLayout(coverage_settings);
        coverage_form->setContentsMargins(0, 8, 0, 0);
        coverage_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        const std::array labels{
            ui::text("report_dialog.label_88"), QString{},
            ui::text("report_dialog.label_89"), QString{},
            ui::text("report_dialog.label_90"), ui::text("report_dialog.label_91")};
        const std::array names{"coverageSouth", "coverageNorth",      "coverageWest",
                               "coverageEast",  "coverageResolution", "coverageElevation"};
        for (int i = 0; i < 6; ++i) {
            coverage_fields[i] = new QDoubleSpinBox(coverage_settings);
            coverage_fields[i]->setObjectName(names[i]);
            coverage_fields[i]->setAccessibleName(QString::fromLatin1(names[i]));
            coverage_fields[i]->setDecimals(4);
            coverage_fields[i]->setRange(i == 4   ? .01
                                         : i == 5 ? 0
                                         : i < 2  ? -90
                                                  : -180,
                                         i < 2 || i == 5 ? 90 : 180);
            coverage_fields[i]->setSuffix(QStringLiteral("°"));
            coverage_fields[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }
        for (int i : {0, 2}) {
            auto *pair = new QHBoxLayout;
            pair->addWidget(coverage_fields[i]);
            pair->addWidget(coverage_fields[i + 1]);
            coverage_form->addRow(labels[i], pair);
        }
        for (int i : {4, 5})
            coverage_form->addRow(labels[i], coverage_fields[i]);
        coverage_fields[2]->setToolTip(ui::text("report_dialog.label_92"));
        auto *global = new QPushButton(ui::text("report_dialog.global"), coverage_settings);
        global->setObjectName("coverageGlobal");
        coverage_form->addRow(global);
        coverage_ends = new QCheckBox(ui::text("report_dialog.label_93"), coverage_settings);
        coverage_ends->setObjectName("coverageEndGaps");
        coverage_ends->setToolTip(ui::text("report_dialog.coverage_ends.set_tool_tip"));
        coverage_form->addRow(coverage_ends);
        config->addWidget(coverage_settings);
        for (auto *field : coverage_fields)
            QObject::connect(field, &QDoubleSpinBox::valueChanged, owner, [this] { edit(); });
        QObject::connect(coverage_ends, &QCheckBox::toggled, owner, [this] { edit(); });
        QObject::connect(global, &QPushButton::clicked, owner, [this] {
            loading = true;
            coverage_fields[0]->setValue(-90);
            coverage_fields[1]->setValue(90);
            coverage_fields[2]->setValue(-180);
            coverage_fields[3]->setValue(180);
            loading = false;
            edit();
        });
        sampling = new QWidget(settings);
        auto *sampling_layout = new QVBoxLayout(sampling);
        sampling_layout->setContentsMargins(0, 8, 0, 0);
        sampling_layout->setSpacing(6);
        auto *sampling_title = new QLabel(ui::text("report_dialog.sampling_title"), sampling);
        sampling_title->setObjectName("reportSectionTitle");
        sampling_layout->addWidget(sampling_title);
        follow = new QCheckBox(ui::text("report_dialog.label_94"), sampling);
        follow->setObjectName("reportFollowScenario");
        sampling_layout->addWidget(follow);
        interval_label = note({}, sampling);
        interval_label->setObjectName("reportInterval");
        sampling_layout->addWidget(interval_label);
        custom_times = new QWidget(sampling);
        auto *time_layout = new QVBoxLayout(custom_times);
        time_layout->setContentsMargins(0, 0, 0, 0);
        time_layout->setSpacing(6);
        first = new EpochEditor(custom_times, time);
        first->setObjectName("reportFirst");
        last = new EpochEditor(custom_times, time);
        last->setObjectName("reportLast");
        time_layout->addWidget(note(ui::text("common.start"), custom_times));
        time_layout->addWidget(first);
        time_layout->addWidget(note(ui::text("common.end"), custom_times));
        time_layout->addWidget(last);
        for (auto *editor : {first, last})
            editor->findChild<QLineEdit *>("epochValue")->setMinimumWidth(180);
        sampling_layout->addWidget(custom_times);
        auto *sampling_form = new QFormLayout;
        sampling_form->setHorizontalSpacing(12);
        sampling_form->setVerticalSpacing(6);
        sampling_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        step = new StepSpinBox(sampling);
        step->setObjectName("reportStep");
        step->setDecimals(9);
        step->setRange(0.000000001, 86400);
        step->setValue(60);
        step->setSuffix(ui::text("report_dialog.step.set_suffix"));
        step->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        step->setToolTip(ui::text("report_dialog.step.set_tool_tip"));
        step_label = new QLabel(ui::text("report_dialog.label_95"));
        sampling_form->addRow(step_label, step);
        access_options = new QWidget(sampling);
        auto *access_form = new QFormLayout(access_options);
        access_form->setContentsMargins(0, 0, 0, 0);
        access_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        access_form->setVerticalSpacing(10);
        access_mode = new QComboBox(access_options);
        access_mode->setObjectName("accessSearchMode");
        access_mode->addItems(
            {ui::text("report_dialog.label_96"), ui::text("report_dialog.label_97")});
        access_mode->setToolTip(ui::text("report_dialog.access_mode.set_tool_tip"));
        access_tolerance = new StepSpinBox(access_options);
        access_tolerance->setObjectName("accessTimeTolerance");
        access_tolerance->setDecimals(6);
        access_tolerance->setRange(.000001, 1000);
        access_tolerance->setValue(.001);
        access_tolerance->setSuffix(ui::text("report_dialog.step.set_suffix"));
        access_tolerance->setToolTip(ui::text("report_dialog.access_tolerance.set_tool_tip"));
        access_form->addRow(ui::text("report_dialog.access_form.add_row"), access_mode);
        access_form->addRow(ui::text("report_dialog.access_form.add_row_2"), access_tolerance);
        sampling_form->addRow(access_options);
        frame = new QComboBox(sampling);
        frame->setObjectName("reportFrame");
        frame->addItems({"GCRF", "ITRF"});
        frame_field = new QWidget(sampling);
        auto *frame_form = new QFormLayout(frame_field);
        frame_form->setContentsMargins(0, 0, 0, 0);
        frame_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        frame_form->addRow(ui::text("report_dialog.label_33"), frame);
        sampling_layout->addLayout(sampling_form);
        sampling_layout->addWidget(frame_field);
        config->addWidget(sampling);
        section(ui::text("report_dialog.label_98"));
        format_title = qobject_cast<QLabel *>(config->itemAt(config->count() - 1)->widget());
        units_field = new QWidget(settings);
        auto *units_form = new QFormLayout(units_field);
        units_form->setContentsMargins(0, 0, 0, 0);
        units_form->setHorizontalSpacing(12);
        units_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        units = new QComboBox(units_field);
        units->setObjectName("reportUnits");
        units->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        units->setMinimumContentsLength(8);
        units->addItems({QStringLiteral("km / km·s⁻¹ / °"), QStringLiteral("m / m·s⁻¹ / rad")});
        units_form->addRow(ui::text("report_dialog.units_form.add_row"), units);
        config->addWidget(units_field);
        scale_field = new QWidget(settings);
        auto *scale_form = new QFormLayout(scale_field);
        scale_form->setContentsMargins(0, 0, 0, 0);
        scale_form->setHorizontalSpacing(12);
        scale_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        scale = new QComboBox(scale_field);
        scale->setObjectName("reportTimeScale");
        scale->addItems({"UTC", "TAI", "TT"});
        scale_form->addRow(ui::text("report_dialog.label_47"), scale);
        for (auto *form : {sampling_form, access_form, frame_form, units_form, scale_form}) {
            form->setHorizontalSpacing(12);
            if (auto *label = form->itemAt(0, QFormLayout::LabelRole)->widget())
                label->setFixedWidth(settings->fontMetrics().horizontalAdvance(
                    ui::text("report_dialog.label.set_fixed_width")));
        }
        config->addWidget(scale_field);
        config->addStretch();
        auto *actions = new QWidget(settings_panel);
        actions->setObjectName("reportGenerateActions");
        auto *actions_layout = new QVBoxLayout(actions);
        actions_layout->setContentsMargins(12, 8, 12, 8);
        actions_layout->setSpacing(6);
        estimate = note({}, actions);
        estimate->setObjectName("reportEstimate");
        actions_layout->addWidget(estimate);
        generate = new QPushButton(ui::text("report_dialog.label_99"), actions);
        generate->setObjectName("generateReport");
        generate->setProperty("primary", true);
        generate->setDefault(true);
        auto *generation_buttons = new QHBoxLayout;
        generation_buttons->addStretch();
        generate->setMinimumWidth(104);
        cancel = new QPushButton(ui::text("report_dialog.label_100"), actions);
        cancel->setObjectName("cancelReport");
        cancel->hide();
        generation_buttons->addWidget(cancel);
        generation_buttons->addWidget(generate);
        actions_layout->addLayout(generation_buttons);
        panel_layout->addWidget(actions);
        result_panel = new QWidget(split);
        result_panel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        result_panel->setObjectName("reportResultPanel");
        auto *result_layout = new QVBoxLayout(result_panel);
        result_layout->setContentsMargins(0, 0, 0, 0);
        result_layout->setSpacing(0);
        // One document toolbar; keep the table's title and commands out of its data area.
        result_title = new ReportTitle(ui::text("common.report"), heading);
        result_title->setObjectName("reportResultTitle");
        result_title->setTextFormat(Qt::PlainText);
        result_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        heading_layout->addWidget(result_title, 1);
        refresh_button = new QToolButton(heading);
        refresh_button->setObjectName("refreshReport");
        refresh_button->setIcon(workbench_icon(WorkbenchIcon::refresh));
        refresh_button->setIconSize(QSize(16, 16));
        refresh_button->setFixedSize(28, 28);
        refresh_button->setAccessibleName(ui::text("report.refresh"));
        refresh_button->setToolTip(ui::text("report_dialog.refresh_button.set_tool_tip"));
        refresh_button->setShortcut(QKeySequence(Qt::Key_F5));
        refresh_button->setEnabled(false);
        heading_layout->addWidget(refresh_button);
        export_csv = new QPushButton(ui::text("report_dialog.label_101"), heading);
        export_csv->setObjectName("exportReportCsv");
        export_csv->setEnabled(false);
        heading_layout->addWidget(export_csv);
        result_more = new QToolButton(heading);
        result_more->setObjectName("reportResultMenu");
        result_more->setIcon(workbench_icon(WorkbenchIcon::more));
        result_more->setIconSize(QSize(16, 16));
        result_more->setFixedSize(28, 28);
        result_more->setToolTip(ui::text("report.more_hint"));
        result_more->setAccessibleName(result_more->toolTip());
        result_more->setPopupMode(QToolButton::InstantPopup);
        result_more->setEnabled(false);
        auto *result_menu = new QMenu(result_more);
        copy_button = result_menu->addAction(ui::text("report_dialog.result_menu.add_action"));
        copy_button->setObjectName("reportCopy");
        copy_button->setEnabled(false);
        source_button = result_menu->addAction(ui::text("report_dialog.result_menu.add_action_2"));
        source_button->setObjectName("reportShowSource");
        source_button->setEnabled(false);
        result_more->setMenu(result_menu);
        heading_layout->addWidget(result_more);
        result_label = note(ui::text("report_dialog.label_102"), result_panel);
        result_label->setTextFormat(Qt::PlainText);
        result_label->setObjectName("reportResultLabel");
        auto *result_meta_bar = new QWidget(result_panel);
        result_meta_bar->setObjectName("reportViewBar");
        auto *result_meta = new QHBoxLayout(result_meta_bar);
        result_meta->setContentsMargins(8, 0, 10, 0);
        result_meta->setSpacing(8);
        views = new QTabBar(result_panel);
        views->setObjectName("reportViews");
        views->setExpanding(false);
        views->setDrawBase(false);
        views->addTab(ui::text("report.table"));
        views->addTab(ui::text("report.curves"));
        views->addTab(ui::text("report_dialog.views.add_tab"));
        views->setTabEnabled(2, false);
        views->setTabEnabled(1, false);
        views->setTabVisible(2, false);
        views->setTabVisible(1, false);
        views->setTabToolTip(1, ui::text("report_dialog.views.set_tab_tool_tip"));
        result_meta->addWidget(views);
        result_meta->addWidget(result_label, 1);
        result_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        result_layout->addWidget(result_meta_bar);
        pass_summary = new QWidget(result_panel);
        pass_summary->setObjectName("accessPassSummary");
        auto *metrics = new QGridLayout(pass_summary);
        metrics->setContentsMargins(8, 4, 8, 4);
        metrics->setSpacing(0);
        const std::array metric_names{
            ui::text("report_dialog.label_103"), ui::text("report_dialog.label_104"),
            ui::text("report_dialog.label_105"), ui::text("report_dialog.label_106")};
        std::array<QFrame *, 4> cards{};
        for (std::size_t i = 0; i < cards.size(); ++i) {
            cards[i] = new QFrame(pass_summary);
            cards[i]->setObjectName("accessMetric");
            auto *card = new QVBoxLayout(cards[i]);
            card->setContentsMargins(8, 3, 8, 3);
            card->setSpacing(2);
            auto *caption = note(metric_names[i], cards[i]);
            pass_captions[i] = caption;
            caption->setWordWrap(false);
            card->addWidget(caption);
            pass_values[i] = new QLabel(QStringLiteral("—"), cards[i]);
            pass_values[i]->setObjectName(QStringLiteral("accessMetric%1").arg(i));
            pass_values[i]->setStyleSheet(
                QStringLiteral("font-size: 14px; color: %1;")
                    .arg(theme::color(i == 0 ? theme::accent : theme::text).name()));
            card->addWidget(pass_values[i]);
            metrics->addWidget(cards[i], 0, static_cast<int>(i));
        }
        new WidthObserver(pass_summary, [metrics, cards, columns = 0](int width) mutable {
            const int next = width < 520 ? 2 : 4;
            if (columns == next)
                return;
            columns = next;
            for (int i = 0; i < 4; ++i) {
                metrics->removeWidget(cards[i]);
                metrics->addWidget(cards[i], i / columns, i % columns);
                metrics->setColumnStretch(i, i < columns ? 1 : 0);
            }
        });
        pass_summary->hide();
        result_layout->addWidget(pass_summary);
        event_notice = note({}, result_panel);
        event_notice->setObjectName("orbitalEventNotice");
        event_notice->hide();
        result_layout->addWidget(event_notice);
        result_stack = new QStackedWidget(result_panel);
        result_layout->addWidget(result_stack, 1);
        auto *empty = new QWidget(result_stack);
        empty->setObjectName("reportEmptyState");
        auto *empty_layout = new QVBoxLayout(empty);
        empty_layout->setContentsMargins(20, 20, 20, 20);
        empty_layout->addStretch();
        empty_layout->setSpacing(6);
        auto *empty_icon = new QLabel(empty);
        empty_icon->setObjectName("reportEmptyIcon");
        empty_icon->setFixedSize(32, 32);
        empty_icon->setAlignment(Qt::AlignCenter);
        empty_icon->setPixmap(
            workbench_icon(WorkbenchIcon::properties, theme::color(theme::accent)).pixmap(24, 24));
        empty_layout->addWidget(empty_icon, 0, Qt::AlignHCenter);
        empty_layout->addSpacing(8);
        auto *empty_title = new QLabel(ui::text("report_dialog.empty_title"), empty);
        empty_title->setObjectName("reportEmptyTitle");
        empty_title->setAlignment(Qt::AlignCenter);
        empty_layout->addWidget(empty_title);
        auto *empty_note = note(ui::text("report_dialog.empty_note"), empty);
        empty_note->setAlignment(Qt::AlignCenter);
        empty_layout->addWidget(empty_note);
        empty_layout->addStretch();
        result_stack->addWidget(empty);
        table_page = new QWidget(result_stack);
        auto *table_layout = new QVBoxLayout(table_page);
        table_layout->setContentsMargins(0, 0, 0, 0);
        table_layout->setSpacing(0);
        auto *table_tools = new QToolBar(table_page);
        table_tools->setObjectName("reportTableTools");
        table_tools->setMinimumWidth(0);
        table_tools->setMovable(false);
        table_tools->setToolButtonStyle(Qt::ToolButtonTextOnly);
        auto *precision = new QComboBox(table_tools);
        precision->setObjectName("reportPrecision");
        precision->setAccessibleName(ui::text("report_dialog.precision.set_accessible_name"));
        precision->addItem(ui::text("report_dialog.precision.add_item"), 9);
        precision->addItem(ui::text("report_dialog.precision.add_item_2"), 6);
        precision->addItem(ui::text("report_dialog.precision.add_item_3"), 12);
        precision->addItem(ui::text("report_dialog.precision.add_item_4"), 17);
        precision->setToolTip(ui::text("report_dialog.precision.set_tool_tip"));
        table_tools->addWidget(precision);
        table_tools->addSeparator();
        table_layout->addWidget(table_tools);
        table = new QTableView(table_page);
        table->setObjectName("reportTable");
        table->setMinimumSize(0, 0);
        model = new ReportModel(table);
        table_proxy = new QSortFilterProxyModel(table);
        table_proxy->setSourceModel(model);
        table_proxy->setSortRole(Qt::UserRole);
        table_proxy->setDynamicSortFilter(false);
        table->setModel(table_proxy);
        table->setSortingEnabled(true);
        table->sortByColumn(-1, Qt::AscendingOrder);
        table->horizontalHeader()->setSortIndicatorClearable(true);
        table->horizontalHeader()->setToolTip(ui::text("report_dialog.label_107"));
        table->setAlternatingRowColors(true);
        table->setShowGrid(false);
        table->setWordWrap(false);
        table->setTextElideMode(Qt::ElideRight);
        table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->setDefaultSectionSize(26);
        table->horizontalHeader()->setMinimumSectionSize(70);
        table->horizontalHeader()->setResizeContentsPrecision(50);
        auto *copy = new QAction(ui::text("report_dialog.copy"), table);
        copy->setObjectName("copyReportRows");
        copy->setShortcut(QKeySequence::Copy);
        copy->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        table->addAction(copy);
        table->setContextMenuPolicy(Qt::ActionsContextMenu);
        QObject::connect(copy, &QAction::triggered, owner, [this] { copy_rows(); });
        auto *copy_direct = table_tools->addAction(ui::text("common.copy"));
        copy_direct->setObjectName("reportTableCopy");
        copy_direct->setToolTip(ui::text("report_dialog.copy_direct.set_tool_tip"));
        copy_direct->setEnabled(false);
        QObject::connect(copy_direct, &QAction::triggered, owner, [this] { copy_rows(); });
        choose_columns_action =
            table_tools->addAction(ui::text("report_dialog.table_tools.add_action"));
        choose_columns_action->setObjectName("reportChooseColumns");
        choose_columns_action->setToolTip(
            ui::text("report_dialog.choose_columns_action.set_tool_tip"));
        choose_columns_action->setEnabled(false);
        table->addAction(choose_columns_action);
        QObject::connect(choose_columns_action, &QAction::triggered, owner,
                         [this] { choose_columns(); });
        choose_curve_action =
            table_tools->addAction(ui::text("report_dialog.table_tools.add_action_2"));
        choose_curve_action->setObjectName("reportChooseCurves");
        choose_curve_action->setToolTip(ui::text("report_dialog.choose_curve_action.set_tool_tip"));
        choose_curve_action->setEnabled(false);
        QObject::connect(choose_curve_action, &QAction::triggered, owner, [this] {
            if (choose_curves())
                views->setCurrentIndex(1);
        });
        auto *fit_columns =
            table_tools->addAction(ui::text("report_dialog.table_tools.add_action_3"));
        fit_columns->setObjectName("reportFitColumns");
        fit_columns->setToolTip(ui::text("report_dialog.fit_columns.set_tool_tip"));
        QObject::connect(fit_columns, &QAction::triggered, owner, [this] { fit_table_columns(); });
        auto *original_order =
            table_tools->addAction(ui::text("report_dialog.table_tools.add_action_4"));
        original_order->setObjectName("reportOriginalOrder");
        original_order->setEnabled(false);
        QObject::connect(original_order, &QAction::triggered, owner,
                         [this] { table->sortByColumn(-1, Qt::AscendingOrder); });
        QObject::connect(table->horizontalHeader(), &QHeaderView::sortIndicatorChanged, owner,
                         [original_order](int column, Qt::SortOrder) {
                             original_order->setEnabled(column >= 0);
                         });
        QObject::connect(precision, &QComboBox::currentIndexChanged, owner, [this, precision] {
            model->set_digits(precision->currentData().toInt());
        });
        QObject::connect(copy_button, &QAction::triggered, owner, [this] {
            if (views->currentIndex() > 0) {
                QApplication::clipboard()->setImage(
                    views->currentIndex() == 2 ? coverage_map->image() : plot->image());
                status->setText(ui::text("report_dialog.status.set_text"));
            } else
                copy_rows();
        });
        QObject::connect(source_button, &QAction::triggered, owner, [this] { show_source(); });
        QObject::connect(refresh_button, &QToolButton::clicked, owner, [this] { start(); });
        const auto update_selection = [this, copy_direct, copy] {
            const bool selected = table->selectionModel()->hasSelection();
            copy_direct->setEnabled(selected);
            copy->setEnabled(selected);
            copy_button->setEnabled(views->currentIndex() > 0 || selected);
        };
        QObject::connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, owner,
                         update_selection);
        QObject::connect(table_proxy, &QAbstractItemModel::modelReset, owner, update_selection);
        copy->setEnabled(false);
        table_layout->addWidget(table, 1);
        result_stack->addWidget(table_page);
        plot_page = new QWidget(result_stack);
        auto *plot_layout = new QVBoxLayout(plot_page);
        plot_layout->setContentsMargins(8, 6, 8, 6);
        plot_layout->setSpacing(8);

        plot_field = new ReportSelectionButton(plot_page);
        plot_field->setObjectName("reportPlotFields");
        plot_field->setAccessibleName(ui::text("report_dialog.plot_field.set_accessible_name"));
        plot_object = new ObjectSelectionField(plot_page);
        plot_object->setObjectName("reportPlotObject");
        plot_object->setAccessibleName(ui::text("report_dialog.plot_object.set_accessible_name"));
        plot_field->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        plot_field->setToolTip(ui::text("report_dialog.plot_field.set_tool_tip"));
        plot_object->setToolTip(ui::text("report_dialog.plot_object.set_tool_tip"));
        plot_tools = new QGridLayout;
        plot_tools->setContentsMargins(0, 0, 0, 0);
        plot_tools->setHorizontalSpacing(10);
        plot_tools->setColumnStretch(0, 1);
        plot_tools->setColumnStretch(1, 1);
        plot_tools->addWidget(plot_field, 0, 0);
        plot_tools->addWidget(plot_object, 0, 1);
        reset_plot = new QToolButton(plot_page);
        reset_plot->setObjectName("reportPlotReset");
        reset_plot->setIcon(workbench_icon(WorkbenchIcon::fit));
        reset_plot->setToolTip(ui::text("report_dialog.reset_plot.set_tool_tip"));
        reset_plot->setAccessibleName(ui::text("report_dialog.reset_plot.set_accessible_name"));
        reset_plot->setText(ui::text("report_dialog.reset_plot.set_text"));
        reset_plot->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        new WidthObserver(
            plot_page, [this, mode = WidthMode{}, previous = false](int width) mutable {
                const bool small = mode.narrow(width, 500);
                if (small == previous)
                    return;
                previous = small;
                plot_tools->removeWidget(plot_field);
                plot_tools->removeWidget(plot_object);
                plot_tools->addWidget(plot_field, 0, 0, 1, small ? 2 : 1);
                plot_tools->addWidget(plot_object, small ? 1 : 0, small ? 0 : 1, 1, small ? 2 : 1);
            });
        plot_layout->addLayout(plot_tools);
        plot = new ReportPlot(plot_page);
        auto *navigation = new QToolBar(plot_page);
        navigation->setObjectName("reportPlotTools");
        navigation->setMinimumWidth(0);
        navigation->setMovable(false);
        navigation->setIconSize(QSize(16, 16));
        auto *mode_group = new QActionGroup(navigation);
        auto *box = navigation->addAction(ui::text("report_dialog.navigation.add_action"));
        box->setObjectName("reportPlotBox");
        box->setCheckable(true);
        box->setChecked(true);
        box->setToolTip(ui::text("report_dialog.box.set_tool_tip"));
        mode_group->addAction(box);
        auto *pan = navigation->addAction(ui::text("report_dialog.navigation.add_action_2"));
        pan->setObjectName("reportPlotPan");
        pan->setCheckable(true);
        pan->setToolTip(ui::text("report_dialog.pan.set_tool_tip"));
        mode_group->addAction(pan);
        QObject::connect(pan, &QAction::toggled, plot, &ReportPlot::set_pan_mode);
        navigation->addSeparator();
        auto *zoom_in = navigation->addAction(workbench_icon(WorkbenchIcon::zoom_in),
                                              ui::text("report_dialog.navigation.add_action_3"));
        zoom_in->setObjectName("reportPlotZoomIn");
        zoom_in->setToolTip(ui::text("report_dialog.zoom_in.set_tool_tip"));
        auto *zoom_out = navigation->addAction(workbench_icon(WorkbenchIcon::zoom_out),
                                               ui::text("report_dialog.navigation.add_action_4"));
        zoom_out->setObjectName("reportPlotZoomOut");
        zoom_out->setToolTip(ui::text("report_dialog.zoom_out.set_tool_tip"));
        QObject::connect(zoom_in, &QAction::triggered, plot, [this] { plot->zoom(.8); });
        QObject::connect(zoom_out, &QAction::triggered, plot, [this] { plot->zoom(1.25); });
        navigation->addWidget(reset_plot);
        plot_layout->addWidget(navigation);
        plot->seek = [this](core::Epoch epoch) {
            if (epoch.tai_nanoseconds() >= project.first.tai_nanoseconds() &&
                epoch.tai_nanoseconds() <= project.last.tai_nanoseconds() && seek)
                seek(epoch);
        };
        auto *plot_scroll = new QScrollArea(plot_page);
        plot_scroll->setObjectName("reportPlotScroll");
        plot_scroll->setFrameShape(QFrame::NoFrame);
        plot_scroll->setWidgetResizable(true);
        plot_scroll->setWidget(plot);
        plot_layout->addWidget(plot_scroll, 1);
        plot_note = note(ui::text("report_dialog.label_108"), plot_page);
        plot_layout->addWidget(plot_note);
        result_stack->addWidget(plot_page);
        coverage_page = new QWidget(result_stack);
        auto *coverage_layout = new QVBoxLayout(coverage_page);
        coverage_layout->setContentsMargins(0, 0, 0, 0);
        coverage_metric = new QComboBox(coverage_page);
        coverage_metric->setObjectName("coverageMetric");
        coverage_metric->addItems(
            {ui::text("report_dialog.label_21"), ui::text("report_dialog.label_23")});
        coverage_layout->addWidget(coverage_metric);
        coverage_map = new CoverageMap(coverage_page);
        coverage_layout->addWidget(coverage_map, 1);
        coverage_point = note(ui::text("report_dialog.label_109"), coverage_page);
        coverage_point->setObjectName("coveragePointDetail");
        coverage_layout->addWidget(coverage_point);
        coverage_windows = new QComboBox(coverage_page);
        coverage_windows->setObjectName("coverageWindows");
        coverage_windows->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        coverage_windows->setAccessibleName(
            ui::text("report_dialog.coverage_windows.set_accessible_name"));
        coverage_layout->addWidget(coverage_windows);
        coverage_jump = new QPushButton(ui::text("report_dialog.label_110"), coverage_page);
        coverage_jump->setObjectName("coverageJump");
        coverage_layout->addWidget(coverage_jump);
        coverage_layout->addWidget(
            note(ui::text("report_dialog.coverage_layout.add_widget"), coverage_page));
        result_stack->addWidget(coverage_page);
        coverage_map->selected = [this](std::size_t index) { select_coverage_point(index); };
        QObject::connect(coverage_metric, &QComboBox::currentIndexChanged, owner,
                         [this](int index) { coverage_map->set_metric(index); });
        QObject::connect(coverage_jump, &QPushButton::clicked, owner, [this] {
            const auto index = coverage_windows->currentIndex();
            if (index >= 0 && static_cast<std::size_t>(index) < coverage_starts.size())
                seek_result(coverage_starts[index]);
        });
        QObject::connect(reset_plot, &QToolButton::clicked, owner, [this] { plot->reset_view(); });
        QObject::connect(plot_field, &QPushButton::clicked, owner, [this] { choose_curves(); });
        QObject::connect(plot_object, &ObjectSelectionField::selectionChanged, owner,
                         [this] { plot->set_object(plot_object->id()); });
        QObject::connect(views, &QTabBar::currentChanged, owner, [this] { select_view(); });
        split->setStretchFactor(0, 0);
        split->setStretchFactor(1, 1);
        split->setSizes({330, 790});
        QObject::connect(split, &QSplitter::splitterMoved, owner, [this] {
            if (!compact && wide_settings)
                wide_sizes = split->sizes();
        });
        progress = new QProgressBar(owner);
        progress->setObjectName("reportProgress");
        progress->setMaximumHeight(4);
        progress->setTextVisible(false);
        progress->hide();
        root->addWidget(progress);
        status = note(ui::text("report_dialog.label_111"), owner);
        status->setObjectName("reportStatus");
        status->setContentsMargins(10, 3, 10, 3);
        root->addWidget(status);
        QObject::connect(library, &QPushButton::clicked, owner, [this] { choose_report(); });
        QObject::connect(scope_button, &QPushButton::clicked, owner, [this] { choose_scope(); });
        QObject::connect(view_button, &QPushButton::clicked, owner, [this] {
            if (compact)
                narrow_settings = !narrow_settings;
            else
                wide_settings = !wide_settings;
            adapt(owner->width());
        });
        QObject::connect(follow, &QCheckBox::toggled, owner, [this] {
            update_interval();
            edit();
        });
        for (auto *combo : {frame, units, scale})
            QObject::connect(combo, &QComboBox::currentIndexChanged, owner, [this] { edit(); });
        for (auto *field : {access_station, access_target})
            QObject::connect(field, &ObjectSelectionField::selectionChanged, owner, [this] {
                if (!loading && pair_report(draft.query.kind)) {
                    scope_ids = {access_station->id(), access_target->id()};
                    all_objects = false;
                    refresh_scope();
                    edit();
                }
            });
        QObject::connect(scale, &QComboBox::currentIndexChanged, owner, [this](int index) {
            first->set_scale(static_cast<CalendarScale>(index));
            last->set_scale(static_cast<CalendarScale>(index));
            update_interval();
        });
        QObject::connect(step, &QDoubleSpinBox::valueChanged, owner, [this] { edit(); });
        QObject::connect(access_tolerance, &QDoubleSpinBox::valueChanged, owner,
                         [this] { edit(); });
        QObject::connect(access_mode, &QComboBox::currentIndexChanged, owner, [this] {
            if (report_uses_event_search(draft.query.kind))
                step_label->setText(access_mode->currentIndex() == 0
                                        ? ui::text("report_dialog.step_label.set_text")
                                        : ui::text("report_dialog.label.set_fixed_width"));
            if (draft.query.kind == ReportKind::relative_motion)
                step_label->setText(ui::text("report_dialog.step_label.set_text_2"));
            edit();
        });
        for (auto *editor : {first, last})
            for (auto *line : editor->findChildren<QLineEdit *>())
                QObject::connect(line, &QLineEdit::editingFinished, owner, [this] { edit(); });
        QObject::connect(generate, &QPushButton::clicked, owner, [this] { start(); });
        QObject::connect(store, &QAction::triggered, owner, [this] { this->persist(true); });
        QObject::connect(update_template, &QAction::triggered, owner, [this] { this->persist(); });
        QObject::connect(remove, &QAction::triggered, owner, [this] { erase(); });
        QObject::connect(cancel, &QPushButton::clicked, owner, [this] {
            if (cancellation)
                cancellation->store(true);
            if (waiting) {
                waiting = false;
                ++request;
                set_busy(false);
                status->setText(ui::text("report_dialog.status.set_text_2"));
            }
        });
        QObject::connect(export_csv, &QPushButton::clicked, owner, [this] { export_file(); });
        QObject::connect(
            table, &QTableView::doubleClicked, owner, [this](const QModelIndex &view_index) {
                const auto index = table_proxy->mapToSource(view_index);
                if (!model->result)
                    return;
                const auto &r = *model->result;
                const auto &id = r.table.columns()[index.column()].id;
                auto field = id == "access.max_elevation" ? "access.max_elevation_time"
                             : id == "access.min_range"   ? "access.min_range_time"
                                                          : id.c_str();
                const auto &source = r.source_table ? *r.source_table : r.table;
                const auto found = std::ranges::find(source.columns(), field, &results::Column::id);
                if (found != source.columns().end()) {
                    const auto &cell = source.rows()[index.row()][found - source.columns().begin()];
                    if (const auto *epoch = std::get_if<core::Epoch>(&cell)) {
                        seek_result(*epoch);
                        return;
                    }
                }
                for (const auto &cell : source.rows().at(index.row()))
                    if (const auto *epoch = std::get_if<core::Epoch>(&cell)) {
                        seek_result(*epoch);
                        break;
                    }
            });
        table->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(table, &QWidget::customContextMenuRequested, owner, [this](QPoint point) {
            const auto view_index = table->indexAt(point);
            const auto index = table_proxy->mapToSource(view_index);
            if (!index.isValid() || !model->result)
                return;
            const auto &r = *model->result;
            const auto &source = r.source_table ? *r.source_table : r.table;
            QMenu menu(owner);
            if (!table->selectionModel()->isRowSelected(view_index.row(), {}))
                table->selectRow(view_index.row());
            menu.addActions(table->actions());
            menu.addAction(ui::text("report_dialog.label_112"), owner, [this, index] {
                QApplication::clipboard()->setText(
                    text(model->result->cell_text(index.row(), index.column())));
            });
            const auto curve_field = std::ranges::find(
                curve_choices, text(r.table.columns()[index.column()].id), &ReportCurveField::id);
            if (views->isTabEnabled(1) && curve_field != curve_choices.end()) {
                const auto id = curve_field->id;
                menu.addAction(ui::text("report_dialog.id"), owner, [this, id] {
                    auto selected = curve_ids;
                    if (!selected.contains(id))
                        selected << id;
                    if (selected.size() > 6) {
                        if (!choose_curves(selected))
                            return;
                    } else
                        apply_curves(selected);
                    views->setCurrentIndex(1);
                });
                menu.addAction(ui::text("report_dialog.label_113"), owner, [this, id] {
                    apply_curves({id});
                    views->setCurrentIndex(1);
                });
            }
            if (views->isTabEnabled(1))
                menu.addAction(ui::text("report_dialog.label_114"), owner, [this] {
                    if (choose_curves())
                        views->setCurrentIndex(1);
                });
            if (!application::report_is_interval(r.definition.query.kind)) {
                menu.exec(table->viewport()->mapToGlobal(point));
                return;
            }
            menu.addSeparator();
            for (const auto &[id, label] : std::array{
                     std::pair{"time.epoch", ui::text("report_dialog.label_115")},
                     std::pair{"access.max_elevation_time", ui::text("report_dialog.label_116")},
                     std::pair{"access.min_range_time", ui::text("report_dialog.label_117")},
                     std::pair{"access.stop", ui::text("report_dialog.label_118")},
                     std::pair{"lighting.stop", ui::text("report_dialog.label_119")}}) {
                const auto field = std::ranges::find(source.columns(), id, &results::Column::id);
                if (field != source.columns().end()) {
                    const auto epoch = std::get<core::Epoch>(
                        source.rows()[index.row()][field - source.columns().begin()]);
                    menu.addAction(label, owner, [this, epoch] { seek_result(epoch); });
                }
            }
            menu.exec(table->viewport()->mapToGlobal(point));
        });
        QObject::connect(event_notice, &QLabel::linkActivated, owner, [this](const QString &link) {
            if (link == "closest-approach" && model->result &&
                model->result->intersatellite_statistics)
                seek_result(model->result->intersatellite_statistics->minimum_range.epoch);
        });
        auto *timer = new QTimer(owner);
        timer->setInterval(80);
        QObject::connect(timer, &QTimer::timeout, owner, [this] { poll(); });
        timer->start();
        fill_library();
        choose(0);
        adapt(owner->width());
    }
    ~Impl() {
        if (cancellation)
            cancellation->store(true);
    }
    void fill_library() {
        const bool saved = std::ranges::any_of(
            project.reports, [this](const auto &definition) { return definition.id == draft.id; });
        library->setText(saved ? text(draft.name) : kind_name(draft.query.kind));
        library->setIcon(workbench_icon(saved ? WorkbenchIcon::save : WorkbenchIcon::properties));
        library->setToolTip(library->text() + ui::text("report_dialog.library.set_tool_tip"));
        library->setAccessibleDescription(library->toolTip());
        remove->setEnabled(saved);
        update_template->setEnabled(saved);
        store->setText(saved ? ui::text("report_dialog.store.set_text")
                             : ui::text("report_dialog.menu.add_action"));
        refresh_scope();
    }
    QString report_scope(const std::vector<std::string> &ids, bool all) const {
        if (all)
            return ui::text("report_dialog.label_120");
        QStringList names;
        const auto objects = project_object_choices(project);
        for (const auto &id : ids) {
            const auto found = std::ranges::find(objects, id, &ObjectChoice::id);
            names << (found == objects.end() ? text(id) + ui::text("report_dialog.found")
                                             : found->path);
        }
        return names.join(QStringLiteral("、"));
    }
    std::vector<ReportChoice> report_choices() const {
        const auto description = [](ReportKind kind) -> QString {
            switch (kind) {
            case ReportKind::scenario_summary:
                return ui::text("report_dialog.label_121");
            case ReportKind::object_summary:
                return ui::text("report_dialog.label_122");
            case ReportKind::cartesian_state:
                return ui::text("report_dialog.label_123");
            case ReportKind::ground_stations:
                return ui::text("report_dialog.label_124");
            case ReportKind::classical_elements:
                return ui::text("report_dialog.label_125");
            case ReportKind::geodetic_position:
                return ui::text("report_dialog.label_126");
            case ReportKind::orbital_motion:
                return ui::text("report_dialog.label_127");
            case ReportKind::access_intervals:
                return ui::text("report_dialog.label_128");
            case ReportKind::aer:
                return ui::text("report_dialog.label_129");
            case ReportKind::lighting_intervals:
                return ui::text("report_dialog.label_130");
            case ReportKind::lighting_state:
                return ui::text("report_dialog.label_131");
            case ReportKind::orbital_events:
                return ui::text("report_dialog.label_132");
            case ReportKind::relative_motion:
                return ui::text("report_dialog.label_133");
            case ReportKind::intersatellite_access:
                return ui::text("report_dialog.label_134");
            case ReportKind::coverage_grid:
                return ui::text("report_dialog.label_135");
            case ReportKind::coverage_time:
                return ui::text("report_dialog.label_136");
            case ReportKind::state_covariance:
                return ui::text("report_dialog.label_137");
            }
            return {};
        };
        const auto output = [](ReportKind kind) -> QString {
            switch (kind) {
            case ReportKind::coverage_grid:
                return ui::text("report_dialog.output");
            case ReportKind::coverage_time:
                return ui::text("report_dialog.label_138");
            case ReportKind::scenario_summary:
                return ui::text("report_dialog.label_139");
            case ReportKind::object_summary:
                return ui::text("report_dialog.label_140");
            case ReportKind::ground_stations:
                return ui::text("report_dialog.label_141");
            case ReportKind::access_intervals:
            case ReportKind::intersatellite_access:
                return ui::text("report_dialog.label_142");
            case ReportKind::lighting_intervals:
                return ui::text("report_dialog.label_143");
            case ReportKind::orbital_events:
                return ui::text("report_dialog.label_144");
            default:
                return ui::text("report_dialog.label_145");
            }
        };
        const auto icon = [](ReportKind kind) {
            switch (kind) {
            case ReportKind::scenario_summary:
                return WorkbenchIcon::earth;
            case ReportKind::object_summary:
                return WorkbenchIcon::satellite;
            case ReportKind::ground_stations:
                return WorkbenchIcon::ground_station;
            case ReportKind::coverage_grid:
            case ReportKind::coverage_time:
            case ReportKind::geodetic_position:
                return WorkbenchIcon::map;
            case ReportKind::access_intervals:
                return WorkbenchIcon::clock;
            case ReportKind::aer:
                return WorkbenchIcon::focus;
            default:
                return WorkbenchIcon::orbit;
            }
        };
        std::vector<ReportChoice> choices;
        for (auto kind : report_kinds)
            choices.push_back({QStringLiteral("builtin:%1").arg(static_cast<int>(kind)),
                               kind_name(kind),
                               kind_name(kind),
                               description(kind),
                               report_is_intersatellite(kind) ? ui::text("report_dialog.label_146")
                               : report_is_access(kind)       ? ui::text("report_dialog.label_147")
                                                        : report_scope(scope_ids, all_objects),
                               false,
                               compatible(kind) ? QString{} : ui::text("report_dialog.label_148"),
                               output(kind),
                               {},
                               icon(kind)});
        for (const auto &definition : project.reports) {
            QString warning;
            const auto objects = project_object_choices(project);
            for (const auto &id : definition.query.object_ids)
                if (std::ranges::find(objects, id, &ObjectChoice::id) == objects.end())
                    warning = ui::text("report_dialog.label_149");
            choices.push_back({"template:" + text(definition.id),
                               text(definition.name),
                               kind_name(definition.query.kind),
                               description(definition.query.kind),
                               report_scope(definition.query.object_ids,
                                            definition.query.scope == ReportScope::all_compatible),
                               true,
                               {},
                               output(definition.query.kind),
                               ui::text("report_dialog.label_150") + warning,
                               icon(definition.query.kind)});
        }
        return choices;
    }
    void choose_report() {
        const auto choices = report_choices();
        const auto saved_key = "template:" + text(draft.id);
        const bool saved =
            std::ranges::find(choices, saved_key, &ReportChoice::key) != choices.end();
        ReportSelectionDialog chooser(
            choices,
            saved ? saved_key
                  : QStringLiteral("builtin:%1").arg(static_cast<int>(draft.query.kind)),
            owner);
        report_chooser = &chooser;
        const auto accepted = chooser.exec() == QDialog::Accepted;
        report_chooser = nullptr;
        if (!accepted)
            return;
        // Resolve against the current project: it may have changed while the window was open.
        const auto current = report_choices();
        const auto found = std::ranges::find(current, chooser.selection(), &ReportChoice::key);
        if (found == current.end() || !found->unavailable_reason.isEmpty())
            return;
        const int row = static_cast<int>(std::distance(current.begin(), found));
        if ((saved && found->key == saved_key) ||
            (!saved && row == static_cast<int>(draft.query.kind)))
            return;
        choose(row);
    }
    bool compatible(ReportKind kind) const {
        if (report_is_coverage(kind))
            return !project.satellites.empty();
        if (kind == ReportKind::state_covariance) {
            const auto eligible = [&](const std::string &id) {
                return std::ranges::any_of(project.satellites, [&](const auto &s) {
                    return s.orbit.object_id == id && s.orbit.computes_covariance();
                });
            };
            return all_objects ? std::ranges::any_of(
                                     project.satellites,
                                     [](const auto &s) { return s.orbit.computes_covariance(); })
                               : !scope_ids.empty() && std::ranges::all_of(scope_ids, eligible);
        }
        if (report_is_intersatellite(kind))
            return project.satellites.size() >= 2;
        if (report_is_access(kind))
            return !project.ground_stations.empty() && !project.satellites.empty();
        if (std::ranges::any_of(project.sensors, [&](const auto &s) {
                return std::ranges::find(scope_ids, s.object_id) != scope_ids.end();
            }))
            return false;
        if (kind == ReportKind::scenario_summary)
            return all_objects;
        if (report_satellites_only(kind) && !all_objects)
            return std::all_of(scope_ids.begin(), scope_ids.end(), [&](const auto &id) {
                return std::any_of(
                    project.satellites.begin(), project.satellites.end(),
                    [&](const auto &satellite) { return satellite.orbit.object_id == id; });
            });
        if (kind != ReportKind::ground_stations || all_objects)
            return true;
        return std::all_of(scope_ids.begin(), scope_ids.end(), [&](const auto &id) {
            return std::any_of(project.ground_stations.begin(), project.ground_stations.end(),
                               [&](const auto &station) { return station.object_id == id; });
        });
    }
    void refresh_scope() {
        QStringList labels;
        bool single_station = false;
        for (const auto &id : scope_ids) {
            QString label = text(id) + ui::text("report_dialog.found");
            for (const auto &sensor : project.sensors)
                if (sensor.object_id == id)
                    label = text(sensor.name());
            for (const auto &satellite : project.satellites)
                if (satellite.orbit.object_id == id)
                    label = text(satellite.name());
            for (const auto &station : project.ground_stations)
                if (station.object_id == id) {
                    label = text(station.name());
                    single_station = true;
                }
            labels << label;
        }
        scope_label->setText(all_objects ? ui::text("report_dialog.scope_label.set_text")
                             : labels.size() == 1
                                 ? labels.front()
                                 : ui::text("report_dialog.label_151").arg(labels.size()));
        scope_label->setToolTip(labels.join('\n'));
        scope_detail->setText(all_objects ? ui::text("report_dialog.scope_detail.set_text")
                              : labels.size() == 1
                                  ? (single_station ? ui::text("report_dialog.detail.set_text")
                                                    : ui::text("report_dialog.label_152"))
                                  : ui::text("report_dialog.label_153"));
        owner->setWindowTitle(
            scope_ids.size() == 1
                ? ui::text("report_dialog.owner.set_window_title").arg(labels.front())
                : ui::text("common.report"));
        if (report_chooser)
            report_chooser->set_choices(report_choices());
    }
    void adapt(int width) {
        const bool was_compact = compact;
        const bool settings_hidden = settings_panel->isHidden();
        compact = panel_mode.narrow(width, 800);
        settings_panel->setMinimumWidth(compact ? 0 : 290);
        settings_panel->setMaximumWidth(compact ? QWIDGETSIZE_MAX : 380);
        settings_panel->setVisible(compact ? narrow_settings : wide_settings);
        result_panel->setVisible(!compact || !narrow_settings);
        // A panel first shown inside a narrow dock may retain a zero splitter size.
        // Allocate the single visible pane explicitly; keep the user's wide split intact.
        if (compact)
            split->setSizes(narrow_settings ? QList<int>{width, 0} : QList<int>{0, width});
        else if (!wide_settings)
            split->setSizes({0, width});
        else if (wide_settings && (was_compact || settings_hidden))
            split->setSizes(wide_sizes);
        view_button->setText(compact ? (narrow_settings
                                            ? ui::text("report_dialog.view_button.set_text")
                                            : ui::text("report_dialog.view_button.set_text_2"))
                                     : (wide_settings ? ui::text("report_dialog.label_83")
                                                      : ui::text("report_dialog.label_154")));
    }
    void update_interval() {
        for (auto *editor : {first, last})
            editor->findChild<QLineEdit *>("epochValue")->setMinimumWidth(180);
        const bool scenario = follow->isChecked();
        custom_times->setVisible(!scenario);
        interval_label->setVisible(scenario);
        if (scenario) {
            first->set_epoch(project.first);
            last->set_epoch(project.last);
        }
        interval_label->setText(first->findChild<QLineEdit *>("epochValue")->text() + "\n" +
                                last->findChild<QLineEdit *>("epochValue")->text());
    }
    void choose_scope() {
        auto choices =
            project_object_choices(project, {ObjectKind::satellite, ObjectKind::ground_station});
        if (report_is_coverage(draft.query.kind))
            choices = project_object_choices(project, {ObjectKind::satellite, ObjectKind::sensor});
        else if (draft.query.kind == ReportKind::ground_stations)
            choices = project_object_choices(project, {ObjectKind::ground_station});
        else if (report_satellites_only(draft.query.kind))
            choices = project_object_choices(project, {ObjectKind::satellite});
        ObjectSelectionDialog chooser(std::move(choices),
                                      {ui::text("report_dialog.label_155"), true,
                                       report_is_coverage(draft.query.kind)
                                           ? QString{}
                                           : ui::text("report_dialog.label_156")},
                                      {scope_ids, all_objects}, owner);
        if (chooser.exec() != QDialog::Accepted)
            return;
        scope_ids = chooser.selection().ids;
        all_objects = chooser.selection().all;
        refresh_scope();
        if (!compatible(draft.query.kind)) {
            const auto next = draft.query.kind == ReportKind::scenario_summary
                                  ? ReportKind::object_summary
                                  : ReportKind::cartesian_state;
            choose(static_cast<int>(next));
        } else
            edit();
    }
    void show_source() {
        if (!model->result)
            return;
        const auto &result = *model->result;
        QDialog dialog(owner);
        dialog.setWindowTitle(ui::text("report_dialog.result"));
        dialog.setObjectName("reportSourceDialog");
        dialog.resize(640, 400);
        auto *layout = new QVBoxLayout(&dialog);
        auto *source = new QPlainTextEdit(&dialog);
        source->setObjectName("reportSource");
        source->setReadOnly(true);
        source->setPlainText(ui::text("report_dialog.source.set_plain_text")
                                 .arg(text(result.definition.name), text(result.algorithm),
                                      text(result.input_sha256), text(result.data_sha256),
                                      text(result.time_sha256)));
        layout->addWidget(source, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        buttons->button(QDialogButtonBox::Close)->setText(ui::text("common.close"));
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.exec();
    }
    void fill_access_pair(bool preserve_missing) {
        const QSignalBlocker station_guard(access_station), target_guard(access_target);
        const bool intersatellite = report_is_intersatellite(draft.query.kind);
        auto *form = static_cast<QFormLayout *>(access_pair->layout());
        static_cast<QLabel *>(form->labelForField(access_station))
            ->setText(intersatellite ? ui::text("report_dialog.label_157")
                                     : ui::text("report_dialog.label_52"));
        static_cast<QLabel *>(form->labelForField(access_target))
            ->setText(intersatellite ? ui::text("report_dialog.label_158")
                                     : ui::text("report_dialog.pair_form.add_row"));
        access_station->set_options({intersatellite ? ui::text("report_dialog.label_159")
                                                    : ui::text("report_dialog.label_86"),
                                     false,
                                     {}});
        access_target->set_options({intersatellite ? ui::text("report_dialog.label_160")
                                                   : ui::text("report_dialog.label_87"),
                                    false,
                                    {}});
        access_station->setAccessibleName(
            intersatellite ? ui::text("report_dialog.label_157")
                           : ui::text("report_dialog.access_station.set_accessible_name"));
        access_target->setAccessibleName(
            intersatellite ? ui::text("report_dialog.label_158")
                           : ui::text("report_dialog.access_target.set_accessible_name"));
        access_station->set_objects(project_object_choices(
            project, intersatellite ? std::initializer_list{ObjectKind::satellite}
                                    : std::initializer_list{ObjectKind::ground_station}));
        access_target->set_objects(project_object_choices(
            project, intersatellite
                         ? std::initializer_list{ObjectKind::satellite}
                         : std::initializer_list{ObjectKind::satellite, ObjectKind::sensor}));
        if (scope_ids.size() == 2 &&
            (preserve_missing ||
             (access_station->contains(scope_ids[0]) && access_target->contains(scope_ids[1])))) {
            access_station->set_id(scope_ids[0]);
            access_target->set_id(scope_ids[1]);
        } else {
            for (auto *field : {access_station, access_target}) {
                field->set_id(field->objects().empty() ? "" : field->objects().front().id);
                for (const auto &id : scope_ids)
                    if (field->contains(id)) {
                        field->set_id(id);
                        break;
                    }
            }
            if (intersatellite && access_station->id() == access_target->id())
                for (const auto &object : access_target->objects())
                    if (object.id != access_station->id()) {
                        access_target->set_id(object.id);
                        break;
                    }
        }
        scope_ids = {access_station->id(), access_target->id()};
        all_objects = false;
    }
    void choose(int row) {
        if (row < 0 || loading)
            return;
        if (row < builtin_count && !compatible(static_cast<ReportKind>(row)))
            return;
        loading = true;
        if (row < builtin_count) {
            draft = {};
            draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            draft.query.kind = static_cast<ReportKind>(row);
            draft.name = kind_name(draft.query.kind).toStdString();
            draft.query.first = project.first;
            draft.query.last = project.last;
            draft.query.object_ids = scope_ids;
            if (report_is_access(draft.query.kind))
                draft.query.step_nanoseconds = draft.query.kind == ReportKind::access_intervals
                                                   ? 60'000'000'000
                                                   : 1'000'000'000;
        } else {
            draft = project.reports.at(static_cast<std::size_t>(row - builtin_count));
            scope_ids = draft.query.object_ids;
            all_objects = draft.query.scope == ReportScope::all_compatible;
        }
        const bool coverage = report_is_coverage(draft.query.kind);
        coverage_settings->setVisible(coverage);
        if (coverage && row < builtin_count) {
            if (all_objects)
                scope_ids.clear();
            const auto choices =
                project_object_choices(project, {ObjectKind::satellite, ObjectKind::sensor});
            std::erase_if(scope_ids, [&](const auto &id) {
                return std::ranges::find(choices, id, &ObjectChoice::id) == choices.end();
            });
            if (scope_ids.empty() && !project.satellites.empty())
                scope_ids.push_back(project.satellites.front().orbit.object_id);
            all_objects = false;
        }
        const auto &grid = draft.query.coverage;
        const std::array angles{grid.south_rad, grid.north_rad,      grid.west_rad,
                                grid.east_rad,  grid.resolution_rad, grid.minimum_elevation_rad};
        for (int i = 0; i < 6; ++i) {
            coverage_fields[i]->setValue(angles[i] * 180 / std::numbers::pi);
            coverage_fields[i]->setProperty("originalRadians", angles[i]);
            coverage_fields[i]->setProperty("originalDisplay", coverage_fields[i]->value());
        }
        coverage_ends->setChecked(grid.include_end_gaps);
        const bool access = pair_report(draft.query.kind);
        access_options->setVisible(report_uses_event_search(draft.query.kind));
        access_mode->setCurrentIndex(static_cast<int>(draft.query.event_search_mode));
        access_tolerance->setValue(static_cast<double>(draft.query.event_tolerance_nanoseconds) /
                                   1e9);
        step_label->setText(report_uses_event_search(draft.query.kind)
                                ? (access_mode->currentIndex() == 0
                                       ? ui::text("report_dialog.step_label.set_text")
                                       : ui::text("report_dialog.label.set_fixed_width"))
                                : ui::text("report_dialog.label_95"));
        step->setToolTip(report_uses_event_search(draft.query.kind)
                             ? ui::text("report_dialog.step.set_tool_tip_2")
                             : ui::text("report_dialog.step.set_tool_tip"));
        if (draft.query.kind == ReportKind::relative_motion) {
            step_label->setText(ui::text("report_dialog.step_label.set_text_2"));
            step->setToolTip(ui::text("report_dialog.step.set_tool_tip_3"));
        }
        step->setMinimum(access ? .001 : .000000001);
        access_pair->setVisible(access);
        scope_label->setVisible(!access);
        scope_button->setVisible(!access);
        scope_detail->setVisible(!access);
        if (access)
            fill_access_pair(row >= builtin_count);
        follow->setChecked(draft.query.follow_scenario);
        first->set_epoch(draft.query.follow_scenario ? project.first : draft.query.first);
        last->set_epoch(draft.query.follow_scenario ? project.last : draft.query.last);
        step->setValue(static_cast<double>(draft.query.step_nanoseconds) / 1e9);
        frame->setCurrentIndex(draft.query.frame == core::FrameId::gcrf ? 0 : 1);
        if (draft.query.kind == ReportKind::geodetic_position)
            frame->setCurrentIndex(1);
        if (report_satellites_only(draft.query.kind))
            frame->setCurrentIndex(0);
        frame_field->setVisible(draft.query.kind == ReportKind::cartesian_state);
        units->setItemText(0, draft.query.kind == ReportKind::lighting_state
                                  ? ui::text("report_dialog.units.set_item_text")
                                  : QStringLiteral("km / km·s⁻¹ / °"));
        units->setItemText(1, draft.query.kind == ReportKind::lighting_state
                                  ? ui::text("report_dialog.units.set_item_text_2")
                                  : QStringLiteral("m / m·s⁻¹ / rad"));
        units->setCurrentIndex(draft.format.units == ReportUnits::engineering ? 0 : 1);
        scale->setCurrentIndex(static_cast<int>(draft.format.time_scale));
        update_interval();
        refresh_scope();
        sampling->setVisible(report_is_sampled(draft.query.kind));
        units_field->setVisible(draft.query.kind != ReportKind::lighting_intervals &&
                                (report_is_sampled(draft.query.kind) ||
                                 draft.query.kind == ReportKind::ground_stations));
        scale_field->setVisible(report_is_sampled(draft.query.kind) ||
                                draft.query.kind == ReportKind::scenario_summary);
        format_title->setVisible(draft.query.kind != ReportKind::object_summary);
        template_description->setText(
            draft.query.kind == ReportKind::scenario_summary
                ? ui::text("report_dialog.template_description.set_text")
            : draft.query.kind == ReportKind::object_summary  ? ui::text("report_dialog.label_161")
            : draft.query.kind == ReportKind::cartesian_state ? ui::text("report_dialog.label_162")
            : draft.query.kind == ReportKind::classical_elements
                ? ui::text("report_dialog.label_163")
            : draft.query.kind == ReportKind::geodetic_position
                ? ui::text("report_dialog.label_164")
            : draft.query.kind == ReportKind::orbital_motion ? ui::text("report_dialog.label_165")
                                                             : ui::text("report_dialog.label_166"));
        template_description->setToolTip(template_description->text());
        if (draft.query.kind == ReportKind::classical_elements)
            template_description->setText(
                ui::text("report_dialog.template_description.set_text_2"));
        if (draft.query.kind == ReportKind::state_covariance)
            template_description->setText(
                ui::text("report_dialog.template_description.set_text_3"));
        if (draft.query.kind == ReportKind::orbital_motion)
            template_description->setText(
                ui::text("report_dialog.template_description.set_text_4"));
        if (draft.query.kind == ReportKind::geodetic_position)
            template_description->setText(
                ui::text("report_dialog.template_description.set_text_5"));
        if (draft.query.kind == ReportKind::orbital_events) {
            template_description->setText(
                ui::text("report_dialog.template_description.set_text_6"));
            template_description->setToolTip(
                ui::text("report_dialog.template_description.set_tool_tip"));
        }
        if (report_is_intersatellite(draft.query.kind)) {
            template_description->setText(
                draft.query.kind == ReportKind::relative_motion
                    ? ui::text("report_dialog.template_description.set_text_7")
                    : ui::text("report_dialog.label_167"));
            template_description->setToolTip(
                ui::text("report_dialog.template_description.set_tool_tip_2"));
        }
        if (coverage) {
            template_description->setText(
                ui::text("report_dialog.template_description.set_text_8"));
            units->setItemText(0, QStringLiteral("km² / ° / %"));
            units->setItemText(1, ui::text("report_dialog.units.set_item_text_3"));
        }
        if (report_is_lighting(draft.query.kind))
            template_description->setText(ui::text("report_dialog.template_description.set_text_9")
                                              .arg(text(draft.query.lighting_ephemeris)));
        if (report_is_access(draft.query.kind))
            template_description->setText(
                draft.query.kind == ReportKind::access_intervals
                    ? ui::text("report_dialog.template_description.set_text_10")
                    : ui::text("report_dialog.label_168"));
        remove->setEnabled(row >= builtin_count);
        update_template->setEnabled(row >= builtin_count);
        store->setText(row >= builtin_count ? ui::text("report_dialog.store.set_text")
                                            : ui::text("report_dialog.menu.add_action"));
        fill_library();
        loading = false;
        edit();
    }
    void seek_result(core::Epoch epoch) {
        if (epoch.tai_nanoseconds() < project.first.tai_nanoseconds() ||
            epoch.tai_nanoseconds() > project.last.tai_nanoseconds())
            status->setText(ui::text("report_dialog.status.set_text_3"));
        else
            seek(epoch);
    }
    ReportDefinition definition() const {
        auto d = draft;
        d.query.object_ids =
            d.query.kind == ReportKind::scenario_summary ? std::vector<std::string>{} : scope_ids;
        d.query.scope = all_objects ? ReportScope::all_compatible : ReportScope::objects;
        if (pair_report(d.query.kind))
            d.query.object_ids = {access_station->id(), access_target->id()};
        d.query.follow_scenario = follow->isChecked();
        if (report_is_sampled(d.query.kind) && !d.query.follow_scenario) {
            d.query.first = first->epoch();
            d.query.last = last->epoch();
        }
        d.query.step_nanoseconds = static_cast<std::int64_t>(std::llround(step->value() * 1e9));
        d.query.event_search_mode =
            static_cast<core::AccessSearchMode>(access_mode->currentIndex());
        d.query.event_tolerance_nanoseconds =
            static_cast<std::int64_t>(std::llround(access_tolerance->value() * 1e9));
        d.query.frame = frame->currentIndex() == 0 ? core::FrameId::gcrf : core::FrameId::itrf;
        d.format.units = units->currentIndex() == 0 ? ReportUnits::engineering : ReportUnits::si;
        d.format.time_scale = static_cast<ReportTimeScale>(scale->currentIndex());
        if (d.format.columns.empty())
            d.format.columns = default_columns(d.query.kind);
        auto &g = d.query.coverage;
        const auto angle = [&](int i) {
            const auto *field = coverage_fields[i];
            return field->value() == field->property("originalDisplay").toDouble()
                       ? field->property("originalRadians").toDouble()
                       : field->value() * std::numbers::pi / 180;
        };
        g.south_rad = angle(0);
        g.north_rad = angle(1);
        g.west_rad = angle(2);
        g.east_rad = angle(3);
        g.resolution_rad = angle(4);
        g.minimum_elevation_rad = angle(5);
        g.include_end_gaps = coverage_ends->isChecked();
        validate_report_definition(d);
        return d;
    }
    void edit() {
        if (loading)
            return;
        changed = true;
        update_estimate();
        if (model->result && !job.valid() && !waiting) {
            try {
                const auto d = definition();
                if (d.query == model->result->definition.query &&
                    d.id == model->result->definition.id && result_is_current()) {
                    auto result = std::make_shared<runtime::ReportResult>(
                        model->result->with_template(d.format));
                    result->definition = d;
                    publish(result, false);
                    status->setText(ui::text("report_dialog.status.set_text_4"));
                }
            } catch (const std::exception &error) {
                status->setText(ui::text("report_dialog.status.set_text_5")
                                    .arg(QString::fromUtf8(error.what())));
            }
        }
        result_status();
    }
    void fit_table_columns() {
        table->resizeColumnsToContents();
        if (!model->result)
            return;
        const auto &columns = model->result->table.columns();
        for (std::size_t c = 0; c < columns.size(); ++c) {
            const auto &column = columns[c];
            // Long object names must not consume the whole viewport; exact text
            // remains in tooltips and copies. Date columns retain their measured width.
            if (column.unit.empty())
                table->setColumnWidth(static_cast<int>(c),
                                      std::clamp(table->columnWidth(static_cast<int>(c)), 90, 180));
        }
    }
    void choose_columns() {
        if (!model->result || !choose_columns_action->isEnabled())
            return;
        try {
            const auto snapshot = model->result;
            const auto generation = input_generation;
            const auto current = definition();
            const auto source = complete_plot_result(*snapshot, retained_column_specs);
            std::vector<ReportColumnChoice> choices;
            for (std::size_t i = 0; i < source->table.columns().size(); ++i) {
                const auto &column = source->table.columns()[i];
                const auto &specs = source->definition.format.columns;
                const auto spec = std::ranges::find(specs, column.id, &ReportColumnSpec::field);
                choices.push_back({text(column.id),
                                   spec != specs.end() && !spec->title.empty()
                                       ? text(spec->title)
                                       : localized(text(column.title)),
                                   text(source->column_unit(i))});
            }
            QStringList selected, defaults;
            for (const auto &column : current.format.columns)
                selected << text(column.field);
            for (const auto &column : default_columns(current.query.kind))
                defaults << text(column.field);
            ReportColumnSelectionDialog chooser(std::move(choices), selected, defaults, owner);
            if (chooser.exec() != QDialog::Accepted)
                return;
            const auto live = definition();
            if (model->result != snapshot || input_generation != generation ||
                live.id != current.id || live.query != current.query ||
                live.format != current.format || !result_is_current()) {
                status->setText(ui::text("report_dialog.status.set_text_6"));
                return;
            }
            if (chooser.selection() == selected)
                return;
            auto next = current;
            next.format.columns.clear();
            for (const auto &id : chooser.selection()) {
                const auto found = retained_column_specs.find(id.toStdString());
                next.format.columns.push_back(found == retained_column_specs.end()
                                                  ? ReportColumnSpec{id.toStdString(), {}, {}}
                                                  : found->second);
            }
            auto result =
                std::make_shared<runtime::ReportResult>(snapshot->with_template(next.format));
            result->definition = next;
            // Validate the complete new presentation before updating the configuration.
            draft.format = next.format;
            publish(std::move(result), false, false);
            status->setText(ui::text("report_dialog.status.set_text_7"));
        } catch (const std::exception &error) {
            status->setText(
                ui::text("report_dialog.status.set_text_8").arg(QString::fromUtf8(error.what())));
        }
    }
    void publish(std::shared_ptr<runtime::ReportResult> result, bool new_calculation = true,
                 bool update_plot = true) {
        std::string sort_field;
        const auto sort_order = table->horizontalHeader()->sortIndicatorOrder();
        std::vector<int> selected_rows;
        const int scroll = table->verticalScrollBar()->value();
        const int horizontal_scroll = table->horizontalScrollBar()->value();
        int current_row = -1;
        std::string current_field;
        if (model->result &&
            model->result->definition.query.kind == result->definition.query.kind) {
            const auto &columns = model->result->table.columns();
            for (std::size_t i = 0; i < columns.size(); ++i)
                column_widths.insert_or_assign(columns[i].id,
                                               table->columnWidth(static_cast<int>(i)));
            if (!new_calculation) {
                const auto current = table_proxy->mapToSource(table->currentIndex());
                if (current.isValid()) {
                    current_row = current.row();
                    current_field = columns[current.column()].id;
                }
                const auto sorted = table->horizontalHeader()->sortIndicatorSection();
                if (sorted >= 0 && static_cast<std::size_t>(sorted) < columns.size())
                    sort_field = columns[sorted].id;
                for (const auto &row : table->selectionModel()->selectedRows())
                    selected_rows.push_back(table_proxy->mapToSource(row).row());
            }
        } else {
            column_widths.clear();
        }
        if (new_calculation)
            retained_column_specs.clear();
        for (const auto &column : result->definition.format.columns)
            retained_column_specs.insert_or_assign(column.field, column);
        table->sortByColumn(-1, Qt::AscendingOrder);
        model->publish(result);
        if (new_calculation && owner->result_published && result_is_current())
            owner->result_published(result, *result_analysis);
        changed = false;
        pass_summary->setVisible(result->access_statistics.has_value() ||
                                 result->lighting_statistics.has_value() ||
                                 result->orbital_event_statistics.has_value() ||
                                 result->intersatellite_statistics.has_value() ||
                                 result->coverage_statistics.has_value());
        event_notice->hide();
        for (auto *value : pass_values)
            value->setToolTip({});
        if (result->access_statistics) {
            const std::array captions{
                ui::text("report_dialog.label_103"), ui::text("report_dialog.label_104"),
                ui::text("report_dialog.label_105"), ui::text("report_dialog.label_106")};
            for (int i = 0; i < 4; ++i)
                pass_captions[i]->setText(captions[i]);
            const auto &s = *result->access_statistics;
            pass_values[0]->setText(QString::number(s.count));
            const auto duration = [](double seconds) {
                return seconds >= 3600 ? QStringLiteral("%1 h").arg(seconds / 3600, 0, 'f', 2)
                       : seconds >= 60 ? QStringLiteral("%1 min").arg(seconds / 60, 0, 'f', 2)
                                       : QStringLiteral("%1 s").arg(seconds, 0, 'f', 3);
            };
            pass_values[1]->setText(duration(s.total_duration_s));
            pass_values[2]->setText(s.count ? duration(s.longest_duration_s) : QStringLiteral("—"));
            pass_values[3]->setText(s.count ? duration(s.mean_duration_s) : QStringLiteral("—"));
            pass_summary->setToolTip(ui::text("report_dialog.pass_summary.set_tool_tip")
                                         .arg(s.count ? duration(s.shortest_duration_s)
                                                      : ui::text("report_dialog.label_169")));
        }
        if (result->intersatellite_statistics) {
            const auto &s = *result->intersatellite_statistics;
            const std::array captions{
                ui::text("report_dialog.label_70"), ui::text("report_dialog.s"),
                ui::text("report_dialog.label_170"), ui::text("report_dialog.label_104")};
            for (int i = 0; i < 4; ++i)
                pass_captions[i]->setText(captions[i]);
            const bool si = result->definition.format.units == ReportUnits::si;
            pass_values[0]->setText(QStringLiteral("%1 %2")
                                        .arg(s.minimum_range.value / (si ? 1 : 1000), 0, 'f', 3)
                                        .arg(si ? "m" : "km"));
            pass_values[1]->setText(QStringLiteral("%1 %2")
                                        .arg(s.maximum_range.value / (si ? 1 : 1000), 0, 'f', 3)
                                        .arg(si ? "m" : "km"));
            pass_values[2]->setText(QString::number(s.visibility.count));
            pass_values[3]->setText(
                QStringLiteral("%1 min").arg(s.visibility.total_duration_s / 60, 0, 'f', 2));
            pass_summary->setToolTip(ui::text("report_dialog.pass_summary.set_tool_tip_2"));
            event_notice->setText(ui::text("report_dialog.event_notice.set_text"));
            event_notice->show();
        }
        if (result->lighting_statistics) {
            const auto &s = *result->lighting_statistics;
            const std::array captions{ui::text("report_dialog.s_2"), ui::text("report_dialog.s_3"),
                                      ui::text("report_dialog.label_171"),
                                      ui::text("report_dialog.label_172")};
            for (int i = 0; i < 4; ++i)
                pass_captions[i]->setText(captions[i]);
            for (int i = 0; i < 3; ++i) {
                pass_values[i]->setText(
                    QStringLiteral("%1 %").arg(100 * s.phases[i].fraction, 0, 'f', 2));
                pass_values[i]->setToolTip(ui::text("report_dialog.label_173")
                                               .arg(s.phases[i].count)
                                               .arg(s.phases[i].total_duration_s, 0, 'f', 6));
            }
            pass_values[3]->setText(
                QStringLiteral("%1 min").arg(s.phases[2].longest_duration_s / 60, 0, 'f', 2));
            pass_summary->setToolTip(
                ui::text("report_dialog.pass_summary.set_tool_tip_3").arg(s.object_count));
        }
        if (result->orbital_event_statistics) {
            const auto &s = *result->orbital_event_statistics;
            const std::array captions{
                ui::text("report_dialog.label_55"), ui::text("report_dialog.label_56"),
                ui::text("report_dialog.label_57"), ui::text("report_dialog.label_58")};
            for (int i = 0; i < 4; ++i) {
                pass_captions[i]->setText(captions[i]);
                pass_values[i]->setText(QString::number(s.counts[i]));
            }
            pass_summary->setToolTip(
                ui::text("report_dialog.pass_summary.set_tool_tip_4").arg(s.object_count));
            QStringList notes;
            if (s.near_circular_objects)
                notes << ui::text("report_dialog.label_174").arg(s.near_circular_objects);
            if (s.near_equatorial_objects)
                notes << ui::text("report_dialog.label_175").arg(s.near_equatorial_objects);
            event_notice->setText(notes.join(QStringLiteral("；")));
            event_notice->setVisible(!notes.isEmpty());
        }
        views->setTabEnabled(2, result->coverage_statistics.has_value());
        views->setTabVisible(2, views->isTabEnabled(2));
        coverage_map->set_result(result, new_calculation);
        if (result->coverage_statistics) {
            const auto &s = *result->coverage_statistics;
            const std::array captions{ui::text("report_dialog.s_4"), ui::text("report_dialog.s_5"),
                                      ui::text("report_dialog.label_176"),
                                      ui::text("report_dialog.label_177")};
            for (int i = 0; i < 4; ++i)
                pass_captions[i]->setText(captions[i]);
            pass_values[0]->setText(QString::number(s.points.size()));
            pass_values[1]->setText(QStringLiteral("%1 %").arg(
                100 * s.ever_covered_area_m2 / s.total_area_m2, 0, 'f', 2));
            pass_values[2]->setText(
                QStringLiteral("%1 %").arg(100 * s.mean_coverage_fraction, 0, 'f', 2));
            pass_values[3]->setText(
                QStringLiteral("%1 h").arg(s.maximum_gap_seconds / 3600, 0, 'f', 2));
            pass_summary->setToolTip(ui::text("report_dialog.pass_summary.set_tool_tip_5"));
            select_coverage_point(coverage_map->point_index());
        }
        fit_table_columns();
        for (std::size_t c = 0; c < result->table.columns().size(); ++c) {
            const auto &id = result->table.columns()[c].id;
            table->setColumnHidden(static_cast<int>(c), false);
            if (column_widths.contains(id))
                table->setColumnWidth(static_cast<int>(c), column_widths.at(id));
            if (id == sort_field)
                table->sortByColumn(static_cast<int>(c), sort_order);
        }
        for (const auto row : selected_rows) {
            const auto index = table_proxy->mapFromSource(model->index(row, 0));
            if (index.isValid())
                table->selectionModel()->select(index, QItemSelectionModel::Select |
                                                           QItemSelectionModel::Rows);
        }
        if (!new_calculation) {
            if (current_row >= 0) {
                const auto &columns = result->table.columns();
                const auto field = std::ranges::find(columns, current_field, &results::Column::id);
                const int column =
                    field == columns.end() ? 0 : static_cast<int>(field - columns.begin());
                table->selectionModel()->setCurrentIndex(
                    table_proxy->mapFromSource(model->index(current_row, column)),
                    QItemSelectionModel::NoUpdate);
            }
            table->verticalScrollBar()->setValue(scroll);
            table->horizontalScrollBar()->setValue(horizontal_scroll);
        }
        result_status();
        if (update_plot)
            prepare_plot(!new_calculation);
        if (new_calculation && result->coverage_statistics)
            views->setCurrentIndex(result->definition.query.kind == ReportKind::coverage_grid ? 2
                                                                                              : 1);
    }
    void select_coverage_point(std::size_t index) {
        if (!model->result || !model->result->coverage_statistics)
            return;
        const auto &s = *model->result->coverage_statistics;
        if (index >= s.points.size())
            return;
        const auto &p = s.points[index];
        coverage_point->setText(
            ui::text("report_dialog.coverage_point.set_text")
                .arg(index + 1)
                .arg(p.cell.location.longitude_rad * 180 / std::numbers::pi, 0, 'f', 3)
                .arg(p.cell.location.latitude_rad * 180 / std::numbers::pi, 0, 'f', 3)
                .arg(p.cell.area_m2 / 1e6, 0, 'f', 1));
        coverage_windows->clear();
        coverage_starts.clear();
        for (const auto &w : p.windows) {
            coverage_starts.push_back(w.first);
            const auto stamp = [&](core::Epoch e) {
                return QString::number(e.seconds_since(model->result->first), 'f', 3);
            };
            coverage_windows->addItem(QStringLiteral("+%1 → +%2 s · %3 s%4")
                                          .arg(stamp(w.first), stamp(w.last))
                                          .arg(w.last.seconds_since(w.first), 0, 'f', 3)
                                          .arg(w.start_clipped || w.stop_clipped
                                                   ? ui::text("report_dialog.label_178")
                                                   : QString{}));
        }
        if (p.windows.empty())
            coverage_windows->addItem(ui::text("report_dialog.coverage_windows.add_item"));
        coverage_windows->setToolTip(ui::text("report_dialog.coverage_windows.set_tool_tip"));
        coverage_jump->setEnabled(!p.windows.empty());
    }
    void update_estimate() {
        try {
            const auto d = definition();
            const auto rows = runtime::estimate_report_rows(project, d);
            if (report_is_coverage(d.query.kind)) {
                estimate->setText(ui::text("report_dialog.estimate.set_text")
                                      .arg(core::coverage_grid(d.query.coverage).size())
                                      .arg(d.query.object_ids.size())
                                      .arg(rows));
                return;
            }
            estimate->setText((report_uses_event_search(d.query.kind)
                                   ? ui::text("report_dialog.estimate.set_text_2")
                                   : ui::text("report_dialog.label_179"))
                                  .arg(static_cast<qulonglong>(rows))
                                  .arg(report_is_sampled(d.query.kind)
                                           ? ui::text("report_dialog.label_180")
                                           : QString{}));
        } catch (const std::exception &error) {
            estimate->setText(QString::fromUtf8(error.what()));
        }
    }
    bool result_is_current() const {
        // The report hash also records its actual resources, including a newly selected
        // time table. Compare the captured document and providers with the live inputs.
        return result_analysis &&
               runtime::report_input_sha256(*result_analysis->snapshot().project) ==
                   runtime::report_input_sha256(project) &&
               same_time(result_analysis->data().time(), time);
    }
    void result_status() {
        if (!model->result)
            return;
        const auto &r = *model->result;
        const bool stale = !result_is_current();
        bool editable = false;
        try {
            const auto current = definition();
            editable = !stale && !job.valid() && !waiting && !export_job.valid() &&
                       current.id == r.definition.id && current.query == r.definition.query;
        } catch (const std::exception &) {
        }
        choose_columns_action->setEnabled(editable);
        result_title->setText(text(r.definition.name));
        result_title->setToolTip(text(r.definition.name));
        result_label->setText(ui::text("report_dialog.result_label.set_text")
                                  .arg(static_cast<qulonglong>(r.table.rows().size()))
                                  .arg(stale     ? ui::text("report_dialog.label_181")
                                       : changed ? ui::text("report_dialog.label_182")
                                                 : ui::text("report_dialog.label_183")));
    }
    void set_busy(bool value) {
        choose_columns_action->setEnabled(!value && model->result && result_is_current() &&
                                          !changed);
        settings->setEnabled(!value);
        library->setEnabled(!value);
        generate->setEnabled(!value);
        store->setEnabled(!value);
        update_template->setEnabled(
            !value && std::any_of(project.reports.begin(), project.reports.end(),
                                  [this](const auto &r) { return r.id == draft.id; }));
        remove->setEnabled(!value &&
                           std::any_of(project.reports.begin(), project.reports.end(),
                                       [this](const auto &r) { return r.id == draft.id; }));
        export_csv->setEnabled(!value && bool(model->result));
        refresh_button->setEnabled(!value && bool(model->result));
        source_button->setEnabled(bool(model->result));
        result_more->setEnabled(bool(model->result));
        config_menu->setEnabled(!value);
        cancel->setVisible(value);
        progress->setVisible(value);
    }
    void start() {
        try {
            const auto d = definition();
            expected = runtime::estimate_report_rows(project, d);
            waiting = true;
            const auto ticket = ++request;
            set_busy(true);
            progress->setRange(0, 0);
            status->setText(ui::text("report_dialog.status.set_text_9"));
            const QPointer<ReportDialog> guard(owner);
            prepare(d.query, [guard, ticket, d](std::optional<ReportInput> context, QString error) {
                if (!guard)
                    return;
                auto &self = *guard->impl_;
                if (ticket != self.request || !self.waiting)
                    return;
                self.waiting = false;
                if (!context) {
                    self.set_busy(false);
                    self.status->setText(error);
                    return;
                }
                try {
                    if (runtime::report_input_sha256(context->project) !=
                            runtime::report_input_sha256(self.project) ||
                        !same_time(context->data.time(), self.time)) {
                        self.set_busy(false);
                        self.status->setText(ui::text("report_dialog.status.set_text_10"));
                        return;
                    }
                    self.expected = runtime::estimate_report_rows(context->project, d);
                    self.job_generation = self.input_generation;
                    self.cancellation = std::make_shared<std::atomic_bool>(false);
                    self.completed = std::make_shared<std::atomic_size_t>(0);
                    self.progress->setRange(0, static_cast<int>(self.expected));
                    self.progress->setValue(0);
                    self.status->setText(ui::text("report_dialog.status.set_text_11"));
                    auto retained = self.result_analysis;
                    if (retained && (retained->data().data_lock() != context->data.data_lock() ||
                                     retained->data().frames() != context->data.frames() ||
                                     analysis_reuse_key(*retained->snapshot().project) !=
                                         analysis_reuse_key(context->project)))
                        retained.reset();
                    self.job = std::async(std::launch::async, [snapshot = std::move(*context), d,
                                                               retained = std::move(retained),
                                                               cancel = self.cancellation,
                                                               count = self.completed]() mutable {
                        if (cancel->load())
                            throw runtime::AnalysisCancelled{};
                        auto analysis =
                            retained
                                ? std::move(*retained)
                                : runtime::prepare(
                                      application::ProjectDocument{std::move(snapshot.project)},
                                      std::move(snapshot.data), std::move(snapshot.sessions));
                        auto result = analysis.report(
                            d, [cancel] { return cancel->load(); },
                            [count](std::size_t current, std::size_t) { count->store(current); });
                        return GeneratedReport{std::move(analysis), std::move(result)};
                    });
                } catch (const std::exception &failure) {
                    self.set_busy(false);
                    self.status->setText(ui::text("report_dialog.status.set_text_12")
                                             .arg(QString::fromUtf8(failure.what())));
                }
            });
        } catch (const std::exception &error) {
            waiting = false;
            set_busy(false);
            status->setText(QString::fromUtf8(error.what()));
        }
    }
    void persist(bool as_copy = false) {
        try {
            auto d = definition();
            if (as_copy) {
                QInputDialog naming(owner);
                naming.setObjectName("reportTemplateDialog");
                naming.setWindowTitle(ui::text("report_dialog.label_184"));
                naming.setLabelText(ui::text("report_dialog.label_185"));
                naming.setTextValue(text(d.name));
                naming.setOkButtonText(ui::text("report_dialog.label_186"));
                naming.setCancelButtonText(ui::text("common.cancel"));
                if (naming.exec() != QDialog::Accepted)
                    return;
                d.name = naming.textValue().trimmed().toStdString();
                d.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                validate_report_definition(d);
            }
            auto reports = project.reports;
            auto found = std::find_if(reports.begin(), reports.end(),
                                      [&](const auto &r) { return r.id == d.id; });
            if (found == reports.end())
                reports.push_back(d);
            else
                *found = d;
            save(reports);
            project.reports = std::move(reports);
            draft = d;
            if (model->result && result_is_current() &&
                model->result->definition.query == d.query &&
                model->result->definition.format == d.format) {
                // Naming a matching result does not invalidate its sampled data or chart.
                auto presentation = std::make_shared<runtime::ReportResult>(*model->result);
                presentation->definition = d;
                publish(std::move(presentation), false, false);
            } else if (as_copy) {
                changed = true;
            }
            fill_library();
            remove->setEnabled(true);
            update_template->setEnabled(true);
            store->setText(ui::text("report_dialog.store.set_text"));
            result_status();
            status->setText(ui::text("report_dialog.status.set_text_13"));
        } catch (const std::exception &error) {
            status->setText(QString::fromUtf8(error.what()));
        }
    }
    void erase() {
        try {
            auto reports = project.reports;
            std::erase_if(reports, [this](const auto &r) { return r.id == draft.id; });
            save(reports);
            project.reports = std::move(reports);
            fill_library();
            choose(static_cast<int>(compatible(draft.query.kind) ? draft.query.kind
                                                                 : ReportKind::object_summary));
            status->setText(ui::text("report_dialog.status.set_text_14"));
        } catch (const std::exception &error) {
            status->setText(QString::fromUtf8(error.what()));
        }
    }
    void select_view() {
        const bool chart = views->currentIndex() > 0 && model->result;
        if (model->result)
            result_stack->setCurrentWidget(views->currentIndex() == 2 ? coverage_page
                                           : chart                    ? plot_page
                                                                      : table_page);
        export_csv->setText(chart ? ui::text("report_dialog.export_csv.set_text")
                                  : ui::text("report_dialog.label_101"));
        copy_button->setText(chart ? ui::text("report_dialog.copy_button.set_text")
                                   : ui::text("report_dialog.result_menu.add_action"));
        copy_button->setToolTip(chart ? ui::text("report_dialog.copy_button.set_tool_tip")
                                      : ui::text("report_dialog.copy_button.set_tool_tip_2"));
        copy_button->setEnabled(chart || table->selectionModel()->hasSelection());
    }
    std::vector<ReportCurveField> selected_curves() const {
        std::vector<ReportCurveField> selected;
        for (const auto &field : curve_choices)
            if (curve_ids.contains(field.id))
                selected.push_back(field);
        return selected;
    }
    void update_curve_button() {
        QStringList names;
        for (const auto &field : selected_curves())
            names << field.label;
        plot_field->setText(names.size() == 1
                                ? ui::text("report_dialog.plot_field.set_text").arg(names.front())
                                : ui::text("report_dialog.ield.set_text").arg(names.size()));
        plot_field->setAccessibleDescription(names.join(QStringLiteral("、")));
        plot_field->setToolTip(
            ui::text("report_dialog.plot_field.set_tool_tip_2").arg(names.join("\n")));
        plot_field->setEnabled(!curve_choices.empty());
    }
    void apply_curves(const QStringList &selected) {
        if (!model->result)
            return;
        curve_ids = selected;
        curve_preferences[model->result->definition.query.kind] = curve_ids;
        plot->set_fields(selected_curves());
        update_curve_button();
    }
    bool choose_curves(QStringList selected = {}) {
        if (curve_choices.empty())
            return false;
        const auto snapshot = model->result;
        ReportCurveSelectionDialog chooser(curve_choices, selected.isEmpty() ? curve_ids : selected,
                                           owner);
        if (chooser.exec() != QDialog::Accepted)
            return false;
        if (model->result != snapshot) {
            status->setText(ui::text("report_dialog.status.set_text_15"));
            return false;
        }
        apply_curves(chooser.selection());
        return true;
    }
    void prepare_plot(bool preserve_view) {
        const auto plot_result = complete_plot_result(*model->result, retained_column_specs);
        const auto &r = *plot_result;
        const auto previous_object = plot_object->id();
        const QSignalBlocker objects_guard(plot_object);
        curve_choices.clear();
        plot_object->set_objects({});
        plot_object->set_id({});
        const auto &columns = r.table.columns();
        const auto id_column = std::ranges::find(columns, "object.id", &results::Column::id);
        const auto name_column = std::ranges::find(columns, "object.name", &results::Column::id);
        bool sampled =
            report_is_sampled(r.definition.query.kind) &&
            !report_is_interval(r.definition.query.kind) &&
            r.definition.query.kind != ReportKind::orbital_events && id_column != columns.end() &&
            name_column != columns.end() &&
            std::ranges::find(columns, "time.epoch", &results::Column::id) != columns.end() &&
            !r.table.rows().empty();
        QString unavailable;
        if (sampled) {
            // A column-only table can be valid without UTC coverage. Keep it usable
            // and require an explicit time-scale choice before drawing UTC labels.
            const auto time_column = static_cast<std::size_t>(
                std::ranges::find(columns, "time.epoch", &results::Column::id) - columns.begin());
            try {
                (void)r.cell_text(0, time_column);
                (void)r.cell_text(r.table.rows().size() - 1, time_column);
            } catch (const std::exception &error) {
                sampled = false;
                unavailable =
                    ui::text("report_dialog.label_187").arg(QString::fromUtf8(error.what()));
            }
        }
        if (sampled) {
            for (std::size_t c = 0; c < r.table.columns().size(); ++c) {
                bool numeric = false;
                for (std::size_t row = 0; row < r.table.rows().size() && !numeric; ++row)
                    numeric = r.numeric_value(row, c).has_value();
                if (!numeric)
                    continue;
                const auto &column = r.table.columns()[c];
                const auto suffix = r.column_unit(c);
                const auto spec = std::ranges::find(r.definition.format.columns, column.id,
                                                    &ReportColumnSpec::field);
                const auto title = spec != r.definition.format.columns.end() && !spec->title.empty()
                                       ? text(spec->title)
                                       : localized(text(column.title));
                curve_choices.push_back(
                    {c, text(column.id),
                     title + (suffix.empty() ? QString{} : " (" + text(suffix) + ")"),
                     text(suffix)});
            }
            // A compact legend remains legible for up to twelve objects. Larger scopes keep
            // their complete table/CSV; the chart explicitly asks the user to choose an object.
            std::map<std::string, std::string> objects;
            for (const auto &row : r.table.rows())
                objects.try_emplace(std::get<std::string>(row[id_column - columns.begin()]),
                                    std::get<std::string>(row[name_column - columns.begin()]));
            std::map<std::string, ObjectKind> kinds;
            for (const auto &choice : project_object_choices(project))
                kinds.emplace(choice.id, choice.kind);
            std::vector<ObjectChoice> choices;
            // Result names and IDs belong to the immutable table, even after project edits.
            for (const auto &[id, object_name] : objects)
                choices.push_back(
                    {id, text(object_name), kinds.contains(id) ? kinds.at(id) : ObjectKind::other});
            plot_object->set_options({ui::text("report_dialog.label_188"), false,
                                      objects.size() <= 12
                                          ? ui::text("report_dialog.label_189").arg(objects.size())
                                          : QString{}});
            plot_object->set_objects(std::move(choices));
            if (plot_object->contains(previous_object))
                plot_object->set_id(previous_object);
            else if (objects.size() <= 12)
                plot_object->set_selection({{}, true});
            else
                plot_object->set_id(objects.begin()->first);
            plot_note->setText(objects.size() > 12 ? ui::text("report_dialog.plot_note.set_text")
                                                   : ui::text("report_dialog.label_108"));
        }
        curve_ids = curve_preferences[r.definition.query.kind];
        curve_ids.removeIf([this](const auto &id) {
            return std::ranges::find(curve_choices, id, &ReportCurveField::id) ==
                   curve_choices.end();
        });
        if (curve_ids.isEmpty() && !curve_choices.empty()) {
            const auto preferred =
                r.definition.query.kind == ReportKind::classical_elements  ? "elements.nu"
                : r.definition.query.kind == ReportKind::geodetic_position ? "geodetic.height"
                : r.definition.query.kind == ReportKind::aer               ? "aer.elevation"
                : r.definition.query.kind == ReportKind::relative_motion   ? "relative.range"
                : r.definition.query.kind == ReportKind::coverage_time     ? "coverage.fraction"
                                                                           : "";
            const auto field = std::ranges::find(curve_choices, QString::fromLatin1(preferred),
                                                 &ReportCurveField::id);
            curve_ids << (field == curve_choices.end() ? curve_choices.front().id : field->id);
        }
        curve_preferences[r.definition.query.kind] = curve_ids;
        plot->set_result(plot_result, selected_curves(), plot_object->id(), preserve_view);
        update_curve_button();
        views->setTabEnabled(1, sampled && !curve_choices.empty());
        views->setTabToolTip(1, unavailable);
        choose_curve_action->setVisible(views->isTabEnabled(1));
        choose_curve_action->setEnabled(views->isTabEnabled(1));
        views->setTabVisible(1, views->isTabEnabled(1) || !unavailable.isEmpty());
        if (!views->isTabEnabled(views->currentIndex()))
            views->setCurrentIndex(0);
        select_view();
    }
    void export_file() {
        if (!model->result)
            return;
        if (views->currentIndex() > 0) {
            QFileDialog chooser(owner, ui::text("report_dialog.label_190"));
            chooser.setAcceptMode(QFileDialog::AcceptSave);
            chooser.setFileMode(QFileDialog::AnyFile);
            chooser.setNameFilter("PNG (*.png)");
            chooser.setDefaultSuffix("png");
            chooser.selectFile("report.png");
            if (chooser.exec() != QDialog::Accepted)
                return;
            QSaveFile output(chooser.selectedFiles().value(0));
            const bool ok = output.open(QIODevice::WriteOnly) &&
                            (views->currentIndex() == 2 ? coverage_map->image() : plot->image())
                                .save(&output, "PNG") &&
                            output.commit();
            status->setText(
                ok ? ui::text("report_dialog.status.set_text_16")
                   : ui::text("report_dialog.status.set_text_17").arg(output.errorString()));
            return;
        }
        QFileDialog chooser(owner, ui::text("report_dialog.label_191"));
        chooser.setAcceptMode(QFileDialog::AcceptSave);
        chooser.setFileMode(QFileDialog::AnyFile);
        chooser.setNameFilter("CSV (*.csv)");
        chooser.setDefaultSuffix("csv");
        chooser.selectFile("report.csv");
        if (chooser.exec() != QDialog::Accepted)
            return;
        const auto path = std::filesystem::path(chooser.selectedFiles().value(0).toStdWString());
        cancellation = std::make_shared<std::atomic_bool>(false);
        set_busy(true);
        progress->setRange(0, 0);
        status->setText(ui::text("report_dialog.status.set_text_18"));
        try {
            export_job = std::async(
                std::launch::async, [result = model->result, path, cancel = cancellation] {
                    runtime::export_report_csv(*result, path, true, cancel.get());
                });
        } catch (const std::exception &error) {
            set_busy(false);
            status->setText(
                ui::text("report_dialog.status.set_text_19").arg(QString::fromUtf8(error.what())));
        }
    }
    void copy_rows() {
        if (!model->result)
            return;
        auto rows = table->selectionModel()->selectedRows();
        if (rows.isEmpty()) {
            status->setText(ui::text("report_dialog.status.set_text_20"));
            return;
        }
        std::sort(rows.begin(), rows.end(),
                  [](const auto &a, const auto &b) { return a.row() < b.row(); });
        QString value;
        const auto append = [&](QStringList fields) {
            for (auto &field : fields) {
                if (field.contains('\t') || field.contains('\n') || field.contains('\r') ||
                    field.contains('"')) {
                    field.replace('"', "\"\"");
                    field = '"' + field + '"';
                }
            }
            value += fields.join('\t') + "\r\n";
        };
        QStringList headers;
        for (int c = 0; c < model->columnCount(); ++c)
            if (!table->isColumnHidden(c))
                headers << model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        append(headers);
        for (const auto &row : rows) {
            QStringList cells;
            for (int c = 0; c < model->columnCount(); ++c)
                if (!table->isColumnHidden(c))
                    cells << text(model->result->cell_text(table_proxy->mapToSource(row).row(), c));
            append(cells);
        }
        QApplication::clipboard()->setText(value);
        status->setText(ui::text("report_dialog.status.set_text_21").arg(rows.size()));
    }
    void poll() {
        if (job.valid()) {
            progress->setValue(static_cast<int>(completed->load()));
            if (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            try {
                auto generated = job.get();
                if (cancellation->load())
                    throw runtime::AnalysisCancelled{};
                if (job_generation == input_generation) {
                    result_analysis = std::move(generated.analysis);
                    publish(std::make_shared<runtime::ReportResult>(std::move(generated.report)));
                    narrow_settings = false;
                    adapt(owner->width());
                    status->setText(ui::text("report_dialog.status.set_text_22"));
                }
            } catch (const runtime::AnalysisCancelled &) {
                status->setText(ui::text("report_dialog.status.set_text_23"));
            } catch (const runtime::ReportCancelled &) {
                status->setText(ui::text("report_dialog.status.set_text_23"));
            } catch (const std::exception &error) {
                status->setText(ui::text("report_dialog.status.set_text_24")
                                    .arg(QString::fromUtf8(error.what())));
            }
            if (job_generation != input_generation)
                status->setText(ui::text("report_dialog.status.set_text_25"));
            set_busy(false);
        }
        if (export_job.valid() &&
            export_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                export_job.get();
                status->setText(ui::text("report_dialog.status.set_text_26"));
            } catch (const runtime::ReportCancelled &) {
                status->setText(ui::text("report_dialog.status.set_text_27"));
            } catch (const std::exception &error) {
                status->setText(ui::text("report_dialog.status.set_text_28")
                                    .arg(QString::fromUtf8(error.what())));
            }
            set_busy(false);
        }
    }
    void update(application::Project next, std::shared_ptr<const core::ITimeProvider> clock) {
        try {
            draft = definition();
        } catch (const std::exception &) {
        }
        if (runtime::report_input_sha256(project) != runtime::report_input_sha256(next) ||
            !same_time(time, clock)) {
            ++input_generation;
            if (job.valid() && cancellation)
                cancellation->store(true);
        }
        project = std::move(next);
        time = std::move(clock);
        first->set_time_provider(time);
        last->set_time_provider(time);
        if (follow->isChecked()) {
            first->set_epoch(project.first);
            last->set_epoch(project.last);
        }
        fill_library();
        refresh_scope();
        if (pair_report(draft.query.kind))
            fill_access_pair(true);
        update_interval();
        update_estimate();
        result_status();
    }
};
ReportDialog::ReportDialog(application::Project project,
                           std::shared_ptr<const core::ITimeProvider> time, Prepare prepare,
                           Save save, std::function<void(core::Epoch)> seek, QWidget *parent)
    : QDialog(parent),
      impl_(std::make_unique<Impl>(this, std::move(project), std::move(time), std::move(prepare),
                                   std::move(save), std::move(seek))) {}
ReportDialog::~ReportDialog() = default;
void ReportDialog::select_object(const std::string &id, application::ReportKind kind) {
    auto &self = *impl_;
    if (!self.generate->isEnabled()) {
        self.status->setText(ui::text("report_dialog.status.set_text_29"));
        return;
    }
    self.scope_ids = {id};
    self.all_objects = false;
    self.refresh_scope();
    self.choose(static_cast<int>(self.compatible(kind) ? kind : ReportKind::object_summary));
    self.narrow_settings = true;
    self.wide_settings = true;
    self.adapt(width());
}
void ReportDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (impl_)
        impl_->adapt(event->size().width());
}
void ReportDialog::update_project(application::Project project,
                                  std::shared_ptr<const core::ITimeProvider> time) {
    impl_->update(std::move(project), std::move(time));
}
} // namespace orbitforge::desktop
