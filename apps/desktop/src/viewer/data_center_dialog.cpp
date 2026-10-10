#include "eop_catalog.hpp"
#include "workbench_spin_box.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
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
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace orbitforge::desktop {
namespace {
QString installation_text(const DataInventoryItem &item) {
    switch (item.installation) {
    case DataInstallation::missing:
        return item.online_updates ? QStringLiteral("未下载") : QStringLiteral("未导入");
    case DataInstallation::bundled_sample:
        return QStringLiteral("内置示例");
    case DataInstallation::local_file:
        return QStringLiteral("本地文件");
    case DataInstallation::downloaded:
        return QStringLiteral("完整包已下载");
    case DataInstallation::builtin:
        return QStringLiteral("程序内置");
    }
    return {};
}
QString check_text(DataUpdateState state) {
    switch (state) {
    case DataUpdateState::never_checked:
        return QStringLiteral("尚未检查");
    case DataUpdateState::checking:
        return QStringLiteral("正在检查");
    case DataUpdateState::downloading:
        return QStringLiteral("正在下载");
    case DataUpdateState::updated:
        return QStringLiteral("检查成功 · 已下载更新");
    case DataUpdateState::unchanged:
        return QStringLiteral("检查成功 · 无新内容");
    case DataUpdateState::failed:
        return QStringLiteral("检查失败");
    case DataUpdateState::cancelled:
        return QStringLiteral("已取消");
    case DataUpdateState::interrupted:
        return QStringLiteral("上次下载未完成");
    case DataUpdateState::local_changed:
        return QStringLiteral("本地文件已更换 · 待检查");
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
class DataItemDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const auto bounds = option.rect.adjusted(2, 3, -2, -3);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        painter->setPen(option.state & QStyle::State_HasFocus ? QColor("#a3afff")
                                                              : Qt::transparent);
        painter->setBrush(QColor(selected ? "#303b5d" : hover ? "#252f45" : "#171e2c"));
        painter->drawRoundedRect(bounds, 7, 7);
        const auto icon = index.data(Qt::UserRole).toInt() == static_cast<int>(DataProduct::utc)
                              ? WorkbenchIcon::clock
                              : WorkbenchIcon::earth;
        workbench_icon(icon, QColor(selected ? "#c0c9ff" : "#8e9ebb"))
            .paint(painter, QRect(bounds.left() + 12, bounds.top() + 13, 20, 20));
        const auto lines = index.data().toString().split('\n');
        auto title_font = option.font;
        title_font.setPixelSize(13);
        title_font.setWeight(QFont::DemiBold);
        painter->setFont(title_font);
        painter->setPen(QColor(selected ? "#eff2ff" : "#d0d9eb"));
        painter->drawText(bounds.adjusted(43, 9, -10, -52), Qt::AlignVCenter, lines.value(0));
        title_font.setPixelSize(11);
        title_font.setWeight(QFont::Normal);
        painter->setFont(title_font);
        painter->setPen(QColor(selected ? "#b4c2e6" : "#93a2bd"));
        painter->drawText(bounds.adjusted(43, 33, -10, -29), Qt::AlignVCenter, lines.value(1));
        painter->setPen(QColor(index.data(Qt::UserRole + 1).toBool() ? "#e6c68b" : "#98a8c3"));
        painter->drawText(bounds.adjusted(43, 54, -10, -8), Qt::AlignVCenter, lines.value(2));
        painter->restore();
    }
};
} // namespace
void show_data_center(QWidget *parent, EopCatalog &catalog) {
    QDialog dialog(parent);
    dialog.setObjectName("eopDataDialog");
    dialog.setWindowTitle(QStringLiteral("数据中心"));
    dialog.setWindowIcon(workbench_icon(WorkbenchIcon::database));
    dialog.resize(
        QSize(1020, 720).boundedTo(dialog.screen()->availableGeometry().size() - QSize(48, 72)));
    dialog.setSizeGripEnabled(true);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *heading = new QVBoxLayout;
    heading->setContentsMargins(24, 20, 24, 18);
    heading->setSpacing(6);
    auto *heading_title = new QLabel(QStringLiteral("数据中心"));
    heading_title->setObjectName("panelTitle");
    heading->addWidget(heading_title);
    auto *description =
        new QLabel(QStringLiteral("管理计算数据，查看下载时间、覆盖范围与更新结果。"));
    description->setObjectName("dialogDescription");
    heading->addWidget(description);
    layout->addLayout(heading);
    auto *panels = new QHBoxLayout;
    panels->setSpacing(0);
    auto *sidebar = new QWidget;
    sidebar->setObjectName("dataSidebar");
    sidebar->setFixedWidth(258);
    auto *sidebar_layout = new QVBoxLayout(sidebar);
    sidebar_layout->setContentsMargins(12, 18, 12, 18);
    sidebar_layout->setSpacing(12);
    auto *inventory_caption = new QLabel(QStringLiteral("数据列表"));
    inventory_caption->setObjectName("dataSidebarCaption");
    sidebar_layout->addWidget(inventory_caption);
    auto *list = new QListWidget;
    list->setObjectName("dataInventory");
    list->setAccessibleName(QStringLiteral("数据及安装状态"));
    list->setItemDelegate(new DataItemDelegate(list));
    list->setMouseTracking(true);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setMinimumHeight(280);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    sidebar_layout->addWidget(list, 1);
    auto *inventory_note = new QLabel(QStringLiteral("所有场景共用这些数据"));
    inventory_note->setObjectName("dataSidebarCaption");
    sidebar_layout->addWidget(inventory_note);
    panels->addWidget(sidebar);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *panel = new QWidget;
    auto *detail_layout = new QVBoxLayout(panel);
    detail_layout->setContentsMargins(28, 20, 28, 20);
    detail_layout->setSpacing(10);
    scroll->setWidget(panel);
    panels->addWidget(scroll, 1);
    layout->addLayout(panels, 1);
    auto *title = new QLabel;
    title->setObjectName("dataName");
    title->setWordWrap(true);
    auto *data_heading = new QHBoxLayout;
    data_heading->addWidget(title, 1);
    auto *state = new QLabel;
    state->setObjectName("dataStatus");
    data_heading->addWidget(state, 0, Qt::AlignVCenter);
    detail_layout->addLayout(data_heading);
    auto *data_description = new QLabel;
    data_description->setObjectName("dataDescription");
    data_description->setWordWrap(true);
    detail_layout->addWidget(data_description);
    auto *facts = new QFrame;
    facts->setObjectName("dataFacts");
    auto *form = new QFormLayout(facts);
    form->setContentsMargins(18, 14, 18, 14);
    form->setHorizontalSpacing(20);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
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
    auto *acquired = field(QStringLiteral("获取时间（本地）"), "dataAcquiredAt");
    auto *first = field(QStringLiteral("覆盖起点（UTC）"), "dataValidFrom");
    auto *until = field(QStringLiteral("覆盖截止（UTC）"), "dataValidUntil");
    auto *checked = field(QStringLiteral("最近检查（本地）"), "dataLastChecked");
    detail_layout->addWidget(facts);
    auto *acquisition = new QWidget;
    acquisition->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *acquisition_layout = new QVBoxLayout(acquisition);
    acquisition_layout->setContentsMargins(0, 0, 0, 0);
    acquisition_layout->setSpacing(8);
    auto *acquisition_title = new QLabel(QStringLiteral("获取与更新"));
    acquisition_title->setObjectName("sectionTitle");
    acquisition_layout->addWidget(acquisition_title);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(10);
    auto *download = new QPushButton;
    download->setObjectName("downloadEop");
    download->setProperty("primary", true);
    download->setIcon(workbench_icon(WorkbenchIcon::download, Qt::white));
    auto *raw = new QPushButton(QStringLiteral("导入本地文件…"));
    raw->setObjectName("importRawEop");
    raw->setIcon(workbench_icon(WorkbenchIcon::folder));
    auto *manifest = new QPushButton(QStringLiteral("导入数据…"));
    manifest->setObjectName("importEopSnapshot");
    actions->addWidget(download);
    actions->addWidget(raw);
    actions->addWidget(manifest);
    actions->addStretch();
    acquisition_layout->addLayout(actions);
    auto *automatic_row = new QWidget;
    auto *automatic_layout = new QHBoxLayout(automatic_row);
    automatic_layout->setContentsMargins(0, 0, 0, 0);
    auto *automatic = new QCheckBox(QStringLiteral("自动下载与更新"));
    automatic->setObjectName("dataAutoUpdate");
    automatic->setChecked(catalog.automatic_updates());
    auto *frequency = new QComboBox;
    frequency->setObjectName("dataUpdateFrequency");
    frequency->setAccessibleName(QStringLiteral("自动检查频率"));
    frequency->addItem(QStringLiteral("每 6 小时"), 6);
    frequency->addItem(QStringLiteral("每 24 小时"), 24);
    frequency->addItem(QStringLiteral("每 7 天"), 168);
    automatic_layout->addWidget(automatic);
    automatic_layout->addSpacing(8);
    automatic_layout->addWidget(frequency);
    automatic_layout->addStretch();
    acquisition_layout->addWidget(automatic_row);
    auto *schedule = new QLabel;
    schedule->setObjectName("dataUpdateSchedule");
    schedule->setWordWrap(true);
    acquisition_layout->addWidget(schedule);
    auto *progress = new QProgressBar;
    progress->setObjectName("dataDownloadProgress");
    progress->setTextVisible(false);
    progress->setFixedHeight(5);
    acquisition_layout->addWidget(progress);
    detail_layout->addWidget(acquisition);
    auto *policy_title = new QLabel(QStringLiteral("覆盖范围之外"));
    policy_title->setObjectName("sectionTitle");
    detail_layout->addWidget(policy_title);
    auto *extrapolation = new QWidget;
    auto *extrapolation_layout = new QHBoxLayout(extrapolation);
    extrapolation_layout->setContentsMargins(0, 0, 0, 0);
    auto *allow = new QCheckBox(QStringLiteral("允许有限外推"));
    allow->setObjectName("allowEopExtrapolation");
    auto *days = new WorkbenchSpinBox<QSpinBox>;
    days->setObjectName("eopExtrapolationDays");
    days->setAccessibleName(QStringLiteral("EOP 前后各允许外推的天数"));
    days->setProperty("stepButtons", true);
    days->setMinimumWidth(90);
    days->setRange(1, core::max_eop_extrapolation_days);
    days->setValue(3);
    days->setSuffix(QStringLiteral(" 天"));
    extrapolation_layout->addWidget(allow);
    extrapolation_layout->addSpacing(12);
    auto *limit_caption = new QLabel(QStringLiteral("前后各最多"));
    limit_caption->setObjectName("hint");
    extrapolation_layout->addWidget(limit_caption);
    extrapolation_layout->addWidget(days);
    extrapolation_layout->addStretch();
    detail_layout->addWidget(extrapolation);
    auto *note = new QLabel;
    note->setObjectName("dataNote");
    note->setWordWrap(true);
    detail_layout->insertWidget(3, note);
    auto *expand = new QToolButton;
    expand->setObjectName("dataCenterDetails");
    expand->setText(QStringLiteral("来源、文件与校验信息"));
    expand->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    expand->setCheckable(true);
    expand->setArrowType(Qt::RightArrow);
    detail_layout->addWidget(expand, 0, Qt::AlignLeft);
    auto *technical = new QLabel;
    technical->setObjectName("dataInventoryDetails");
    technical->setTextFormat(Qt::PlainText);
    technical->setWordWrap(true);
    technical->setTextInteractionFlags(Qt::TextSelectableByMouse);
    technical->hide();
    detail_layout->addWidget(technical);
    detail_layout->addStretch(1);
    auto *warnings = new QLabel(catalog.warnings().join('\n'));
    warnings->setObjectName("inputError");
    warnings->setContentsMargins(24, 8, 24, 8);
    warnings->setTextFormat(Qt::PlainText);
    warnings->setWordWrap(true);
    warnings->setVisible(!warnings->text().isEmpty());
    layout->addWidget(warnings);
    auto *status = new QLabel;
    status->setObjectName("dataCenterStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    acquisition_layout->addWidget(status);
    auto *footer_panel = new QWidget;
    footer_panel->setObjectName("dataFooter");
    auto *footer = new QHBoxLayout(footer_panel);
    footer->setContentsMargins(24, 12, 24, 12);
    auto *usage = new QLabel(QStringLiteral("打开或重算时检查数据，数据变化后需确认。"));
    usage->setWordWrap(true);
    usage->setObjectName("hint");
    footer->addWidget(usage, 1);
    auto *cancel = new QPushButton(QStringLiteral("取消下载"));
    cancel->setObjectName("cancelEopDownload");
    actions->insertWidget(3, cancel);
    auto *close = new QPushButton(QStringLiteral("关闭"));
    close->setMinimumWidth(88);
    footer->addWidget(close);
    layout->addWidget(footer_panel);
    std::vector<DataInventoryItem> entries;
    QString operation_error;
    const auto selected = [&] {
        return list->currentItem()
                   ? static_cast<DataProduct>(list->currentItem()->data(Qt::UserRole).toInt())
                   : DataProduct::finals2000a;
    };
    const auto selected_item = [&]() -> const DataInventoryItem & {
        return *std::find_if(entries.begin(), entries.end(),
                             [&](const auto &entry) { return entry.product == selected(); });
    };
    const auto update_details = [&] {
        const auto &item = selected_item();
        const bool is_time = item.product == DataProduct::utc;
        acquisition->setVisible(!is_time);
        policy_title->setVisible(item.extrapolation);
        extrapolation->setVisible(item.extrapolation);
        download->setVisible(item.online_updates);
        raw->setVisible(item.raw_import);
        manifest->setVisible(item.manifest_import && !item.raw_import);
        automatic_row->setVisible(item.online_updates);
        schedule->setVisible(item.online_updates);
        const QSignalBlocker auto_block(automatic), frequency_block(frequency), allow_block(allow),
            days_block(days);
        automatic->setChecked(catalog.automatic_updates());
        const auto hours = catalog.update_interval_hours();
        if (frequency->findData(hours) < 0)
            frequency->addItem(QStringLiteral("每 %1 小时").arg(hours), hours);
        frequency->setCurrentIndex(frequency->findData(hours));
        frequency->setEnabled(automatic->isChecked());
        if (item.provider_index) {
            const auto limit = catalog.extrapolation_days(*item.provider_index);
            allow->setChecked(limit > 0);
            days->setValue(limit > 0 ? limit : 3);
            days->setEnabled(limit > 0);
            limit_caption->setEnabled(limit > 0);
        }
        title->setText(is_time ? QStringLiteral("UTC / 闰秒") : QStringLiteral("地球定向参数"));
        data_description->setText(
            is_time ? QStringLiteral("UTC、TAI 与 TT 之间的时间转换")
            : item.product == DataProduct::finals2000a
                ? QStringLiteral("finals2000A · 地球自转与极移参数，包含观测和预测")
                : QStringLiteral("C04 · 地球定向参数，通过本地文件导入"));
        state->setText(installation_text(item));
        state->setProperty("tone", item.installation == DataInstallation::bundled_sample ||
                                           item.installation == DataInstallation::missing
                                       ? "warning"
                                       : "ready");
        state->style()->unpolish(state);
        state->style()->polish(state);
        acquired->setText(item.acquired_at
                              ? local_time(*item.acquired_at) +
                                    (item.acquisition == runtime::DataAcquisition::downloaded
                                         ? QStringLiteral(" · 下载")
                                         : QStringLiteral(" · 导入"))
                          : item.acquisition == runtime::DataAcquisition::bundled
                              ? QStringLiteral("内置，随程序附带")
                          : item.installation == DataInstallation::missing
                              ? QStringLiteral("尚未下载或导入")
                              : QStringLiteral("未记录（已有本地文件）"));
        first->setText(coverage_time(item.first));
        until->setText(coverage_time(item.last) +
                       (item.exclusive_end ? QStringLiteral("（不含当日）") : QString{}));
        qobject_cast<QLabel *>(form->labelForField(until))
            ->setText(is_time ? QStringLiteral("有效截止（UTC）")
                              : QStringLiteral("覆盖截止（UTC）"));
        if (is_time)
            note->setText(
                QDateTime::currentDateTimeUtc() >= item.last
                    ? QStringLiteral("已超过有效截止，需要更新程序内的 UTC 数据后再计算后续时段。")
                    : QStringLiteral("随程序更新。有效截止之后的时段需要新的 UTC 数据。"));
        else if (item.installation == DataInstallation::bundled_sample)
            note->setText(
                item.online_updates
                    ? QStringLiteral("当前只有内置示例片段，完整包尚未下载。")
                    : QStringLiteral("当前只有内置示例片段，可导入覆盖所需时段的 C04 数据。"));
        else if (item.installation == DataInstallation::missing)
            note->setText(item.online_updates
                              ? QStringLiteral("下载完整文件后，可查看实际计算覆盖范围。")
                              : QStringLiteral("导入本地 C04 数据后，可查看计算覆盖范围。"));
        else
            note->setText(
                QStringLiteral("覆盖截止后仍可用于对应的历史场景。上方时段不含外推。") +
                (item.online_updates ? QStringLiteral("是否有更新以联网检查为准。") : QString{}));
        if (!is_time && item.extrapolation && allow->isChecked())
            note->setText(note->text() +
                          QStringLiteral(" 已启用端点线性外推，精度随距离降低；UTC 有效期不变。"));
        const auto location = is_time             ? QStringLiteral("内嵌在程序中")
                              : item.file.empty() ? QStringLiteral("尚无本地文件")
                                                  : QString::fromStdWString(item.file.wstring());
        technical->setText(
            QStringLiteral("来源：%1\n文件：%2\n大小：%3\nSHA-256：%4%5")
                .arg(item.source_url.isEmpty() ? QStringLiteral("本地导入") : item.source_url,
                     location, item.file_size ? bytes_text(*item.file_size) : QStringLiteral("—"),
                     item.sha256.isEmpty() ? QStringLiteral("—") : item.sha256,
                     item.identity_sha256.isEmpty()
                         ? QString{}
                         : QStringLiteral("\n数据身份：") + item.identity_sha256));
    };
    const auto show_error = [&](const std::exception &error) {
        operation_error = QString::fromUtf8(error.what());
    };
    const auto select_provider = [&](int index) {
        for (std::size_t row = 0; row < entries.size(); ++row)
            if (entries[row].provider_index == static_cast<std::size_t>(index))
                list->setCurrentRow(static_cast<int>(row));
    };
    const auto rebuild = [&] {
        const auto previous = selected();
        entries = catalog.inventory();
        const QSignalBlocker blocker(list);
        list->clear();
        for (const auto &entry : entries) {
            auto *item = new QListWidgetItem(list);
            item->setData(Qt::UserRole, static_cast<int>(entry.product));
            item->setSizeHint(QSize(0, 88));
            if (entry.product == previous)
                list->setCurrentItem(item);
        }
        update_details();
    };
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, [&] {
        operation_error.clear();
        update_details();
    });
    QObject::connect(expand, &QToolButton::toggled, &dialog, [&](bool visible) {
        technical->setVisible(visible);
        expand->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
    });
    QObject::connect(download, &QPushButton::clicked, &dialog, [&] {
        operation_error.clear();
        catalog.start_download();
    });
    QObject::connect(manifest, &QPushButton::clicked, &dialog, [&] {
        const auto file = QFileDialog::getOpenFileName(&dialog, QStringLiteral("导入 EOP 数据"), {},
                                                       QStringLiteral("数据清单 (*.json)"));
        if (file.isEmpty())
            return;
        operation_error.clear();
        try {
            const auto index = catalog.import_snapshot(std::filesystem::path(file.toStdWString()));
            rebuild();
            select_provider(static_cast<int>(index));
        } catch (const std::exception &error) {
            show_error(error);
        }
    });
    QObject::connect(raw, &QPushButton::clicked, &dialog, [&] {
        const auto file =
            QFileDialog::getOpenFileName(&dialog, QStringLiteral("导入完整 finals2000A 文件"));
        if (file.isEmpty())
            return;
        operation_error.clear();
        try {
            catalog.import_raw(std::filesystem::path(file.toStdWString()));
            rebuild();
            if (catalog.last_installed())
                select_provider(static_cast<int>(*catalog.last_installed()));
        } catch (const std::exception &error) {
            show_error(error);
        }
    });
    QObject::connect(automatic, &QCheckBox::toggled, &dialog, [&](bool value) {
        try {
            catalog.set_automatic_updates(value);
        } catch (const std::exception &error) {
            show_error(error);
        }
        update_details();
    });
    QObject::connect(frequency, &QComboBox::currentIndexChanged, &dialog, [&] {
        try {
            catalog.set_update_interval_hours(frequency->currentData().toInt());
        } catch (const std::exception &error) {
            show_error(error);
        }
        update_details();
    });
    const auto save_extrapolation = [&] {
        if (!selected_item().provider_index)
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
        const auto &item = selected_item();
        const auto update = catalog.update_status();
        const bool busy = catalog.downloading();
        for (int row = 0; row < list->count(); ++row) {
            const auto &entry = entries[static_cast<std::size_t>(row)];
            const bool failed =
                entry.online_updates && (update.state == DataUpdateState::failed ||
                                         update.state == DataUpdateState::interrupted);
            const auto coverage = entry.last.isValid()
                                      ? (entry.exclusive_end ? QStringLiteral("有效至 ")
                                                             : QStringLiteral("覆盖至 ")) +
                                            entry.last.date().toString(Qt::ISODate)
                                      : QStringLiteral("尚无覆盖数据");
            const auto summary = failed                         ? check_text(update.state)
                                 : entry.online_updates && busy ? check_text(update.state)
                                                                : coverage;
            const auto name = entry.product == DataProduct::utc ? QStringLiteral("UTC / 闰秒")
                                                                : QStringLiteral("地球定向参数");
            const auto product = entry.product == DataProduct::finals2000a
                                     ? QStringLiteral("finals2000A · ")
                                 : entry.product == DataProduct::c04 ? QStringLiteral("C04 · ")
                                                                     : QString{};
            list->item(row)->setText(name + "\n" + product + installation_text(entry) + "\n" +
                                     summary);
            list->item(row)->setData(Qt::UserRole + 1, failed);
        }
        checked->setText(
            item.online_updates
                ? (update.attempted_at ? local_time(*update.attempted_at) + " · " : QString{}) +
                      check_text(update.state)
            : item.product == DataProduct::utc ? QStringLiteral("随程序更新")
                                               : QStringLiteral("本地导入，不联网检查"));
        download->setText(busy ? check_text(update.state)
                          : update.state == DataUpdateState::failed ||
                                  update.state == DataUpdateState::interrupted
                              ? QStringLiteral("重试下载 / 更新")
                          : item.installation == DataInstallation::missing ||
                                  item.installation == DataInstallation::bundled_sample
                              ? QStringLiteral("下载完整数据")
                              : QStringLiteral("检查并更新"));
        download->setEnabled(!busy);
        raw->setEnabled(!busy);
        manifest->setEnabled(!busy);
        cancel->setVisible(item.online_updates && busy);
        progress->setVisible(item.online_updates && busy);
        if (busy && catalog.total_bytes() > 0) {
            progress->setRange(0, 1000);
            progress->setValue(static_cast<int>(
                std::min<qint64>(1000, catalog.received_bytes() * 1000 / catalog.total_bytes())));
        } else {
            progress->setRange(0, 0);
        }
        schedule->setText(automatic->isChecked()
                              ? QStringLiteral("在应用运行时检查；尚未安装完整文件时会自动下载。")
                              : QStringLiteral("自动更新已关闭，可随时手动下载或检查。"));
        QString message = operation_error;
        if (message.isEmpty() && item.online_updates) {
            if (busy) {
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
                    message += QStringLiteral(" 上次成功检查：") + local_time(*update.checked_at);
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
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, refresh);
    QObject::connect(download, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(raw, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(manifest, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(&timer, &QTimer::timeout, &dialog, refresh);
    timer.start(150);
    refresh();
    dialog.exec();
}
} // namespace orbitforge::desktop
