#include "eop_catalog.hpp"
#include "epoch_editor.hpp"
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <orbitforge/application/project.hpp>

namespace orbitforge::desktop {
EopCatalog::EopCatalog(std::filesystem::path store, std::vector<Provider> bundled)
    : store_(std::move(store)), providers_(std::move(bundled)) {
    try {
        if (!std::filesystem::exists(store_))
            return;
        std::vector<std::filesystem::path> entries;
        for (const auto &entry : std::filesystem::directory_iterator(store_)) {
            const auto name = entry.path().filename().string();
            if (name.size() == 64 &&
                std::all_of(
                    name.begin(), name.end(),
                    [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) &&
                entry.is_directory())
                entries.push_back(entry.path() / "manifest.json");
        }
        std::sort(entries.begin(), entries.end());
        for (const auto &path : entries) {
            try {
                append(runtime::load_eop_snapshot(path));
            } catch (const std::exception &error) {
                warnings_.push_back(QString::fromStdWString(path.wstring()) + ": " +
                                    QString::fromUtf8(error.what()));
            }
        }
    } catch (const std::exception &error) {
        warnings_.push_back(QString::fromUtf8(error.what()));
    }
}
std::size_t EopCatalog::append(Provider provider) {
    for (std::size_t i = 0; i < providers_.size(); ++i)
        if (application::matching_project_data(providers_[i]->data_info(), provider->data_info()))
            return i;
    providers_.push_back(std::move(provider));
    return providers_.size() - 1;
}
std::size_t EopCatalog::import_snapshot(const std::filesystem::path &manifest) {
    const auto saved = runtime::import_eop_snapshot(manifest, store_);
    return append(runtime::load_eop_snapshot(saved));
}
std::optional<std::size_t>
choose_eop_snapshot(QWidget *parent, EopCatalog &catalog, const EopCatalog::Provider &current,
                    std::optional<std::pair<core::Epoch, core::Epoch>> interval) {
    QDialog dialog(parent);
    dialog.setObjectName("eopDataDialog");
    dialog.setWindowTitle(QStringLiteral("地球定向数据"));
    dialog.resize(700, 480);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    auto *intro = new QLabel(
        QStringLiteral("导入本地 IERS C04 或 finals2000A 快照的 manifest.json 及同目录数据文件。\n"
                       "导入会保存独立副本；应用时校验完整场景覆盖，并更新地固坐标与轨迹。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *choices = new QComboBox;
    choices->setObjectName("eopSnapshots");
    layout->addWidget(choices);
    auto *details = new QLabel;
    details->setTextFormat(Qt::PlainText);
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(details, 1);
    auto *error = new QLabel(catalog.warnings().join('\n'));
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    error->setObjectName("inputError");
    layout->addWidget(error);
    auto *buttons = new QHBoxLayout;
    auto *import = new QPushButton(QStringLiteral("导入快照…"));
    import->setObjectName("importEopSnapshot");
    buttons->addWidget(import);
    buttons->addStretch();
    auto *cancel = new QPushButton(QStringLiteral("取消"));
    auto *apply = new QPushButton(interval ? QStringLiteral("应用到当前场景")
                                           : QStringLiteral("用于新建场景"));
    apply->setObjectName("applyEopSnapshot");
    buttons->addWidget(cancel);
    buttons->addWidget(apply);
    layout->addLayout(buttons);
    const auto update = [&] {
        const auto &info =
            catalog.providers().at(static_cast<std::size_t>(choices->currentIndex()))->data_info();
        const bool covers =
            !interval || (info.first_epoch.tai_nanoseconds() <= interval->first.tai_nanoseconds() &&
                          info.last_epoch.tai_nanoseconds() >= interval->second.tai_nanoseconds());
        details->setText(
            QStringLiteral("%1\n覆盖：%2 — %3 UTC\n参考系：%4\n插值：%5\n"
                           "SHA256：%6\n来源：%7\n时间数据：%8\n\n%9")
                .arg(QString::fromStdString(info.id) +
                         (info.interpolation.starts_with("finals2000a")
                              ? QStringLiteral("\nBulletin A · 可能含预测值，保留 I/P 标记")
                              : QStringLiteral("\nC04 · 事后解")),
                     calendar_text(info.first_epoch, CalendarScale::utc),
                     calendar_text(info.last_epoch, CalendarScale::utc),
                     QString::fromStdString(info.reference_frame),
                     QString::fromStdString(info.interpolation),
                     QString::fromStdString(info.sha256), QString::fromStdString(info.source_url),
                     QString::fromStdString(info.time_data.id),
                     covers ? QStringLiteral("可应用")
                            : QStringLiteral("未覆盖当前场景的完整时段")));
        apply->setEnabled(covers);
    };
    const auto refill = [&] {
        while (choices->count() < static_cast<int>(catalog.providers().size())) {
            const auto &info =
                catalog.providers()[static_cast<std::size_t>(choices->count())]->data_info();
            choices->addItem(QString::fromStdString(info.id + " · " + info.sha256.substr(0, 12)));
        }
    };
    refill();
    auto initial = catalog.preferred;
    for (std::size_t i = 0; i < catalog.providers().size(); ++i)
        if (catalog.providers()[i] == current)
            initial = i;
    choices->setCurrentIndex(static_cast<int>(initial));
    QObject::connect(choices, &QComboBox::currentIndexChanged, &dialog, update);
    QObject::connect(import, &QPushButton::clicked, &dialog, [&] {
        const auto file = QFileDialog::getOpenFileName(&dialog, QStringLiteral("导入 EOP 快照"), {},
                                                       QStringLiteral("快照清单 (*.json)"));
        if (file.isEmpty())
            return;
        try {
            const auto index = catalog.import_snapshot(std::filesystem::path(file.toStdWString()));
            refill();
            choices->setCurrentIndex(static_cast<int>(index));
            error->setText(QStringLiteral("快照已导入，点击应用以选择此版本。"));
            update();
        } catch (const std::exception &exception) {
            error->setText(QString::fromUtf8(exception.what()));
        }
    });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(apply, &QPushButton::clicked, &dialog, &QDialog::accept);
    update();
    if (dialog.exec() == QDialog::Accepted)
        return static_cast<std::size_t>(choices->currentIndex());
    return std::nullopt;
}
} // namespace orbitforge::desktop
