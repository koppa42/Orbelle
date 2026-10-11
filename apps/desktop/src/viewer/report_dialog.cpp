#include "report_dialog.hpp"
#include "epoch_editor.hpp"
#include "workbench_style.hpp"
#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <future>
#include <map>

namespace orbitforge::desktop {
namespace {
using namespace application;
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
    }
    return {};
}
QString localized(QString value) {
    static const std::map<QString, QString> labels{
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
        if (role == Qt::TextAlignmentRole &&
            (std::holds_alternative<double>(cell) || std::holds_alternative<std::int64_t>(cell)))
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
            return {};
        auto value = text(result->cell_text(index.row(), index.column()));
        if (role == Qt::DisplayRole && std::holds_alternative<double>(cell))
            return QString::number(value.toDouble(), 'g', 12);
        if (role == Qt::DisplayRole && std::holds_alternative<core::Epoch>(cell)) {
            if (value.endsWith('Z'))
                value.chop(1);
            while (value.endsWith('0'))
                value.chop(1);
            if (value.endsWith('.'))
                value.chop(1);
            return value.replace('T', ' ');
        }
        const auto &id = result->table.columns().at(index.column()).id;
        if (id == "object.type" || id == "object.model" || id == "summary.field" ||
            (id == "summary.value" && index.row() == 6))
            return localized(value);
        return value;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (!result || role != Qt::DisplayRole)
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
    Project project;
    Prepare prepare;
    Save save;
    std::function<void(core::Epoch)> seek;
    std::shared_ptr<const core::ITimeProvider> time;
    QListWidget *library, *objects;
    QTabWidget *tabs;
    QWidget *settings, *object_fields, *sampling;
    QLineEdit *name;
    QCheckBox *all_objects, *follow;
    QComboBox *frame, *units, *scale, *compact_library;
    QWidget *sidebar;
    QVBoxLayout *root_layout;
    std::vector<QFormLayout *> forms;
    EpochEditor *first, *last;
    QDoubleSpinBox *step;
    QLabel *status, *estimate, *result_label;
    QPlainTextEdit *source;
    QPushButton *generate, *store, *remove, *duplicate, *export_csv, *cancel;
    QProgressBar *progress;
    QTableView *table;
    ReportModel *model;
    ReportDefinition draft;
    std::future<runtime::ReportResult> job;
    std::future<void> export_job;
    std::shared_ptr<std::atomic_bool> cancellation;
    std::shared_ptr<std::atomic_size_t> completed;
    bool waiting = false, loading = false, changed = false;
    std::size_t expected = 0;
    std::uint64_t request = 0;

    Impl(ReportDialog *parent, Project input, std::shared_ptr<const core::ITimeProvider> clock,
         Prepare preflight, Save persist, std::function<void(core::Epoch)> jump)
        : owner(parent), project(std::move(input)), prepare(std::move(preflight)),
          save(std::move(persist)), seek(std::move(jump)), time(std::move(clock)) {
        owner->setObjectName("reportDialog");
        owner->setWindowTitle(QStringLiteral("报告"));
        owner->setWindowIcon(workbench_icon(WorkbenchIcon::properties));
        owner->resize(1080, 730);
        auto *root = new QVBoxLayout(owner);
        root_layout = root;
        root->setSizeConstraint(QLayout::SetNoConstraint);
        root->setContentsMargins(24, 22, 24, 18);
        root->setSpacing(16);
        auto *title = new QLabel(QStringLiteral("报告"), owner);
        title->setStyleSheet("font-size: 22px; font-weight: 600;");
        root->addWidget(title);
        root->addWidget(
            note(QStringLiteral("选择模板与对象，生成可追溯的数据表。配置随项目保存。"), owner));
        compact_library = new QComboBox(owner);
        compact_library->setObjectName("reportCompactLibrary");
        compact_library->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        compact_library->setMinimumContentsLength(12);
        compact_library->hide();
        root->addWidget(compact_library);
        auto *split = new QSplitter(owner);
        root->addWidget(split, 1);
        auto *left = new QWidget(split);
        sidebar = left;
        auto *left_layout = new QVBoxLayout(left);
        left_layout->setContentsMargins(0, 0, 16, 0);
        left_layout->setSpacing(12);
        left_layout->addWidget(note(QStringLiteral("模板 / 已保存的报告"), left));
        library = new QListWidget(left);
        library->setObjectName("reportLibrary");
        library->setSpacing(4);
        library->setMinimumWidth(170);
        left_layout->addWidget(library, 1);
        remove = new QPushButton(QStringLiteral("删除已保存的配置"), left);
        remove->setObjectName("deleteReport");
        remove->setMaximumWidth(220);
        tabs = new QTabWidget(split);
        tabs->setObjectName("reportTabs");
        split->setStretchFactor(0, 0);
        split->setStretchFactor(1, 1);
        split->setSizes({210, 790});
        auto *scroll = new QScrollArea(tabs);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        settings = new QWidget(scroll);
        settings->setObjectName("reportSettings");
        scroll->setWidget(settings);
        auto *config = new QVBoxLayout(settings);
        config->setContentsMargins(20, 22, 20, 20);
        config->setSpacing(18);
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(20);
        form->setVerticalSpacing(14);
        name = new QLineEdit(settings);
        name->setObjectName("reportName");
        name->setMaximumWidth(480);
        form->addRow(QStringLiteral("名称"), name);
        config->addLayout(form);
        object_fields = new QWidget(settings);
        auto *object_layout = new QVBoxLayout(object_fields);
        object_layout->setContentsMargins(0, 0, 0, 0);
        object_layout->setSpacing(10);
        all_objects =
            new QCheckBox(QStringLiteral("所有适用对象（含以后插入的对象）"), object_fields);
        all_objects->setObjectName("reportAllObjects");
        all_objects->setToolTip(
            QStringLiteral("包含当前和以后插入的所有适用对象；关闭后选择具体对象。"));
        object_layout->addWidget(all_objects);
        objects = new QListWidget(object_fields);
        objects->setObjectName("reportObjects");
        objects->setMaximumHeight(125);
        object_layout->addWidget(objects);
        config->addWidget(object_fields);
        sampling = new QWidget(settings);
        auto *sampling_form = new QFormLayout(sampling);
        sampling_form->setContentsMargins(0, 0, 0, 0);
        sampling_form->setHorizontalSpacing(20);
        sampling_form->setVerticalSpacing(12);
        follow = new QCheckBox(QStringLiteral("使用场景起止时间"), sampling);
        follow->setObjectName("reportFollowScenario");
        sampling_form->addRow(QStringLiteral("时间范围"), follow);
        first = new EpochEditor(sampling, time);
        first->setObjectName("reportFirst");
        last = new EpochEditor(sampling, time);
        last->setObjectName("reportLast");
        sampling_form->addRow(QStringLiteral("开始"), first);
        sampling_form->addRow(QStringLiteral("结束"), last);
        step = new StepSpinBox(sampling);
        step->setObjectName("reportStep");
        step->setDecimals(9);
        step->setRange(0.000000001, 86400);
        step->setValue(60);
        step->setSuffix(QStringLiteral(" 秒"));
        step->setMaximumWidth(260);
        sampling_form->addRow(QStringLiteral("采样间隔"), step);
        frame = new QComboBox(sampling);
        frame->setObjectName("reportFrame");
        frame->addItems({"GCRF", "ITRF"});
        frame->setMaximumWidth(260);
        sampling_form->addRow(QStringLiteral("输出坐标系"), frame);
        config->addWidget(sampling);
        auto *format = new QFormLayout;
        format->setHorizontalSpacing(20);
        format->setVerticalSpacing(12);
        units = new QComboBox(settings);
        units->setObjectName("reportUnits");
        units->addItems(
            {QStringLiteral("工程单位 · km、km/s、°"), QStringLiteral("SI 单位 · m、m/s、rad")});
        scale = new QComboBox(settings);
        scale->setObjectName("reportTimeScale");
        scale->addItems({"UTC", "TAI", "TT"});
        units->setMaximumWidth(360);
        scale->setMaximumWidth(260);
        format->addRow(QStringLiteral("数值单位"), units);
        format->addRow(QStringLiteral("时间系统"), scale);
        config->addLayout(format);
        for (auto *group : {form, sampling_form, format})
            for (int row = 0; row < group->rowCount(); ++row)
                if (auto *item = group->itemAt(row, QFormLayout::LabelRole); item && item->widget())
                    item->widget()->setMinimumWidth(84);
        forms = {form, sampling_form, format};
        estimate = note({}, settings);
        estimate->setObjectName("reportEstimate");
        config->addWidget(estimate);
        duplicate = new QPushButton(QStringLiteral("另存为新配置"), settings);
        duplicate->setObjectName("duplicateReport");
        duplicate->setMaximumWidth(220);
        config->addWidget(duplicate);
        config->addWidget(remove);
        config->addStretch();
        tabs->addTab(scroll, QStringLiteral("配置"));
        auto *result_page = new QWidget(tabs);
        auto *result_layout = new QVBoxLayout(result_page);
        result_layout->setContentsMargins(16, 18, 16, 12);
        result_layout->setSpacing(12);
        result_label = note(QStringLiteral("生成后在这里查看数据。"), result_page);
        result_label->setObjectName("reportResultLabel");
        result_layout->addWidget(result_label);
        table = new QTableView(result_page);
        table->setObjectName("reportTable");
        model = new ReportModel(table);
        table->setModel(model);
        table->setAlternatingRowColors(true);
        table->setShowGrid(false);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->setDefaultSectionSize(30);
        table->horizontalHeader()->setMinimumSectionSize(75);
        table->horizontalHeader()->setResizeContentsPrecision(50);
        auto *copy = new QAction(QStringLiteral("复制所选行"), table);
        copy->setObjectName("copyReportRows");
        copy->setShortcut(QKeySequence::Copy);
        copy->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        table->addAction(copy);
        table->setContextMenuPolicy(Qt::ActionsContextMenu);
        QObject::connect(copy, &QAction::triggered, owner, [this] { copy_rows(); });
        result_layout->addWidget(table, 1);
        auto *export_row = new QHBoxLayout;
        export_row->addWidget(note(QStringLiteral("双击时间行，可定位场景时间。"), result_page), 1);
        export_csv = new QPushButton(QStringLiteral("导出 CSV…"), result_page);
        export_csv->setObjectName("exportReportCsv");
        export_csv->setEnabled(false);
        export_row->addWidget(export_csv);
        result_layout->addLayout(export_row);
        tabs->addTab(result_page, QStringLiteral("结果"));
        source = new QPlainTextEdit(tabs);
        source->setObjectName("reportSource");
        source->setReadOnly(true);
        source->setPlaceholderText(QStringLiteral("生成后记录输入、数据哈希与算法版本。"));
        tabs->addTab(source, QStringLiteral("来源"));
        status = note({}, owner);
        status->setObjectName("reportStatus");
        root->addWidget(status);
        auto *footer = new QHBoxLayout;
        footer->setSpacing(12);
        progress = new QProgressBar(owner);
        progress->setObjectName("reportProgress");
        progress->hide();
        root->addWidget(progress);
        footer->addStretch();
        store = new QPushButton(QStringLiteral("保存配置"), owner);
        store->setObjectName("saveReport");
        footer->addWidget(store);
        cancel = new QPushButton(QStringLiteral("取消"), owner);
        cancel->setObjectName("cancelReport");
        cancel->hide();
        footer->addWidget(cancel);
        generate = new QPushButton(QStringLiteral("生成报告"), owner);
        generate->setObjectName("generateReport");
        generate->setProperty("primary", true);
        generate->setDefault(true);
        footer->addWidget(generate);
        root->addLayout(footer);
        QObject::connect(library, &QListWidget::currentRowChanged, owner,
                         [this](int row) { choose(row); });
        QObject::connect(compact_library, &QComboBox::currentIndexChanged, owner,
                         [this](int row) { library->setCurrentRow(row); });
        QObject::connect(all_objects, &QCheckBox::toggled, owner, [this] {
            objects->setVisible(!all_objects->isChecked());
            edit();
        });
        QObject::connect(follow, &QCheckBox::toggled, owner, [this] {
            first->setEnabled(!follow->isChecked());
            last->setEnabled(!follow->isChecked());
            edit();
        });
        QObject::connect(name, &QLineEdit::textEdited, owner, [this] { edit(); });
        QObject::connect(objects, &QListWidget::itemChanged, owner, [this] { edit(); });
        for (auto *combo : {frame, units, scale})
            QObject::connect(combo, &QComboBox::currentIndexChanged, owner, [this] { edit(); });
        QObject::connect(scale, &QComboBox::currentIndexChanged, owner, [this](int index) {
            first->set_scale(static_cast<CalendarScale>(index));
            last->set_scale(static_cast<CalendarScale>(index));
        });
        QObject::connect(step, &QDoubleSpinBox::valueChanged, owner, [this] { edit(); });
        for (auto *editor : {first, last})
            for (auto *line : editor->findChildren<QLineEdit *>())
                QObject::connect(line, &QLineEdit::editingFinished, owner, [this] { edit(); });
        QObject::connect(generate, &QPushButton::clicked, owner, [this] { start(); });
        QObject::connect(store, &QPushButton::clicked, owner, [this] { this->persist(); });
        QObject::connect(duplicate, &QPushButton::clicked, owner, [this] { this->persist(true); });
        QObject::connect(remove, &QPushButton::clicked, owner, [this] { erase(); });
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
            table, &QTableView::doubleClicked, owner, [this](const QModelIndex &index) {
                if (!model->result)
                    return;
                for (const auto &cell : model->result->table.rows().at(index.row()))
                    if (const auto *epoch = std::get_if<core::Epoch>(&cell)) {
                        if (epoch->tai_nanoseconds() < project.first.tai_nanoseconds() ||
                            epoch->tai_nanoseconds() > project.last.tai_nanoseconds())
                            status->setText(
                                QStringLiteral("该结果时间已超出当前场景，请刷新报告。"));
                        else
                            seek(*epoch);
                        break;
                    }
            });
        auto *timer = new QTimer(owner);
        timer->setInterval(80);
        QObject::connect(timer, &QTimer::timeout, owner, [this] { poll(); });
        timer->start();
        fill_library();
        library->setCurrentRow(0);
    }
    ~Impl() {
        if (cancellation)
            cancellation->store(true);
    }
    void fill_library() {
        const int previous = library->currentRow();
        QSignalBlocker guard(library);
        QSignalBlocker compact_guard(compact_library);
        library->clear();
        compact_library->clear();
        for (auto kind : {ReportKind::scenario_summary, ReportKind::object_summary,
                          ReportKind::cartesian_state, ReportKind::ground_stations}) {
            auto *item = new QListWidgetItem(kind_name(kind), library);
            item->setData(Qt::UserRole, static_cast<int>(kind));
            item->setSizeHint({160, 42});
            compact_library->addItem(kind_name(kind));
        }
        for (const auto &definition : project.reports) {
            auto *item = new QListWidgetItem(text(definition.name), library);
            item->setIcon(workbench_icon(WorkbenchIcon::save));
            item->setData(Qt::UserRole, text(definition.id));
            item->setToolTip(kind_name(definition.kind));
            item->setSizeHint({160, 42});
            compact_library->addItem(workbench_icon(WorkbenchIcon::save), text(definition.name));
        }
        for (int i = 4; i < library->count(); ++i)
            if (library->item(i)->data(Qt::UserRole).toString() == text(draft.id))
                library->setCurrentRow(i);
        if (library->currentRow() < 0 && previous >= 0 && previous < 4)
            library->setCurrentRow(static_cast<int>(draft.kind));
        compact_library->setCurrentIndex(library->currentRow());
    }
    void adapt(int width) {
        const bool compact = width < 850;
        sidebar->setVisible(!compact);
        compact_library->setVisible(compact);
        root_layout->setContentsMargins(compact ? 12 : 24, compact ? 14 : 22, compact ? 12 : 24,
                                        14);
        root_layout->setSpacing(compact ? 10 : 16);
        all_objects->setText(compact ? QStringLiteral("所有适用对象")
                                     : QStringLiteral("所有适用对象（含以后插入的对象）"));
        for (auto *form : forms)
            form->setRowWrapPolicy(width < 540 ? QFormLayout::WrapAllRows
                                               : QFormLayout::DontWrapRows);
        for (auto *editor : {first, last})
            editor->findChild<QLineEdit *>("epochValue")->setMinimumWidth(compact ? 180 : 265);
        settings->layout()->setContentsMargins(compact ? 12 : 20, 18, compact ? 12 : 20, 18);
    }
    void fill_objects() {
        QSignalBlocker guard(objects);
        objects->clear();
        auto remaining = draft.object_ids;
        const auto append = [&](const std::string &id, const std::string &label,
                                bool missing = false) {
            auto *item = new QListWidgetItem(
                text(label) + (missing ? QStringLiteral(" · 对象已不存在") : QString{}), objects);
            item->setData(Qt::UserRole, text(id));
            item->setToolTip(text(id));
            item->setCheckState(std::find(draft.object_ids.begin(), draft.object_ids.end(), id) !=
                                        draft.object_ids.end()
                                    ? Qt::Checked
                                    : Qt::Unchecked);
            std::erase(remaining, id);
        };
        if (draft.kind != ReportKind::ground_stations)
            for (const auto &satellite : project.satellites)
                append(satellite.orbit.object_id, satellite.name());
        for (const auto &station : project.ground_stations)
            append(station.object_id, station.name());
        const auto missing = remaining;
        for (const auto &id : missing)
            append(id, id, true);
    }
    void choose(int row) {
        if (row < 0 || loading)
            return;
        loading = true;
        const QSignalBlocker compact_guard(compact_library);
        compact_library->setCurrentIndex(row);
        if (row < 4) {
            draft = {};
            draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            draft.kind = static_cast<ReportKind>(row);
            draft.name = kind_name(draft.kind).toStdString();
            draft.first = project.first;
            draft.last = project.last;
        } else
            draft = project.reports.at(static_cast<std::size_t>(row - 4));
        name->setText(text(draft.name));
        all_objects->setChecked(draft.object_ids.empty());
        follow->setChecked(draft.follow_scenario);
        first->set_epoch(draft.follow_scenario ? project.first : draft.first);
        last->set_epoch(draft.follow_scenario ? project.last : draft.last);
        step->setValue(static_cast<double>(draft.step_nanoseconds) / 1e9);
        frame->setCurrentIndex(draft.frame == core::FrameId::gcrf ? 0 : 1);
        units->setCurrentIndex(draft.units == ReportUnits::engineering ? 0 : 1);
        scale->setCurrentIndex(static_cast<int>(draft.time_scale));
        first->setEnabled(!draft.follow_scenario);
        last->setEnabled(!draft.follow_scenario);
        objects->setVisible(!draft.object_ids.empty());
        fill_objects();
        object_fields->setVisible(draft.kind != ReportKind::scenario_summary);
        sampling->setVisible(draft.kind == ReportKind::cartesian_state);
        units->setEnabled(draft.kind == ReportKind::cartesian_state ||
                          draft.kind == ReportKind::ground_stations);
        scale->setEnabled(draft.kind == ReportKind::cartesian_state ||
                          draft.kind == ReportKind::scenario_summary);
        remove->setEnabled(row >= 4);
        tabs->setCurrentIndex(0);
        loading = false;
        edit();
    }
    ReportDefinition definition() const {
        auto d = draft;
        d.name = name->text().trimmed().toStdString();
        d.object_ids.clear();
        if (!all_objects->isChecked() && d.kind != ReportKind::scenario_summary) {
            for (int i = 0; i < objects->count(); ++i)
                if (objects->item(i)->checkState() == Qt::Checked)
                    d.object_ids.push_back(
                        objects->item(i)->data(Qt::UserRole).toString().toStdString());
            if (d.object_ids.empty())
                throw std::invalid_argument("请选择至少一个对象，或启用所有适用对象。");
        }
        d.follow_scenario = follow->isChecked();
        if (d.kind == ReportKind::cartesian_state && !d.follow_scenario) {
            d.first = first->epoch();
            d.last = last->epoch();
        }
        d.step_nanoseconds = static_cast<std::int64_t>(std::llround(step->value() * 1e9));
        d.frame = frame->currentIndex() == 0 ? core::FrameId::gcrf : core::FrameId::itrf;
        d.units = units->currentIndex() == 0 ? ReportUnits::engineering : ReportUnits::si;
        d.time_scale = static_cast<ReportTimeScale>(scale->currentIndex());
        validate_report_definition(d);
        return d;
    }
    void edit() {
        if (loading)
            return;
        changed = true;
        try {
            const auto d = definition();
            const auto rows = runtime::estimate_report_rows(project, d);
            estimate->setText(
                QStringLiteral("预计 %1 行%2")
                    .arg(static_cast<qulonglong>(rows))
                    .arg(d.kind == ReportKind::cartesian_state
                             ? QStringLiteral(" · 包含起止时刻，末段不足一个间隔时仍保留结束时刻。")
                             : QString{}));
        } catch (const std::exception &error) {
            estimate->setText(QString::fromUtf8(error.what()));
        }
        result_status();
    }
    void result_status() {
        if (!model->result)
            return;
        const auto &r = *model->result;
        const bool stale = r.input_sha256 != runtime::report_input_sha256(project);
        result_label->setText(QStringLiteral("%1 · %2 行%3")
                                  .arg(text(r.definition.name))
                                  .arg(static_cast<qulonglong>(r.table.rows().size()))
                                  .arg(stale ? QStringLiteral(" · 场景已变化，请重新生成")
                                       : changed
                                           ? QStringLiteral(" · 配置已更改，当前仍显示上次结果")
                                           : QStringLiteral(" · 与当前场景一致")));
    }
    void set_busy(bool value) {
        settings->setEnabled(!value);
        library->setEnabled(!value);
        compact_library->setEnabled(!value);
        generate->setEnabled(!value);
        store->setEnabled(!value);
        duplicate->setEnabled(!value);
        remove->setEnabled(!value &&
                           std::any_of(project.reports.begin(), project.reports.end(),
                                       [this](const auto &r) { return r.id == draft.id; }));
        export_csv->setEnabled(!value && bool(model->result));
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
            prepare([guard, ticket, d](std::optional<runtime::ReportContext> context,
                                       QString error) {
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
                    self.cancellation = std::make_shared<std::atomic_bool>(false);
                    self.completed = std::make_shared<std::atomic_size_t>(0);
                    self.progress->setRange(0, static_cast<int>(self.expected));
                    self.progress->setValue(0);
                    self.status->setText(QStringLiteral("正在生成报告…"));
                    self.job = std::async(std::launch::async, [snapshot = std::move(*context), d,
                                                               cancel = self.cancellation,
                                                               count = self.completed] {
                        return runtime::generate_report(
                            snapshot, d, cancel.get(),
                            [count](std::size_t current, std::size_t) { count->store(current); });
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
                d.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                d.name += " · 副本";
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
            name->setText(text(d.name));
            if (as_copy)
                changed = true;
            fill_library();
            remove->setEnabled(true);
            result_status();
            status->setText(QStringLiteral("配置已加入项目；保存项目后写入磁盘。"));
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
            library->setCurrentRow(0);
            status->setText(QStringLiteral("已删除配置；保存项目后写入磁盘。"));
        } catch (const std::exception &error) {
            status->setText(QString::fromUtf8(error.what()));
        }
    }
    void export_file() {
        if (!model->result)
            return;
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
                    cells << text(model->result->cell_text(row.row(), c));
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
                auto result = std::make_shared<runtime::ReportResult>(job.get());
                if (cancellation->load())
                    throw runtime::ReportCancelled{};
                model->publish(result);
                changed = false;
                table->resizeColumnsToContents();
                // ID stays available via the source data/CSV; names are more useful in the viewer.
                if (result->table.columns().front().id == "object.id")
                    table->setColumnHidden(0, true);
                else
                    table->setColumnHidden(0, false);
                source->setPlainText(
                    QStringLiteral(
                        "报告：%1\n算法：%2\n\n输入 SHA-256\n%3\n\n场景数据 SHA-256\n%4\n\nUTC "
                        "数据 SHA-256\n%5\n\nCSV 包含表格数据；本页保留本次生成的溯源信息。")
                        .arg(text(result->definition.name), text(result->algorithm),
                             text(result->input_sha256), text(result->data_sha256),
                             text(result->time_sha256)));
                result_status();
                tabs->setCurrentIndex(1);
                status->setText(QStringLiteral("报告已生成。"));
            } catch (const runtime::ReportCancelled &) {
                status->setText(QStringLiteral("已取消；上次结果保留。"));
            } catch (const std::exception &error) {
                status->setText(QStringLiteral("生成失败；上次结果保留。%1")
                                    .arg(QString::fromUtf8(error.what())));
            }
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
    void update(Project next, std::shared_ptr<const core::ITimeProvider> clock) {
        try {
            draft = definition();
        } catch (const std::exception &) {
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
        fill_objects();
        result_status();
    }
};
ReportDialog::ReportDialog(Project project, std::shared_ptr<const core::ITimeProvider> time,
                           Prepare prepare, Save save, std::function<void(core::Epoch)> seek,
                           QWidget *parent)
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
    const int row = static_cast<int>(kind);
    self.library->setCurrentRow(row);
    self.choose(row);
    self.all_objects->setChecked(false);
    for (int i = 0; i < self.objects->count(); ++i) {
        auto *item = self.objects->item(i);
        item->setCheckState(item->data(Qt::UserRole).toString() == text(id) ? Qt::Checked
                                                                            : Qt::Unchecked);
    }
}
void ReportDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (impl_)
        impl_->adapt(event->size().width());
}
void ReportDialog::update_project(Project project,
                                  std::shared_ptr<const core::ITimeProvider> time) {
    impl_->update(std::move(project), std::move(time));
}
} // namespace orbitforge::desktop
