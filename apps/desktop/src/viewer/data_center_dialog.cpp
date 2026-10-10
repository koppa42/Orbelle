#include "eop_catalog.hpp"
#include "epoch_editor.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDialog>
#include <QFileDialog>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>
#include <orbitforge/application/project.hpp>

namespace orbitforge::desktop {
std::optional<std::size_t>
choose_eop_snapshot(QWidget *parent, EopCatalog &catalog, const EopCatalog::Provider &current,
                    std::optional<std::pair<core::Epoch, core::Epoch>> interval) {
    QDialog dialog(parent);
    dialog.setObjectName("eopDataDialog");
    dialog.setWindowTitle(QStringLiteral("数据中心"));
    dialog.resize(780, 360);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);
    auto *title = new QLabel(QStringLiteral("IERS 地球定向数据"));
    auto font = title->font();
    font.setPointSize(font.pointSize() + 3);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    auto *summary = new QLabel;
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto *actions = new QHBoxLayout;
    auto *download = new QPushButton;
    download->setObjectName("downloadEop");
    auto *cancel = new QPushButton(QStringLiteral("取消下载"));
    cancel->setObjectName("cancelEopDownload");
    actions->addWidget(download);
    actions->addWidget(cancel);
    actions->addStretch();
    layout->addLayout(actions);
    auto *automatic = new QCheckBox(QStringLiteral("自动更新数据"));
    automatic->setObjectName("dataAutoUpdate");
    automatic->setChecked(catalog.automatic_updates());
    layout->addWidget(automatic);
    auto *hint = new QLabel(QStringLiteral("首次下载完整数据，之后在应用运行期间每天检查更新。\n"
                                           "已有场景继续使用固定版本，可在下方详情中切换。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *status = new QLabel;
    status->setObjectName("dataCenterStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *expand = new QToolButton;
    expand->setObjectName("dataCenterDetails");
    expand->setText(QStringLiteral("版本与场景引用"));
    expand->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    expand->setCheckable(true);
    expand->setArrowType(Qt::RightArrow);
    layout->addWidget(expand);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *advanced = new QWidget;
    auto *detail_layout = new QVBoxLayout(advanced);
    detail_layout->setContentsMargins(0, 0, 8, 0);
    scroll->setWidget(advanced);
    layout->addWidget(scroll, 1);
    scroll->hide();
    auto *choices = new QComboBox;
    choices->setObjectName("eopSnapshots");
    detail_layout->addWidget(choices);
    auto *details = new QLabel;
    details->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    details->setTextFormat(Qt::PlainText);
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_layout->addWidget(details);
    auto *apply = new QPushButton(interval ? QStringLiteral("应用到当前场景")
                                           : QStringLiteral("用于新建场景"));
    apply->setObjectName("applyEopSnapshot");
    auto *set_default = new QPushButton(QStringLiteral("设为新场景默认"));
    set_default->setObjectName("setDefaultEop");
    auto *binding_actions = new QHBoxLayout;
    binding_actions->addWidget(apply);
    binding_actions->addWidget(set_default);
    binding_actions->addStretch();
    detail_layout->addLayout(binding_actions);
    auto *import = new QPushButton(QStringLiteral("导入已有快照…"));
    import->setObjectName("importEopSnapshot");
    auto *raw = new QPushButton(QStringLiteral("导入完整 finals 文件…"));
    raw->setObjectName("importRawEop");
    auto *imports = new QHBoxLayout;
    imports->addWidget(raw);
    imports->addWidget(import);
    imports->addStretch();
    detail_layout->addLayout(imports);
    const auto clock = runtime::make_builtin_time_provider();
    const auto &time = clock->data_info();
    auto *provenance = new QLabel(
        QStringLiteral("来源：https://datacenter.iers.org/data/9/finals2000A.all\n"
                       "UTC 时间数据：%1，有效至 %2（不含当日）。\n数据目录：%3")
            .arg(QString::fromStdString(time.id),
                 QDate(1858, 11, 17).addDays(time.utc_valid_until_mjd).toString(Qt::ISODate),
                 QString::fromStdWString(catalog.store().wstring())));
    provenance->setWordWrap(true);
    provenance->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_layout->addWidget(provenance);
    detail_layout->addStretch();
    auto *warnings = new QLabel(catalog.warnings().join('\n'));
    warnings->setTextFormat(Qt::PlainText);
    warnings->setWordWrap(true);
    layout->addWidget(warnings);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *close = new QPushButton(QStringLiteral("关闭"));
    buttons->addWidget(close);
    layout->addLayout(buttons);
    const auto update = [&] {
        const int index = choices->currentIndex();
        if (index < 0) {
            apply->setEnabled(false);
            set_default->setEnabled(false);
            return;
        }
        const auto &info = catalog.providers().at(static_cast<std::size_t>(index))->data_info();
        const bool covers =
            !interval || (info.first_epoch.tai_nanoseconds() <= interval->first.tai_nanoseconds() &&
                          info.last_epoch.tai_nanoseconds() >= interval->second.tai_nanoseconds());
        const auto lock = application::eop_data_lock(info);
        details->setText(
            QStringLiteral("%1%2\n可计算范围：%3 — %4 UTC\n文件 SHA-256：%5\n依赖 SHA-256：%6\n%7")
                .arg(QString::fromStdString(info.id),
                     static_cast<std::size_t>(index) == catalog.preferred
                         ? QStringLiteral(" · 新场景默认")
                         : QString{},
                     calendar_text(info.first_epoch, CalendarScale::utc),
                     calendar_text(info.last_epoch, CalendarScale::utc),
                     QString::fromStdString(info.sha256), QString::fromStdString(lock.sha256()),
                     covers ? QStringLiteral("可应用") : QStringLiteral("未覆盖当前场景完整时段")));
        apply->setEnabled(covers);
        set_default->setEnabled(true);
    };
    const auto refill = [&] {
        while (choices->count() < static_cast<int>(catalog.providers().size())) {
            const auto &info =
                catalog.providers()[static_cast<std::size_t>(choices->count())]->data_info();
            choices->addItem(QString::fromStdString(info.id));
        }
    };
    refill();
    auto initial = catalog.preferred;
    if (current)
        for (std::size_t i = 0; i < catalog.providers().size(); ++i)
            if (application::matching_project_data(catalog.providers()[i]->data_info(),
                                                   current->data_info()))
                initial = i;
    choices->setCurrentIndex(static_cast<int>(initial));
    QObject::connect(choices, &QComboBox::currentIndexChanged, &dialog, update);
    QObject::connect(expand, &QToolButton::toggled, &dialog, [&](bool visible) {
        scroll->setVisible(visible);
        expand->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
        dialog.resize(780, visible ? 720 : 360);
    });
    QObject::connect(import, &QPushButton::clicked, &dialog, [&] {
        const auto file = QFileDialog::getOpenFileName(&dialog, QStringLiteral("导入已有 EOP 快照"),
                                                       {}, QStringLiteral("快照清单 (*.json)"));
        if (file.isEmpty())
            return;
        try {
            const auto index = catalog.import_snapshot(std::filesystem::path(file.toStdWString()));
            refill();
            choices->setCurrentIndex(static_cast<int>(index));
            update();
        } catch (const std::exception &error) {
            warnings->setText(QString::fromUtf8(error.what()));
        }
    });
    QObject::connect(set_default, &QPushButton::clicked, &dialog, [&] {
        try {
            catalog.set_preferred(static_cast<std::size_t>(choices->currentIndex()));
            update();
        } catch (const std::exception &error) {
            warnings->setText(QString::fromUtf8(error.what()));
        }
    });
    QObject::connect(automatic, &QCheckBox::toggled, &dialog, [&](bool value) {
        try {
            catalog.set_automatic_updates(value);
        } catch (const std::exception &error) {
            warnings->setText(QString::fromUtf8(error.what()));
        }
    });
    QObject::connect(download, &QPushButton::clicked, &dialog, [&] { catalog.start_download(); });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, [&] { catalog.cancel_download(); });
    QObject::connect(raw, &QPushButton::clicked, &dialog, [&] {
        const auto file =
            QFileDialog::getOpenFileName(&dialog, QStringLiteral("导入完整 finals2000A 文件"));
        if (file.isEmpty())
            return;
        try {
            catalog.import_raw(std::filesystem::path(file.toStdWString()));
            refill();
        } catch (const std::exception &error) {
            warnings->setText(QString::fromUtf8(error.what()));
        }
    });
    QTimer timer;
    std::optional<std::size_t> seen = catalog.last_installed();
    const auto refresh = [&] {
        refill();
        if (catalog.last_installed() != seen) {
            seen = catalog.last_installed();
            if (seen) {
                choices->setCurrentIndex(static_cast<int>(*seen));
                update();
            }
        }
        QString coverage = QStringLiteral("尚未下载完整数据，可先使用内置版本。无需选择日期范围。");
        for (auto it = catalog.providers().rbegin(); it != catalog.providers().rend(); ++it) {
            const auto &info = (*it)->data_info();
            if (!info.id.starts_with("iers-finals2000a-all-"))
                continue;
            coverage = QStringLiteral("完整数据已下载 · 含观测值和预测值\n可计算范围：%1 — %2 UTC")
                           .arg(calendar_text(info.first_epoch, CalendarScale::utc),
                                calendar_text(info.last_epoch, CalendarScale::utc));
            break;
        }
        summary->setText(coverage);
        const bool busy = catalog.downloading();
        download->setText(catalog.has_full_download() ? QStringLiteral("检查并更新")
                                                      : QStringLiteral("下载完整数据"));
        download->setEnabled(!busy);
        raw->setEnabled(!busy);
        cancel->setVisible(busy);
        status->setText(
            catalog.status() +
            (busy ? QStringLiteral("  %1 KiB").arg(catalog.received_bytes() / 1024) : QString{}));
    };
    QObject::connect(&timer, &QTimer::timeout, &dialog, refresh);
    timer.start(150);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(apply, &QPushButton::clicked, &dialog, &QDialog::accept);
    update();
    refresh();
    if (dialog.exec() == QDialog::Accepted)
        return static_cast<std::size_t>(choices->currentIndex());
    return std::nullopt;
}
} // namespace orbitforge::desktop
