#include "eop_catalog.hpp"
#include "epoch_editor.hpp"
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <orbitforge/application/project.hpp>

namespace orbitforge::desktop {
namespace {
QString acquisition_text(const runtime::CatalogSnapshot *entry) {
    if (!entry || (entry->acquisition != runtime::DataAcquisition::bundled &&
                   !entry->acquired_at_unix_seconds))
        return QStringLiteral("未记录（本地已有数据）");
    if (entry->acquisition == runtime::DataAcquisition::bundled)
        return QStringLiteral("内置，随程序附带");
    const auto local =
        QDateTime::fromSecsSinceEpoch(*entry->acquired_at_unix_seconds, QTimeZone::UTC)
            .toLocalTime();
    return local.toString("yyyy-MM-dd HH:mm:ss") +
           (entry->acquisition == runtime::DataAcquisition::downloaded ? QStringLiteral(" · 下载")
                                                                       : QStringLiteral(" · 导入"));
}
QDateTime utc_datetime(const core::ITimeProvider &clock, core::Epoch epoch) {
    const auto t = clock.to_utc(epoch);
    return QDateTime(QDate(t.year, t.month, t.day), QTime(t.hour, t.minute, std::min(t.second, 59)),
                     QTimeZone::UTC);
}
} // namespace
void show_data_center(QWidget *parent, EopCatalog &catalog) {
    QDialog dialog(parent);
    dialog.setObjectName("eopDataDialog");
    dialog.setWindowTitle(QStringLiteral("数据中心"));
    dialog.resize(980, 570);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);
    auto *panels = new QHBoxLayout;
    panels->setSpacing(24);
    auto *list = new QListWidget;
    list->setObjectName("dataInventory");
    list->setMinimumWidth(220);
    list->setMaximumWidth(250);
    list->setMinimumHeight(280);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    panels->addWidget(list);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *panel = new QWidget;
    auto *detail_layout = new QVBoxLayout(panel);
    detail_layout->setContentsMargins(0, 0, 8, 0);
    detail_layout->setSpacing(14);
    scroll->setWidget(panel);
    panels->addWidget(scroll, 1);
    layout->addLayout(panels, 1);
    auto *title = new QLabel;
    title->setObjectName("dataName");
    auto font = title->font();
    font.setPointSize(font.pointSize() + 3);
    font.setBold(true);
    title->setFont(font);
    title->setWordWrap(true);
    detail_layout->addWidget(title);
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(24);
    form->setVerticalSpacing(12);
    const auto field = [&](const QString &caption, const char *name) {
        auto *label = new QLabel;
        label->setObjectName(name);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(caption, label);
        return label;
    };
    auto *state = field(QStringLiteral("状态"), "dataStatus");
    auto *acquired = field(QStringLiteral("获取时间（本地）"), "dataAcquiredAt");
    auto *first = field(QStringLiteral("覆盖起点（UTC）"), "dataValidFrom");
    auto *until = field(QStringLiteral("有效至（UTC）"), "dataValidUntil");
    auto *source = field(QStringLiteral("来源"), "dataSource");
    detail_layout->addLayout(form);
    auto *actions = new QHBoxLayout;
    auto *download = new QPushButton;
    download->setObjectName("downloadEop");
    auto *raw = new QPushButton(QStringLiteral("导入本地文件…"));
    raw->setObjectName("importRawEop");
    auto *manifest = new QPushButton(QStringLiteral("导入数据…"));
    manifest->setObjectName("importEopSnapshot");
    actions->addWidget(download);
    actions->addWidget(raw);
    actions->addWidget(manifest);
    actions->addStretch();
    detail_layout->addLayout(actions);
    auto *automatic = new QCheckBox(QStringLiteral("应用运行期间，每天检查更新"));
    automatic->setObjectName("dataAutoUpdate");
    automatic->setChecked(catalog.automatic_updates());
    detail_layout->addWidget(automatic);
    auto *extrapolation = new QWidget;
    auto *extrapolation_layout = new QHBoxLayout(extrapolation);
    extrapolation_layout->setContentsMargins(0, 0, 0, 0);
    auto *allow = new QCheckBox(QStringLiteral("允许有限外推，前后各最多"));
    allow->setObjectName("allowEopExtrapolation");
    auto *days = new QSpinBox;
    days->setObjectName("eopExtrapolationDays");
    days->setRange(1, core::max_eop_extrapolation_days);
    days->setValue(3);
    days->setSuffix(QStringLiteral(" 天"));
    extrapolation_layout->addWidget(allow);
    extrapolation_layout->addWidget(days);
    extrapolation_layout->addStretch();
    detail_layout->addWidget(extrapolation);
    auto *note = new QLabel;
    note->setWordWrap(true);
    detail_layout->addWidget(note);
    auto *expand = new QToolButton;
    expand->setObjectName("dataCenterDetails");
    expand->setText(QStringLiteral("校验信息与存储位置"));
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
    detail_layout->addStretch();
    auto *warnings = new QLabel(catalog.warnings().join('\n'));
    warnings->setTextFormat(Qt::PlainText);
    warnings->setWordWrap(true);
    warnings->setVisible(!warnings->text().isEmpty());
    layout->addWidget(warnings);
    auto *status = new QLabel;
    status->setObjectName("dataCenterStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *footer = new QHBoxLayout;
    auto *usage = new QLabel(QStringLiteral("打开或重算时检查数据，数据变化后需确认。"));
    usage->setWordWrap(true);
    footer->addWidget(usage, 1);
    auto *cancel = new QPushButton(QStringLiteral("取消下载"));
    cancel->setObjectName("cancelEopDownload");
    footer->addWidget(cancel);
    auto *close = new QPushButton(QStringLiteral("关闭"));
    footer->addWidget(close);
    layout->addLayout(footer);
    const auto clock = runtime::make_builtin_time_provider();
    const auto &time = clock->data_info();
    std::vector<runtime::CatalogSnapshot> entries;
    const auto selected = [&] {
        return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toInt() : -2;
    };
    const auto update_details = [&] {
        const auto index = selected();
        const bool is_time = index == -1;
        const bool finals =
            index == -2 || (index >= 0 && catalog.providers()
                                              .at(static_cast<std::size_t>(index))
                                              ->data_info()
                                              .interpolation.starts_with("finals2000a"));
        download->setVisible(finals);
        raw->setVisible(finals);
        manifest->setVisible(!finals && !is_time);
        automatic->setVisible(finals);
        extrapolation->setVisible(index >= 0);
        if (index >= 0) {
            const QSignalBlocker allow_block(allow), days_block(days);
            const auto limit = catalog.extrapolation_days(static_cast<std::size_t>(index));
            allow->setChecked(limit > 0);
            days->setValue(limit > 0 ? limit : 3);
            days->setEnabled(limit > 0);
        }
        download->setText(catalog.has_full_download() ? QStringLiteral("检查更新")
                                                      : QStringLiteral("下载完整数据"));
        const auto now = QDateTime::currentDateTimeUtc();
        if (is_time) {
            const auto expiry = QDate(1858, 11, 17).addDays(time.utc_valid_until_mjd);
            title->setText(QStringLiteral("UTC / 闰秒"));
            acquired->setText(QStringLiteral("内置，随程序附带"));
            first->setText(
                QDate(1858, 11, 17).addDays(time.utc_valid_from_mjd).toString(Qt::ISODate));
            until->setText(expiry.toString(Qt::ISODate) + QStringLiteral("（不含当日）"));
            state->setText(now.date() < expiry ? QStringLiteral("可用") : QStringLiteral("已到期"));
            source->setText(QString::fromStdString(time.source_url));
            note->setText(QStringLiteral("当前随程序更新。不支持超出有效期使用。"));
            technical->setText(
                QStringLiteral("%1\nSHA-256：%2")
                    .arg(QString::fromStdString(time.id), QString::fromStdString(time.sha256)));
            return;
        }
        title->setText(finals ? QStringLiteral("地球定向 · finals2000A")
                              : QStringLiteral("地球定向 · C04"));
        if (index < 0) {
            acquired->setText(QStringLiteral("—"));
            first->setText(QStringLiteral("—"));
            until->setText(QStringLiteral("—"));
            state->setText(QStringLiteral("未下载"));
            source->setText(QStringLiteral("IERS"));
            note->setText(QStringLiteral("下载完整 finals2000A 文件后可用于场景计算。"));
            technical->setText(QStringLiteral("尚无本地文件。"));
            return;
        }
        const auto &info = catalog.providers().at(static_cast<std::size_t>(index))->data_info();
        const auto key = application::eop_data_lock(info).sha256();
        const auto entry = std::find_if(entries.begin(), entries.end(),
                                        [&](const auto &e) { return e.lock_sha256 == key; });
        acquired->setText(acquisition_text(entry == entries.end() ? nullptr : &*entry));
        first->setText(calendar_text(info.first_epoch, CalendarScale::utc));
        until->setText(calendar_text(info.last_epoch, CalendarScale::utc));
        state->setText(now < utc_datetime(*clock, info.first_epoch)  ? QStringLiteral("尚未生效")
                       : now > utc_datetime(*clock, info.last_epoch) ? QStringLiteral("覆盖已结束")
                                                                     : QStringLiteral("可用"));
        source->setText(QString::fromStdString(info.source_url));
        note->setText(
            allow->isChecked()
                ? QStringLiteral("外推采用端点线性估算，精度随距离降低；计算会标注实际外推时段。上"
                                 "方有效期仍是原文件覆盖，不含外推。UTC / 闰秒有效期不可延长。")
                : QStringLiteral("覆盖已结束的数据仍可用于对应的历史时段。外推默认关闭；启用后按端"
                                 "点变化率线性估算，天数从原文件边界起算。"));
        technical->setText(QStringLiteral("%1\n文件 SHA-256：%2\n数据身份 SHA-256：%3\n目录：%4")
                               .arg(QString::fromStdString(info.id),
                                    QString::fromStdString(info.sha256),
                                    QString::fromStdString(key),
                                    QString::fromStdWString(catalog.store().wstring())));
    };
    const auto show_error = [&](const std::exception &error) {
        warnings->setText(QString::fromUtf8(error.what()));
        warnings->show();
    };
    const auto select_provider = [&](int index) {
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->data(Qt::UserRole).toInt() == index)
                list->setCurrentRow(row);
    };
    const auto rebuild = [&] {
        const auto previous = selected();
        const auto product =
            previous >= 0
                ? catalog.providers()[static_cast<std::size_t>(previous)]->data_info().interpolation
                : std::string{};
        list->blockSignals(true);
        list->clear();
        entries = catalog.data().snapshots();
        const auto add = [&](const QString &name, int index) {
            auto *item = new QListWidgetItem(name, list);
            item->setData(Qt::UserRole, index);
            item->setSizeHint(QSize(0, 68));
        };
        bool has_finals = false;
        for (const auto index : catalog.current_indices()) {
            const auto &info = catalog.providers()[index]->data_info();
            const bool finals = info.interpolation.starts_with("finals2000a");
            has_finals |= finals;
            const auto now = QDateTime::currentDateTimeUtc();
            const auto state_text = now > utc_datetime(*clock, info.last_epoch)
                                        ? QStringLiteral("历史覆盖")
                                        : QStringLiteral("已安装");
            add((finals ? QStringLiteral("finals2000A") : QStringLiteral("C04")) + "\n" +
                    state_text,
                static_cast<int>(index));
        }
        if (!has_finals)
            add(QStringLiteral("finals2000A\n未下载"), -2);
        add(QStringLiteral("UTC / 闰秒\n随程序附带"), -1);
        list->setCurrentRow(0);
        for (int row = 0; row < list->count(); ++row) {
            const auto index = list->item(row)->data(Qt::UserRole).toInt();
            if (index == previous ||
                (index >= 0 &&
                 catalog.providers()[static_cast<std::size_t>(index)]->data_info().interpolation ==
                     product))
                list->setCurrentRow(row);
        }
        list->blockSignals(false);
        update_details();
    };
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, update_details);
    QObject::connect(expand, &QToolButton::toggled, &dialog, [&](bool visible) {
        technical->setVisible(visible);
        expand->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
    });
    QObject::connect(download, &QPushButton::clicked, &dialog, [&] { catalog.start_download(); });
    QObject::connect(manifest, &QPushButton::clicked, &dialog, [&] {
        const auto file = QFileDialog::getOpenFileName(&dialog, QStringLiteral("导入 EOP 数据"), {},
                                                       QStringLiteral("数据清单 (*.json)"));
        if (file.isEmpty())
            return;
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
    });
    const auto save_extrapolation = [&] {
        if (selected() < 0)
            return;
        try {
            catalog.set_extrapolation_days(static_cast<std::size_t>(selected()),
                                           allow->isChecked() ? days->value() : 0);
            update_details();
        } catch (const std::exception &error) {
            show_error(error);
        }
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
        const bool busy = catalog.downloading();
        download->setEnabled(!busy);
        raw->setEnabled(!busy);
        manifest->setEnabled(!busy);
        cancel->setVisible(busy);
        status->setText(
            catalog.status() +
            (busy ? QStringLiteral("  %1 KiB").arg(catalog.received_bytes() / 1024) : QString{}));
        status->setVisible(!status->text().isEmpty());
    };
    QObject::connect(&timer, &QTimer::timeout, &dialog, refresh);
    timer.start(150);
    refresh();
    dialog.exec();
}
} // namespace orbitforge::desktop
