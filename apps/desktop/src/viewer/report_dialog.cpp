#include "report_dialog.hpp"
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
        painter.drawText(QRect(width() - 28, 0, 20, height()), Qt::AlignCenter,
                         QStringLiteral("…"));
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
        return QStringLiteral("场景摘要");
    case ReportKind::object_summary:
        return QStringLiteral("对象摘要");
    case ReportKind::cartesian_state:
        return QStringLiteral("位置与速度");
    case ReportKind::ground_stations:
        return QStringLiteral("地面站坐标");
    case ReportKind::classical_elements:
        return QStringLiteral("经典轨道根数");
    case ReportKind::geodetic_position:
        return QStringLiteral("经纬高");
    case ReportKind::orbital_motion:
        return QStringLiteral("轨道距离与速率");
    case ReportKind::access_intervals:
        return QStringLiteral("可见时段 · Access");
    case ReportKind::aer:
        return QStringLiteral("AER · 方位 / 仰角 / 距离");
    case ReportKind::lighting_intervals:
        return QStringLiteral("光照与地影时段");
    case ReportKind::lighting_state:
        return QStringLiteral("太阳光照比例");
    case ReportKind::orbital_events:
        return QStringLiteral("轨道事件 · 近远地点 / 交点");
    case ReportKind::relative_motion:
        return QStringLiteral("星间相对运动 · RIC");
    case ReportKind::intersatellite_access:
        return QStringLiteral("星间可见时段");
    case ReportKind::coverage_grid:
        return QStringLiteral("覆盖网格与重访");
    case ReportKind::coverage_time:
        return QStringLiteral("覆盖率时间曲线");
    case ReportKind::state_covariance:
        return QStringLiteral("HPOP · 位置速度协方差");
    }
    return {};
}
QString localized(QString value) {
    static const std::map<QString, QString> labels{
        {"Latitude", QStringLiteral("大地纬度")},
        {"Cell area", QStringLiteral("网格面积")},
        {"Time covered", QStringLiteral("覆盖时长")},
        {"Time covered fraction", QStringLiteral("覆盖时长占比")},
        {"Coverage intervals", QStringLiteral("可见区间数")},
        {"Maximum revisit time", QStringLiteral("最长无覆盖空档")},
        {"Mean revisit time", QStringLiteral("平均无覆盖空档")},
        {"Area covered", QStringLiteral("覆盖面积")},
        {"Area covered fraction", QStringLiteral("覆盖面积占比")},
        {"Cumulative area covered", QStringLiteral("累计覆盖面积")},
        {"Cumulative area covered fraction", QStringLiteral("累计覆盖面积占比")},
        {"Property", QStringLiteral("属性")},
        {"Value", QStringLiteral("值")},
        {"Object ID", QStringLiteral("对象 ID")},
        {"Object", QStringLiteral("对象")},
        {"Type", QStringLiteral("类型")},
        {"Model", QStringLiteral("模型")},
        {"Time", QStringLiteral("时间")},
        {"Frame", QStringLiteral("坐标系")},
        {"Longitude", QStringLiteral("经度")},
        {"Geodetic latitude", QStringLiteral("大地纬度")},
        {"Ellipsoid height", QStringLiteral("椭球高")},
        {"Semi-major axis", QStringLiteral("半长轴")},
        {"Eccentricity", QStringLiteral("偏心率")},
        {"Inclination", QStringLiteral("轨道倾角")},
        {"RAAN", QStringLiteral("升交点赤经")},
        {"Argument of periapsis", QStringLiteral("近地点幅角")},
        {"True anomaly", QStringLiteral("真近点角")},
        {"Geocentric distance", QStringLiteral("地心距离")},
        {"Speed", QStringLiteral("速率")},
        {"Radial speed", QStringLiteral("径向速率")},
        {"Transverse speed", QStringLiteral("横向速率")},
        {"Flight path angle", QStringLiteral("航迹倾角")},
        {"Scenario", QStringLiteral("场景")},
        {"Start", QStringLiteral("开始")},
        {"Stop", QStringLiteral("结束")},
        {"Time scale", QStringLiteral("时间系统")},
        {"Satellites", QStringLiteral("卫星数量")},
        {"Ground stations", QStringLiteral("地面站数量")},
        {"Central body", QStringLiteral("中心天体")},
        {"Earth", QStringLiteral("地球")},
        {"Satellite", QStringLiteral("卫星")},
        {"Ground station", QStringLiteral("地面站")},
        {"Duration", QStringLiteral("持续时间")},
        {"Event", QStringLiteral("事件")},
        {"Periapsis", QStringLiteral("近地点")},
        {"Apoapsis", QStringLiteral("远地点")},
        {"Ascending node", QStringLiteral("升交点")},
        {"Descending node", QStringLiteral("降交点")},
        {"Lighting", QStringLiteral("光照状态")},
        {"Solar disk illumination", QStringLiteral("太阳光照比例")},
        {"Sunlight", QStringLiteral("全光照")},
        {"Penumbra", QStringLiteral("半影")},
        {"Umbra", QStringLiteral("本影")},
        {"Start clipped", QStringLiteral("开始于分析边界")},
        {"Stop clipped", QStringLiteral("结束于分析边界")},
        {"Azimuth", QStringLiteral("方位角")},
        {"Elevation", QStringLiteral("仰角")},
        {"Range", QStringLiteral("斜距")},
        {"Maximum elevation", QStringLiteral("最大仰角")},
        {"Maximum elevation time", QStringLiteral("最大仰角时刻")},
        {"Minimum range", QStringLiteral("最近距离")},
        {"Minimum range time", QStringLiteral("最近距离时刻")},
        {"Inertial relative speed", QStringLiteral("惯性相对速率")},
        {"RIC radial", QStringLiteral("RIC 径向")},
        {"RIC in-track", QStringLiteral("RIC 沿轨")},
        {"RIC cross-track", QStringLiteral("RIC 法向")},
        {"Range rate", QStringLiteral("距离变化率")},
        {"Visible", QStringLiteral("可见（1 / 0）")},
        {"Off boresight", QStringLiteral("离轴角")},
        {"Two body", QStringLiteral("二体")},
        {"J2 secular", QStringLiteral("J2 长期项")},
        {"WGS84 fixed", QStringLiteral("WGS84 固定站")}};
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
            return value + QStringLiteral("\n此边界由分析区间截断，不是实际状态切换的时刻。");
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
        owner->setWindowTitle(QStringLiteral("报告"));
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
        view_button = new QPushButton(QStringLiteral("收起设置"), heading);
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
        section(QStringLiteral("对象范围"));
        auto *scope_row = new QHBoxLayout;
        scope_label = new QLabel(settings);
        scope_label->setObjectName("reportScopeLabel");
        scope_label->setWordWrap(true);
        scope_label->setTextFormat(Qt::PlainText);
        scope_row->addWidget(scope_label, 1);
        scope_button = new QPushButton(QStringLiteral("选择对象…"), settings);
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
        access_station->set_options({QStringLiteral("选择可见性分析的地面站"), false, {}});
        access_station->setAccessibleName(QStringLiteral("可见性分析地面站"));
        access_station->setObjectName("accessStation");
        access_target = new ObjectSelectionField(access_pair);
        access_target->set_options({QStringLiteral("选择可见性分析的卫星或传感器"), false, {}});
        access_target->setAccessibleName(QStringLiteral("可见性分析目标"));
        access_target->setObjectName("accessTarget");
        pair_form->addRow(QStringLiteral("地面站"), access_station);
        pair_form->addRow(QStringLiteral("卫星 / 传感器"), access_target);
        config->addWidget(access_pair);
        auto *template_heading = new QHBoxLayout;
        auto *template_title = new QLabel(QStringLiteral("报告类型"), settings);
        template_title->setObjectName("reportSectionTitle");
        template_heading->addWidget(template_title);
        template_heading->addStretch();
        config_menu = new QToolButton(settings);
        config_menu->setObjectName("reportConfigMenu");
        config_menu->setText(QStringLiteral("模板"));
        config_menu->setToolTip(QStringLiteral("将当前对象、类型和采样设置保存为项目模板"));
        config_menu->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(config_menu);
        store = menu->addAction(QStringLiteral("保存为模板…"));
        store->setObjectName("saveReport");
        update_template = menu->addAction(QStringLiteral("更新当前模板"));
        update_template->setObjectName("updateReport");
        menu->addSeparator();
        remove = menu->addAction(QStringLiteral("删除当前模板"));
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
        library->setAccessibleName(QStringLiteral("选择报告"));
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
            QStringLiteral("南界 / 北界（北纬为正）"), QString{},
            QStringLiteral("西界 / 东界（东经为正）"), QString{},
            QStringLiteral("网格间隔（赤道处）"),      QStringLiteral("最低仰角")};
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
        coverage_fields[2]->setToolTip(
            QStringLiteral("西界大于东界时，区域跨越日期变更线。全球使用 −180° 至 180°。"));
        auto *global = new QPushButton(QStringLiteral("使用全球范围"), coverage_settings);
        global->setObjectName("coverageGlobal");
        coverage_form->addRow(global);
        coverage_ends = new QCheckBox(QStringLiteral("重访统计包含首尾空档"), coverage_settings);
        coverage_ends->setObjectName("coverageEndGaps");
        coverage_ends->setToolTip(QStringLiteral(
            "首尾空档截断到分析区间；从未覆盖的网格点，其空档始终等于整个分析时长。"));
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
        auto *sampling_title = new QLabel(QStringLiteral("时间与采样"), sampling);
        sampling_title->setObjectName("reportSectionTitle");
        sampling_layout->addWidget(sampling_title);
        follow = new QCheckBox(QStringLiteral("使用场景时段"), sampling);
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
        time_layout->addWidget(note(QStringLiteral("开始"), custom_times));
        time_layout->addWidget(first);
        time_layout->addWidget(note(QStringLiteral("结束"), custom_times));
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
        step->setSuffix(QStringLiteral(" 秒"));
        step->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        step->setToolTip(QStringLiteral("报告独立采样，始终包含起止时刻。"));
        step_label = new QLabel(QStringLiteral("采样间隔"));
        sampling_form->addRow(step_label, step);
        access_options = new QWidget(sampling);
        auto *access_form = new QFormLayout(access_options);
        access_form->setContentsMargins(0, 0, 0, 0);
        access_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        access_form->setVerticalSpacing(10);
        access_mode = new QComboBox(access_options);
        access_mode->setObjectName("accessSearchMode");
        access_mode->addItems({QStringLiteral("固定步长"), QStringLiteral("极值辅助搜索")});
        access_mode->setToolTip(QStringLiteral("极值辅助搜索在检查区间内寻找峰谷，以发现短窗口和短"
                                               "间隙。快速变化时仍需减小最大检查间隔。"));
        access_tolerance = new StepSpinBox(access_options);
        access_tolerance->setObjectName("accessTimeTolerance");
        access_tolerance->setDecimals(6);
        access_tolerance->setRange(.000001, 1000);
        access_tolerance->setValue(.001);
        access_tolerance->setSuffix(QStringLiteral(" 秒"));
        access_tolerance->setToolTip(
            QStringLiteral("已发现事件、边界与过境极值的时间搜索容差；不是轨道的物理精度。"));
        access_form->addRow(QStringLiteral("搜索方式"), access_mode);
        access_form->addRow(QStringLiteral("时间容差"), access_tolerance);
        sampling_form->addRow(access_options);
        frame = new QComboBox(sampling);
        frame->setObjectName("reportFrame");
        frame->addItems({"GCRF", "ITRF"});
        frame_field = new QWidget(sampling);
        auto *frame_form = new QFormLayout(frame_field);
        frame_form->setContentsMargins(0, 0, 0, 0);
        frame_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        frame_form->addRow(QStringLiteral("坐标系"), frame);
        sampling_layout->addLayout(sampling_form);
        sampling_layout->addWidget(frame_field);
        config->addWidget(sampling);
        section(QStringLiteral("显示格式"));
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
        units_form->addRow(QStringLiteral("数值单位"), units);
        config->addWidget(units_field);
        scale_field = new QWidget(settings);
        auto *scale_form = new QFormLayout(scale_field);
        scale_form->setContentsMargins(0, 0, 0, 0);
        scale_form->setHorizontalSpacing(12);
        scale_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        scale = new QComboBox(scale_field);
        scale->setObjectName("reportTimeScale");
        scale->addItems({"UTC", "TAI", "TT"});
        scale_form->addRow(QStringLiteral("时间系统"), scale);
        for (auto *form : {sampling_form, access_form, frame_form, units_form, scale_form}) {
            form->setHorizontalSpacing(12);
            if (auto *label = form->itemAt(0, QFormLayout::LabelRole)->widget())
                label->setFixedWidth(
                    settings->fontMetrics().horizontalAdvance(QStringLiteral("最大检查间隔")));
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
        generate = new QPushButton(QStringLiteral("生成报告"), actions);
        generate->setObjectName("generateReport");
        generate->setProperty("primary", true);
        generate->setDefault(true);
        auto *generation_buttons = new QHBoxLayout;
        generation_buttons->addStretch();
        generate->setMinimumWidth(104);
        cancel = new QPushButton(QStringLiteral("取消生成"), actions);
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
        result_title = new ReportTitle(QStringLiteral("报告"), heading);
        result_title->setObjectName("reportResultTitle");
        result_title->setTextFormat(Qt::PlainText);
        result_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        heading_layout->addWidget(result_title, 1);
        refresh_button = new QToolButton(heading);
        refresh_button->setObjectName("refreshReport");
        refresh_button->setText(QStringLiteral("↻"));
        refresh_button->setAccessibleName(QStringLiteral("刷新报告"));
        refresh_button->setToolTip(QStringLiteral("按当前设置重新生成（F5）"));
        refresh_button->setShortcut(QKeySequence(Qt::Key_F5));
        refresh_button->setEnabled(false);
        heading_layout->addWidget(refresh_button);
        export_csv = new QPushButton(QStringLiteral("导出 CSV…"), heading);
        export_csv->setObjectName("exportReportCsv");
        export_csv->setEnabled(false);
        heading_layout->addWidget(export_csv);
        result_more = new QToolButton(heading);
        result_more->setObjectName("reportResultMenu");
        result_more->setText(QStringLiteral("···"));
        result_more->setToolTip(QStringLiteral("复制与数据来源"));
        result_more->setPopupMode(QToolButton::InstantPopup);
        result_more->setEnabled(false);
        auto *result_menu = new QMenu(result_more);
        copy_button = result_menu->addAction(QStringLiteral("复制选中行"));
        copy_button->setObjectName("reportCopy");
        copy_button->setEnabled(false);
        source_button = result_menu->addAction(QStringLiteral("查看数据来源…"));
        source_button->setObjectName("reportShowSource");
        source_button->setEnabled(false);
        result_more->setMenu(result_menu);
        heading_layout->addWidget(result_more);
        result_label = note(QStringLiteral("等待生成"), result_panel);
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
        views->addTab(QStringLiteral("表格"));
        views->addTab(QStringLiteral("曲线"));
        views->addTab(QStringLiteral("热力图"));
        views->setTabEnabled(2, false);
        views->setTabEnabled(1, false);
        views->setTabVisible(2, false);
        views->setTabVisible(1, false);
        views->setTabToolTip(1, QStringLiteral("生成按时间采样的报告后，可查看曲线。"));
        result_meta->addWidget(views);
        result_meta->addWidget(result_label, 1);
        result_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        result_layout->addWidget(result_meta_bar);
        pass_summary = new QWidget(result_panel);
        pass_summary->setObjectName("accessPassSummary");
        auto *metrics = new QGridLayout(pass_summary);
        metrics->setContentsMargins(8, 4, 8, 4);
        metrics->setSpacing(0);
        const std::array metric_names{QStringLiteral("可见过境"), QStringLiteral("总可见时长"),
                                      QStringLiteral("最长过境"), QStringLiteral("平均时长")};
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
        auto *empty_title = new QLabel(QStringLiteral("还没有报告结果"), empty);
        empty_title->setObjectName("reportEmptyTitle");
        empty_title->setAlignment(Qt::AlignCenter);
        empty_layout->addWidget(empty_title);
        auto *empty_note = note(QStringLiteral("选择对象和报告类型，点击“生成报告”。"), empty);
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
        precision->setAccessibleName(QStringLiteral("表格显示精度"));
        precision->addItem(QStringLiteral("常规精度"), 9);
        precision->addItem(QStringLiteral("6 位有效数字"), 6);
        precision->addItem(QStringLiteral("12 位有效数字"), 12);
        precision->addItem(QStringLiteral("完整精度"), 17);
        precision->setToolTip(QStringLiteral(
            "只调整显示。常规：9 位有效数字，事件值 3 位小数。复制与导出始终保留完整精度。"));
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
        table->horizontalHeader()->setToolTip(
            QStringLiteral("点击列标题排序，再次点击反向；第三次恢复原始顺序。"));
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
        auto *copy = new QAction(QStringLiteral("复制所选行"), table);
        copy->setObjectName("copyReportRows");
        copy->setShortcut(QKeySequence::Copy);
        copy->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        table->addAction(copy);
        table->setContextMenuPolicy(Qt::ActionsContextMenu);
        QObject::connect(copy, &QAction::triggered, owner, [this] { copy_rows(); });
        auto *copy_direct = table_tools->addAction(QStringLiteral("复制"));
        copy_direct->setObjectName("reportTableCopy");
        copy_direct->setToolTip(QStringLiteral("复制所选行，含列标题（Ctrl+C）"));
        copy_direct->setEnabled(false);
        QObject::connect(copy_direct, &QAction::triggered, owner, [this] { copy_rows(); });
        choose_columns_action = table_tools->addAction(QStringLiteral("显示列…"));
        choose_columns_action->setObjectName("reportChooseColumns");
        choose_columns_action->setToolTip(QStringLiteral("选择并重排表格和 CSV 的列，不重新计算"));
        choose_columns_action->setEnabled(false);
        table->addAction(choose_columns_action);
        QObject::connect(choose_columns_action, &QAction::triggered, owner,
                         [this] { choose_columns(); });
        choose_curve_action = table_tools->addAction(QStringLiteral("曲线…"));
        choose_curve_action->setObjectName("reportChooseCurves");
        choose_curve_action->setToolTip(QStringLiteral("选择多个字段并在曲线中比较"));
        choose_curve_action->setEnabled(false);
        QObject::connect(choose_curve_action, &QAction::triggered, owner, [this] {
            if (choose_curves())
                views->setCurrentIndex(1);
        });
        auto *fit_columns = table_tools->addAction(QStringLiteral("适合列宽"));
        fit_columns->setObjectName("reportFitColumns");
        fit_columns->setToolTip(QStringLiteral("按当前显示精度调整列宽；也可拖动表头边界调整"));
        QObject::connect(fit_columns, &QAction::triggered, owner, [this] { fit_table_columns(); });
        auto *original_order = table_tools->addAction(QStringLiteral("原始顺序"));
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
                status->setText(QStringLiteral("图片已复制。"));
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
        plot_field->setAccessibleName(QStringLiteral("选择曲线"));
        plot_object = new ObjectSelectionField(plot_page);
        plot_object->setObjectName("reportPlotObject");
        plot_object->setAccessibleName(QStringLiteral("曲线对象"));
        plot_field->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        plot_field->setToolTip(QStringLiteral("选择要同时显示的曲线；同单位叠加，不同单位分区"));
        plot_object->setToolTip(QStringLiteral("选择比较全部对象或只看单个对象"));
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
        reset_plot->setToolTip(QStringLiteral("还原完整范围（Home）"));
        reset_plot->setAccessibleName(QStringLiteral("还原曲线范围"));
        reset_plot->setText(QStringLiteral("全范围"));
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
        auto *box = navigation->addAction(QStringLiteral("框选"));
        box->setObjectName("reportPlotBox");
        box->setCheckable(true);
        box->setChecked(true);
        box->setToolTip(QStringLiteral("拖动框选时间范围；Esc 取消操作"));
        mode_group->addAction(box);
        auto *pan = navigation->addAction(QStringLiteral("平移"));
        pan->setObjectName("reportPlotPan");
        pan->setCheckable(true);
        pan->setToolTip(QStringLiteral("拖动平移时间范围；也可按住鼠标中键拖动"));
        mode_group->addAction(pan);
        QObject::connect(pan, &QAction::toggled, plot, &ReportPlot::set_pan_mode);
        navigation->addSeparator();
        auto *zoom_in =
            navigation->addAction(workbench_icon(WorkbenchIcon::zoom_in), QStringLiteral("放大"));
        zoom_in->setObjectName("reportPlotZoomIn");
        zoom_in->setToolTip(QStringLiteral("放大时间范围（+）；滚轮以指针为中心缩放"));
        auto *zoom_out =
            navigation->addAction(workbench_icon(WorkbenchIcon::zoom_out), QStringLiteral("缩小"));
        zoom_out->setObjectName("reportPlotZoomOut");
        zoom_out->setToolTip(QStringLiteral("缩小时间范围（−）"));
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
        plot_note = note(QStringLiteral("滚轮缩放 · 中键平移 · 双击采样点定位时刻"), plot_page);
        plot_layout->addWidget(plot_note);
        result_stack->addWidget(plot_page);
        coverage_page = new QWidget(result_stack);
        auto *coverage_layout = new QVBoxLayout(coverage_page);
        coverage_layout->setContentsMargins(0, 0, 0, 0);
        coverage_metric = new QComboBox(coverage_page);
        coverage_metric->setObjectName("coverageMetric");
        coverage_metric->addItems(
            {QStringLiteral("覆盖时长占比"), QStringLiteral("最长无覆盖空档")});
        coverage_layout->addWidget(coverage_metric);
        coverage_map = new CoverageMap(coverage_page);
        coverage_layout->addWidget(coverage_map, 1);
        coverage_point = note(QStringLiteral("选择网格点查看可见区间"), coverage_page);
        coverage_point->setObjectName("coveragePointDetail");
        coverage_layout->addWidget(coverage_point);
        coverage_windows = new QComboBox(coverage_page);
        coverage_windows->setObjectName("coverageWindows");
        coverage_windows->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        coverage_windows->setAccessibleName(QStringLiteral("网格点可见区间"));
        coverage_layout->addWidget(coverage_windows);
        coverage_jump = new QPushButton(QStringLiteral("定位所选区间开始时刻"), coverage_page);
        coverage_jump->setObjectName("coverageJump");
        coverage_layout->addWidget(coverage_jump);
        coverage_layout->addWidget(note(
            QStringLiteral("WGS84 网格中心采样 · 按网格面积加权 · 不含地形和折射"), coverage_page));
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
        status = note(QStringLiteral("就绪"), owner);
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
                                        ? QStringLiteral("搜索步长")
                                        : QStringLiteral("最大检查间隔"));
            if (draft.query.kind == ReportKind::relative_motion)
                step_label->setText(QStringLiteral("采样 / 检查间隔"));
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
                status->setText(QStringLiteral("已取消报告生成。"));
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
            menu.addAction(QStringLiteral("复制此单元格"), owner, [this, index] {
                QApplication::clipboard()->setText(
                    text(model->result->cell_text(index.row(), index.column())));
            });
            const auto curve_field = std::ranges::find(
                curve_choices, text(r.table.columns()[index.column()].id), &ReportCurveField::id);
            if (views->isTabEnabled(1) && curve_field != curve_choices.end()) {
                const auto id = curve_field->id;
                menu.addAction(QStringLiteral("加入曲线比较"), owner, [this, id] {
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
                menu.addAction(QStringLiteral("只显示此列曲线"), owner, [this, id] {
                    apply_curves({id});
                    views->setCurrentIndex(1);
                });
            }
            if (views->isTabEnabled(1))
                menu.addAction(QStringLiteral("选择曲线…"), owner, [this] {
                    if (choose_curves())
                        views->setCurrentIndex(1);
                });
            if (!application::report_is_interval(r.definition.query.kind)) {
                menu.exec(table->viewport()->mapToGlobal(point));
                return;
            }
            menu.addSeparator();
            for (const auto &[id, label] : std::array{
                     std::pair{"time.epoch", QStringLiteral("定位到时段开始")},
                     std::pair{"access.max_elevation_time", QStringLiteral("定位到最大仰角")},
                     std::pair{"access.min_range_time", QStringLiteral("定位到最近距离")},
                     std::pair{"access.stop", QStringLiteral("定位到过境结束")},
                     std::pair{"lighting.stop", QStringLiteral("定位到时段结束")}}) {
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
        library->setToolTip(library->text() + QStringLiteral("\n选择报告…"));
        library->setAccessibleDescription(library->toolTip());
        remove->setEnabled(saved);
        update_template->setEnabled(saved);
        store->setText(saved ? QStringLiteral("另存为模板…") : QStringLiteral("保存为模板…"));
        refresh_scope();
    }
    QString report_scope(const std::vector<std::string> &ids, bool all) const {
        if (all)
            return QStringLiteral("整个场景中的适用对象");
        QStringList names;
        const auto objects = project_object_choices(project);
        for (const auto &id : ids) {
            const auto found = std::ranges::find(objects, id, &ObjectChoice::id);
            names << (found == objects.end() ? text(id) + QStringLiteral("（已删除）")
                                             : found->path);
        }
        return names.join(QStringLiteral("、"));
    }
    std::vector<ReportChoice> report_choices() const {
        const auto description = [](ReportKind kind) -> QString {
            switch (kind) {
            case ReportKind::scenario_summary:
                return QStringLiteral("场景时段、对象数量与中心天体。");
            case ReportKind::object_summary:
                return QStringLiteral("对象名称、类型与计算模型。");
            case ReportKind::cartesian_state:
                return QStringLiteral("按指定间隔输出位置与速度，可选 GCRF 或 ITRF。");
            case ReportKind::ground_stations:
                return QStringLiteral("WGS84 经度、大地纬度、椭球高与 ITRF 坐标。");
            case ReportKind::classical_elements:
                return QStringLiteral("由 GCRF 状态换算瞬时六根数；J2 / J4 "
                                      "结果不是平均根数。");
            case ReportKind::geodetic_position:
                return QStringLiteral("ITRF / WGS84 "
                                      "经度、大地纬度和椭球高。高度不表示离地距离。");
            case ReportKind::orbital_motion:
                return QStringLiteral("GCRF 地心距离、总速率、径向与横向速率、航迹倾角。");
            case ReportKind::access_intervals:
                return QStringLiteral("地面站与卫星 / "
                                      "传感器的可见时段，考虑地球遮挡、最低仰角与传感器视场。");
            case ReportKind::aer:
                return QStringLiteral("地面站观测目标的方位角、仰角、斜距与可见标记。");
            case ReportKind::lighting_intervals:
                return QStringLiteral(
                    "全光照、半影与本影的连续时段、持续时间和占比。地球球面遮挡，使用几何太阳。");
            case ReportKind::lighting_state:
                return QStringLiteral("按时刻输出可见太阳圆盘的比例，支持光照曲线与 "
                                      "CSV。地球球面遮挡，使用几何太阳。");
            case ReportKind::orbital_events:
                return QStringLiteral("近地点、远地点、升交点和降交点的精确时刻、地心距离与速率。交"
                                      "点采用 GCRF 赤道面，近圆 / 近赤道情况明确提示。");
            case ReportKind::relative_motion:
                return QStringLiteral(
                    "基准卫星 RIC 相对位置、星间距离、距离变化率和惯性相对速率，附地球遮挡状态。");
            case ReportKind::intersatellite_access:
                return QStringLiteral(
                    "WGS84 椭球遮挡下的星间可见时段、持续时间和每段最近距离。双击定位时刻。");
            case ReportKind::coverage_grid:
                return QStringLiteral(
                    "区域或全球网格的覆盖时长、覆盖比例与重访空档；多颗卫星 / 传感器联合覆盖。");
            case ReportKind::coverage_time:
                return QStringLiteral(
                    "随时间变化的覆盖面积与累计覆盖比例；按 WGS84 网格面积加权。");
            case ReportKind::state_covariance:
                return QStringLiteral("传播已启用的 HPOP 协方差，输出 GCRF 位置速度矩阵的下三角。");
            }
            return {};
        };
        const auto output = [](ReportKind kind) -> QString {
            switch (kind) {
            case ReportKind::coverage_grid:
                return QStringLiteral("热力图 · 网格统计 · 可见区间");
            case ReportKind::coverage_time:
                return QStringLiteral("覆盖率曲线 · 面积统计 · 热力图");
            case ReportKind::scenario_summary:
                return QStringLiteral("属性表");
            case ReportKind::object_summary:
                return QStringLiteral("对象表");
            case ReportKind::ground_stations:
                return QStringLiteral("站点坐标表");
            case ReportKind::access_intervals:
            case ReportKind::intersatellite_access:
                return QStringLiteral("可见时间区间表");
            case ReportKind::lighting_intervals:
                return QStringLiteral("光照时间区间表 · 分类统计");
            case ReportKind::orbital_events:
                return QStringLiteral("事件时刻表 · 分类统计 · 时间定位");
            default:
                return QStringLiteral("时间序列 · 表格与曲线");
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
            choices.push_back(
                {QStringLiteral("builtin:%1").arg(static_cast<int>(kind)),
                 kind_name(kind),
                 kind_name(kind),
                 description(kind),
                 report_is_intersatellite(kind) ? QStringLiteral("基准卫星 → 目标卫星 · 有序对象对")
                 : report_is_access(kind)
                     ? QStringLiteral("地面站 → 卫星 / 传感器；保留当前对象，随后选择配对对象")
                     : report_scope(scope_ids, all_objects),
                 false,
                 compatible(kind) ? QString{} : QStringLiteral("不适用于当前对象范围"),
                 output(kind),
                 {},
                 icon(kind)});
        for (const auto &definition : project.reports) {
            QString warning;
            const auto objects = project_object_choices(project);
            for (const auto &id : definition.query.object_ids)
                if (std::ranges::find(objects, id, &ObjectChoice::id) == objects.end())
                    warning =
                        QStringLiteral("\n绑定的对象已删除；载入后需重新选择对象，才能生成报告。");
            choices.push_back(
                {"template:" + text(definition.id),
                 text(definition.name),
                 kind_name(definition.query.kind),
                 description(definition.query.kind),
                 report_scope(definition.query.object_ids,
                              definition.query.scope == ReportScope::all_compatible),
                 true,
                 {},
                 output(definition.query.kind),
                 QStringLiteral("载入模板将恢复已保存的对象、时间和格式设置。") + warning,
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
            QString label = text(id) + QStringLiteral("（已删除）");
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
        scope_label->setText(all_objects ? QStringLiteral("整个场景")
                             : labels.size() == 1
                                 ? labels.front()
                                 : QStringLiteral("已选 %1 个对象").arg(labels.size()));
        scope_label->setToolTip(labels.join('\n'));
        scope_detail->setText(
            all_objects ? QStringLiteral("包含当前和以后添加的适用对象")
            : labels.size() == 1
                ? (single_station ? QStringLiteral("仅此地面站") : QStringLiteral("仅此对象"))
                : QStringLiteral("仅使用指定对象，不自动添加新对象"));
        owner->setWindowTitle(scope_ids.size() == 1
                                  ? QStringLiteral("报告 · %1").arg(labels.front())
                                  : QStringLiteral("报告"));
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
        view_button->setText(
            compact ? (narrow_settings ? QStringLiteral("查看结果") : QStringLiteral("编辑设置"))
                    : (wide_settings ? QStringLiteral("收起设置") : QStringLiteral("展开设置")));
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
                                      {QStringLiteral("选择报告适用对象"), true,
                                       report_is_coverage(draft.query.kind)
                                           ? QString{}
                                           : QStringLiteral("全部适用对象（含以后新增）")},
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
        dialog.setWindowTitle(QStringLiteral("报告来源"));
        dialog.setObjectName("reportSourceDialog");
        dialog.resize(640, 400);
        auto *layout = new QVBoxLayout(&dialog);
        auto *source = new QPlainTextEdit(&dialog);
        source->setObjectName("reportSource");
        source->setReadOnly(true);
        source->setPlainText(QStringLiteral("报告：%1\n算法：%2\n\n输入 SHA-256\n%3\n\n场景数据 "
                                            "SHA-256\n%4\n\nUTC 数据 SHA-256\n%5")
                                 .arg(text(result.definition.name), text(result.algorithm),
                                      text(result.input_sha256), text(result.data_sha256),
                                      text(result.time_sha256)));
        layout->addWidget(source, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.exec();
    }
    void fill_access_pair(bool preserve_missing) {
        const QSignalBlocker station_guard(access_station), target_guard(access_target);
        const bool intersatellite = report_is_intersatellite(draft.query.kind);
        auto *form = static_cast<QFormLayout *>(access_pair->layout());
        static_cast<QLabel *>(form->labelForField(access_station))
            ->setText(intersatellite ? QStringLiteral("基准卫星") : QStringLiteral("地面站"));
        static_cast<QLabel *>(form->labelForField(access_target))
            ->setText(intersatellite ? QStringLiteral("目标卫星")
                                     : QStringLiteral("卫星 / 传感器"));
        access_station->set_options({intersatellite ? QStringLiteral("选择 RIC 基准卫星")
                                                    : QStringLiteral("选择可见性分析的地面站"),
                                     false,
                                     {}});
        access_target->set_options({intersatellite ? QStringLiteral("选择目标卫星")
                                                   : QStringLiteral("选择可见性分析的卫星或传感器"),
                                    false,
                                    {}});
        access_station->setAccessibleName(intersatellite ? QStringLiteral("基准卫星")
                                                         : QStringLiteral("可见性分析地面站"));
        access_target->setAccessibleName(intersatellite ? QStringLiteral("目标卫星")
                                                        : QStringLiteral("可见性分析目标"));
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
                                       ? QStringLiteral("搜索步长")
                                       : QStringLiteral("最大检查间隔"))
                                : QStringLiteral("采样间隔"));
        step->setToolTip(report_uses_event_search(draft.query.kind)
                             ? QStringLiteral("事件搜索的最大检查间隔；快速变化时减半核对收敛。事件"
                                              "时刻自动细化，不限于检查节点。")
                             : QStringLiteral("报告独立采样，始终包含起止时刻。"));
        if (draft.query.kind == ReportKind::relative_motion) {
            step_label->setText(QStringLiteral("采样 / 检查间隔"));
            step->setToolTip(QStringLiteral("采样输出包含起止时刻；此间隔也用于可见性和连续距离极值"
                                            "搜索。快速变化时减半检查收敛。"));
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
                                  ? QStringLiteral("百分比 (%)")
                                  : QStringLiteral("km / km·s⁻¹ / °"));
        units->setItemText(1, draft.query.kind == ReportKind::lighting_state
                                  ? QStringLiteral("比例 (0–1)")
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
                ? QStringLiteral("场景时段、对象数量与中心天体")
            : draft.query.kind == ReportKind::object_summary
                ? QStringLiteral("对象名称、类型与计算模型")
            : draft.query.kind == ReportKind::cartesian_state
                ? QStringLiteral("按指定间隔输出位置与速度")
            : draft.query.kind == ReportKind::classical_elements
                ? QStringLiteral("GCRF 状态换算的瞬时根数；圆轨道 / 赤道轨道的奇异角采用约定值。J2 "
                                 "结果不是平均根数。")
            : draft.query.kind == ReportKind::geodetic_position
                ? QStringLiteral("ITRF / WGS84 经度、大地纬度和椭球高；高度不表示离地距离。")
            : draft.query.kind == ReportKind::orbital_motion
                ? QStringLiteral("GCRF 地心距离、总速率、径向 / 横向速率与航迹倾角；径向向外为正。")
                : QStringLiteral("WGS84 经纬度、高度与 ITRF 坐标"));
        template_description->setToolTip(template_description->text());
        if (draft.query.kind == ReportKind::classical_elements)
            template_description->setText(QStringLiteral("瞬时六根数 · GCRF"));
        if (draft.query.kind == ReportKind::state_covariance)
            template_description->setText(QStringLiteral("位置速度协方差下三角 · GCRF"));
        if (draft.query.kind == ReportKind::orbital_motion)
            template_description->setText(QStringLiteral("地心距离、速率与航迹倾角 · GCRF"));
        if (draft.query.kind == ReportKind::geodetic_position)
            template_description->setText(QStringLiteral("经度、大地纬度与椭球高 · WGS84"));
        if (draft.query.kind == ReportKind::orbital_events) {
            template_description->setText(QStringLiteral(
                "近远地点 · GCRF 赤道交点\n连续求根定位事件；双击结果可跳转场景时间。"));
            template_description->setToolTip(QStringLiteral(
                "地心距离的极值与 GCRF 赤道面穿越。e ≤ 1e-8 时不定义近远地点；|sin(i)| ≤ 1e-8 "
                "时不定义交点。不支持含机动的任务序列。快速变化时减小检查间隔并核对收敛。"));
        }
        if (report_is_intersatellite(draft.query.kind)) {
            template_description->setText(
                draft.query.kind == ReportKind::relative_motion
                    ? QStringLiteral("距离与 RIC 相对位置 · 基准 → "
                                     "目标\n距离变化率向外为正；可见性采用 WGS84 椭球。")
                    : QStringLiteral("星间视线 · WGS84 "
                                     "椭球\n可见时段、持续时间与每段最近距离；双击定位时刻。"));
            template_description->setToolTip(QStringLiteral(
                "连续距离极值包含不可见时段。RIC "
                "径向向外、沿轨由法向与径向确定、法向为基准轨道角动量方向。惯性相对速率不是 RIC "
                "坐标导数。不含地形、折射或光行时；暂不支持任务序列。"));
        }
        if (coverage) {
            template_description->setText(QStringLiteral(
                "联合覆盖 · WGS84 地表网格\n以网格中心可见性代表网格；减小间隔可核对空间收敛。"));
            units->setItemText(0, QStringLiteral("km² / ° / %"));
            units->setItemText(1, QStringLiteral("m² / rad / 比例"));
        }
        if (report_is_lighting(draft.query.kind))
            template_description->setText(
                QStringLiteral(
                    "地球球面 · 几何太阳 · %1\n全光照 / 半影 / 本影；与卫星的光压力开关无关。")
                    .arg(text(draft.query.lighting_ephemeris)));
        if (report_is_access(draft.query.kind))
            template_description->setText(
                draft.query.kind == ReportKind::access_intervals
                    ? QStringLiteral("地球遮挡 · 最低仰角 · "
                                     "传感器视场\n输出过境区间、最大仰角与最近距离；快速变化时减小"
                                     "检查间隔并核对收敛。")
                    : QStringLiteral(
                          "当地北向起算方位角，水平线起算仰角。包含不可见时刻，以 0 / 1 标记。"));
        remove->setEnabled(row >= builtin_count);
        update_template->setEnabled(row >= builtin_count);
        store->setText(row >= builtin_count ? QStringLiteral("另存为模板…")
                                            : QStringLiteral("保存为模板…"));
        fill_library();
        loading = false;
        edit();
    }
    void seek_result(core::Epoch epoch) {
        if (epoch.tai_nanoseconds() < project.first.tai_nanoseconds() ||
            epoch.tai_nanoseconds() > project.last.tai_nanoseconds())
            status->setText(QStringLiteral("该结果时间已超出当前场景，请刷新报告。"));
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
                    status->setText(QStringLiteral("显示格式已更新。"));
                }
            } catch (const std::exception &error) {
                status->setText(
                    QStringLiteral("无法应用显示格式：%1").arg(QString::fromUtf8(error.what())));
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
                status->setText(QStringLiteral("报告或设置已更新，请重新选择显示列。"));
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
            status->setText(QStringLiteral("显示列已更新，可保存为模板；未重新计算。"));
        } catch (const std::exception &error) {
            status->setText(
                QStringLiteral("无法应用显示列：%1").arg(QString::fromUtf8(error.what())));
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
            const std::array captions{QStringLiteral("可见过境"), QStringLiteral("总可见时长"),
                                      QStringLiteral("最长过境"), QStringLiteral("平均时长")};
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
            pass_summary->setToolTip(
                QStringLiteral("按当前分析区间统计，包含被区间边界截断的过境。最短过境：%"
                               "1。双击最大仰角或最近距离可定位到对应时刻。")
                    .arg(s.count ? duration(s.shortest_duration_s) : QStringLiteral("无")));
        }
        if (result->intersatellite_statistics) {
            const auto &s = *result->intersatellite_statistics;
            const std::array captions{QStringLiteral("最近距离"), QStringLiteral("最远距离"),
                                      QStringLiteral("可见时段"), QStringLiteral("总可见时长")};
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
            pass_summary->setToolTip(QStringLiteral(
                "距离极值覆盖整个分析区间，包含不可见时段；时段统计包含分析边界截断部分。"));
            event_notice->setText(QStringLiteral(
                "<a href=\"closest-approach\">定位全区间最近时刻</a> · 距离极值包含不可见时段"));
            event_notice->show();
        }
        if (result->lighting_statistics) {
            const auto &s = *result->lighting_statistics;
            const std::array captions{QStringLiteral("全光照占比"), QStringLiteral("半影占比"),
                                      QStringLiteral("本影占比"), QStringLiteral("最长本影")};
            for (int i = 0; i < 4; ++i)
                pass_captions[i]->setText(captions[i]);
            for (int i = 0; i < 3; ++i) {
                pass_values[i]->setText(
                    QStringLiteral("%1 %").arg(100 * s.phases[i].fraction, 0, 'f', 2));
                pass_values[i]->setToolTip(QStringLiteral("%1 段 · 累计 %2 秒")
                                               .arg(s.phases[i].count)
                                               .arg(s.phases[i].total_duration_s, 0, 'f', 6));
            }
            pass_values[3]->setText(
                QStringLiteral("%1 min").arg(s.phases[2].longest_duration_s / 60, 0, 'f', 2));
            pass_summary->setToolTip(QStringLiteral("按 %1 "
                                                    "颗卫星分别累计，包含分析边界截断部分。占比以所"
                                                    "选卫星的累计分析时长为分母。双击时刻定位。")
                                         .arg(s.object_count));
        }
        if (result->orbital_event_statistics) {
            const auto &s = *result->orbital_event_statistics;
            const std::array captions{QStringLiteral("近地点"), QStringLiteral("远地点"),
                                      QStringLiteral("升交点"), QStringLiteral("降交点")};
            for (int i = 0; i < 4; ++i) {
                pass_captions[i]->setText(captions[i]);
                pass_values[i]->setText(QString::number(s.counts[i]));
            }
            pass_summary->setToolTip(
                QStringLiteral(
                    "%1 颗卫星的事件数量；交点参考面为 GCRF 赤道面。双击任意一行定位该事件时刻。")
                    .arg(s.object_count));
            QStringList notes;
            if (s.near_circular_objects)
                notes << QStringLiteral("%1 颗卫星存在近圆状态，相应近远地点不定义")
                             .arg(s.near_circular_objects);
            if (s.near_equatorial_objects)
                notes << QStringLiteral("%1 颗卫星存在近赤道状态，相应交点不定义")
                             .arg(s.near_equatorial_objects);
            event_notice->setText(notes.join(QStringLiteral("；")));
            event_notice->setVisible(!notes.isEmpty());
        }
        views->setTabEnabled(2, result->coverage_statistics.has_value());
        views->setTabVisible(2, views->isTabEnabled(2));
        coverage_map->set_result(result, new_calculation);
        if (result->coverage_statistics) {
            const auto &s = *result->coverage_statistics;
            const std::array captions{QStringLiteral("网格点"), QStringLiteral("曾覆盖面积占比"),
                                      QStringLiteral("平均覆盖率"), QStringLiteral("最长空档")};
            for (int i = 0; i < 4; ++i)
                pass_captions[i]->setText(captions[i]);
            pass_values[0]->setText(QString::number(s.points.size()));
            pass_values[1]->setText(QStringLiteral("%1 %").arg(
                100 * s.ever_covered_area_m2 / s.total_area_m2, 0, 'f', 2));
            pass_values[2]->setText(
                QStringLiteral("%1 %").arg(100 * s.mean_coverage_fraction, 0, 'f', 2));
            pass_values[3]->setText(
                QStringLiteral("%1 h").arg(s.maximum_gap_seconds / 3600, 0, 'f', 2));
            pass_summary->setToolTip(QStringLiteral(
                "按 WGS84 "
                "网格面积加权；平均覆盖率为覆盖面积随时间的平均值。最长空档按所选首尾策略统计。"));
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
            QStringLiteral("网格 %1 · 经度 %2° / 纬度 %3° · 面积 %4 km²")
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
                                                   ? QStringLiteral(" · 边界截断")
                                                   : QString{}));
        }
        if (p.windows.empty())
            coverage_windows->addItem(QStringLiteral("此网格点在分析时段内无覆盖"));
        coverage_windows->setToolTip(
            QStringLiteral("相对于分析开始时刻的秒数；多资产重叠区间已合并。"));
        coverage_jump->setEnabled(!p.windows.empty());
    }
    void update_estimate() {
        try {
            const auto d = definition();
            const auto rows = runtime::estimate_report_rows(project, d);
            if (report_is_coverage(d.query.kind)) {
                estimate->setText(QStringLiteral("%1 个网格点 · %2 个资产 · %3 行输出")
                                      .arg(core::coverage_grid(d.query.coverage).size())
                                      .arg(d.query.object_ids.size())
                                      .arg(rows));
                return;
            }
            estimate->setText((report_uses_event_search(d.query.kind)
                                   ? QStringLiteral("%1 个检查节点 · 自动细化%2")
                                   : QStringLiteral("预计 %1 行%2"))
                                  .arg(static_cast<qulonglong>(rows))
                                  .arg(report_is_sampled(d.query.kind)
                                           ? QStringLiteral(" · 含起止时刻")
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
        result_label->setText(QStringLiteral("%1 行%2")
                                  .arg(static_cast<qulonglong>(r.table.rows().size()))
                                  .arg(stale ? QStringLiteral(" · 场景已变化，请重新生成")
                                       : changed
                                           ? QStringLiteral(" · 设置已更改 · 当前显示上次结果")
                                           : QStringLiteral(" · 与当前场景一致")));
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
            status->setText(QStringLiteral("正在检查场景数据…"));
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
                        self.status->setText(
                            QStringLiteral("场景在准备期间已变化，请重新生成；上次结果保留。"));
                        return;
                    }
                    self.expected = runtime::estimate_report_rows(context->project, d);
                    self.job_generation = self.input_generation;
                    self.cancellation = std::make_shared<std::atomic_bool>(false);
                    self.completed = std::make_shared<std::atomic_size_t>(0);
                    self.progress->setRange(0, static_cast<int>(self.expected));
                    self.progress->setValue(0);
                    self.status->setText(QStringLiteral("正在生成报告…"));
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
                    self.status->setText(
                        QStringLiteral("无法启动报告：%1").arg(QString::fromUtf8(failure.what())));
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
                naming.setWindowTitle(QStringLiteral("保存报告模板"));
                naming.setLabelText(QStringLiteral("模板名称"));
                naming.setTextValue(text(d.name));
                naming.setOkButtonText(QStringLiteral("保存模板"));
                naming.setCancelButtonText(QStringLiteral("取消"));
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
            store->setText(QStringLiteral("另存为模板…"));
            result_status();
            status->setText(QStringLiteral("模板已加入项目；保存项目后写入磁盘。"));
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
            status->setText(QStringLiteral("已删除配置；保存项目后写入磁盘。"));
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
        export_csv->setText(chart ? QStringLiteral("导出 PNG…") : QStringLiteral("导出 CSV…"));
        copy_button->setText(chart ? QStringLiteral("复制图片") : QStringLiteral("复制选中行"));
        copy_button->setToolTip(chart ? QStringLiteral("复制当前图片")
                                      : QStringLiteral("复制所选行（Ctrl+C）"));
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
                                ? QStringLiteral("曲线 · %1").arg(names.front())
                                : QStringLiteral("选择曲线 · %1 项").arg(names.size()));
        plot_field->setAccessibleDescription(names.join(QStringLiteral("、")));
        plot_field->setToolTip(QStringLiteral("选择要比较的曲线\n%1").arg(names.join("\n")));
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
            status->setText(QStringLiteral("报告结果已更新，请重新选择曲线。"));
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
                    QStringLiteral("曲线时间标签不可用：%1。请检查时间数据或选择 TAI / TT。")
                        .arg(QString::fromUtf8(error.what()));
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
            plot_object->set_options({QStringLiteral("选择曲线显示对象"), false,
                                      objects.size() <= 12
                                          ? QStringLiteral("全部结果对象（%1）").arg(objects.size())
                                          : QString{}});
            plot_object->set_objects(std::move(choices));
            if (plot_object->contains(previous_object))
                plot_object->set_id(previous_object);
            else if (objects.size() <= 12)
                plot_object->set_selection({{}, true});
            else
                plot_object->set_id(objects.begin()->first);
            plot_note->setText(
                objects.size() > 12
                    ? QStringLiteral(
                          "对象超过 12 个，选择单个查看或在左侧缩小范围。滚轮缩放，双击定位时刻。")
                    : QStringLiteral("滚轮缩放 · 中键平移 · 双击采样点定位时刻"));
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
            QFileDialog chooser(owner, QStringLiteral("导出图片"));
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
            status->setText(ok ? QStringLiteral("图片已导出（2 倍分辨率）。")
                               : QStringLiteral("图片导出失败：%1").arg(output.errorString()));
            return;
        }
        QFileDialog chooser(owner, QStringLiteral("导出报告"));
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
        status->setText(QStringLiteral("正在导出上方结果…"));
        try {
            export_job = std::async(
                std::launch::async, [result = model->result, path, cancel = cancellation] {
                    runtime::export_report_csv(*result, path, true, cancel.get());
                });
        } catch (const std::exception &error) {
            set_busy(false);
            status->setText(
                QStringLiteral("无法启动导出：%1").arg(QString::fromUtf8(error.what())));
        }
    }
    void copy_rows() {
        if (!model->result)
            return;
        auto rows = table->selectionModel()->selectedRows();
        if (rows.isEmpty()) {
            status->setText(QStringLiteral("请先选择要复制的行。"));
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
        status->setText(
            QStringLiteral("已复制 %1 行（含列标题，保留完整精度）。").arg(rows.size()));
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
                    status->setText(QStringLiteral("报告已生成。"));
                }
            } catch (const runtime::AnalysisCancelled &) {
                status->setText(QStringLiteral("已取消；上次结果保留。"));
            } catch (const runtime::ReportCancelled &) {
                status->setText(QStringLiteral("已取消；上次结果保留。"));
            } catch (const std::exception &error) {
                status->setText(QStringLiteral("生成失败；上次结果保留。%1")
                                    .arg(QString::fromUtf8(error.what())));
            }
            if (job_generation != input_generation)
                status->setText(
                    QStringLiteral("场景已变化，未发布过期报告；上次结果保留。请重新生成。"));
            set_busy(false);
        }
        if (export_job.valid() &&
            export_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                export_job.get();
                status->setText(QStringLiteral("CSV 已导出。"));
            } catch (const runtime::ReportCancelled &) {
                status->setText(QStringLiteral("已取消导出；原文件保留。"));
            } catch (const std::exception &error) {
                status->setText(
                    QStringLiteral("导出失败：%1").arg(QString::fromUtf8(error.what())));
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
        self.status->setText(QStringLiteral("报告任务正在运行；完成或取消后可切换对象。"));
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
