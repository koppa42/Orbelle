#include "initial_data_setup.hpp"
#include "../ui_text.hpp"
#include "eop_catalog.hpp"
#include "responsive_layout.hpp"
#include "workbench_theme.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <orbelle/runtime/ephemeris.hpp>

namespace orbelle::desktop {
std::vector<DataProduct> initial_data_products() {
    // UTC must be ready before interpreting either EOP product.
    std::vector products{DataProduct::utc, DataProduct::c04, DataProduct::finals2000a,
                         DataProduct::egm2008};
    if (runtime::ephemeris_available())
        products.push_back(DataProduct::de440s);
    return products;
}
bool initial_data_ready(const EopCatalog &catalog, DataProduct product, QDate utc_day) {
    if (product != DataProduct::utc)
        return catalog.has_full_download(product);
    const auto time = catalog.time_provider();
    if (!time)
        return false;
    if (!utc_day.isValid())
        utc_day = QDateTime::currentDateTimeUtc().date();
    try {
        for (const auto day : {utc_day, utc_day.addDays(1)})
            (void)time->from_utc({day.year(), day.month(), day.day(), 0, 0, 0, 0});
        return true;
    } catch (const core::TimeDataRangeError &) {
        return false;
    }
}
namespace {
QString product_title(DataProduct product) {
    switch (product) {
    case DataProduct::utc:
        return ui::text("initial_data_setup.label");
    case DataProduct::c04:
        return ui::text("initial_data_setup.label_2");
    case DataProduct::finals2000a:
        return ui::text("initial_data_setup.label_3");
    case DataProduct::egm2008:
        return ui::text("initial_data_setup.label_4");
    case DataProduct::de440s:
        return ui::text("initial_data_setup.label_5");
    case DataProduct::egm96:
        return ui::text("initial_data_setup.label_6");
    case DataProduct::custom_gravity:
        return ui::text("initial_data_setup.label_7");
    }
    return {};
}
} // namespace
bool prepare_initial_data(QWidget *parent, EopCatalog &catalog,
                          const std::vector<DataProduct> &required,
                          const std::function<void(DataProduct)> &download,
                          const std::function<void()> &visible_ready, QDate utc_day) {
    if (!utc_day.isValid())
        utc_day = QDateTime::currentDateTimeUtc().date();
    const auto ready = [&](auto product) { return initial_data_ready(catalog, product, utc_day); };
    const auto complete = [&] { return std::all_of(required.begin(), required.end(), ready); };
    if (complete())
        return true;
    // The workbench already owns native Vulkan children but is not shown until
    // setup succeeds. Windows can hide dialogs owned by that hidden HWND even
    // while Qt reports isVisible(). Give first-launch setup its own taskbar window.
    QDialog dialog(parent && parent->isVisible() ? parent : nullptr);
    if (parent)
        dialog.setScreen(parent->screen());
    dialog.setObjectName("initialDataSetupDialog");
    dialog.setWindowTitle(ui::text("initial_data_setup.label_8"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(16);
    auto *heading = new QLabel(ui::text("initial_data_setup.heading"));
    heading->setObjectName("panelTitle");
    layout->addWidget(heading);
    auto *description = new QLabel(ui::text("initial_data_setup.description"));
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget;
    auto *form = new QFormLayout(content);
    form->setContentsMargins(0, 8, 12, 8);
    form->setVerticalSpacing(12);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    std::vector<QLabel *> states;
    for (auto product : required) {
        auto *state = new QLabel;
        state->setObjectName(QStringLiteral("initialDataState%1").arg(static_cast<int>(product)));
        states.push_back(state);
        form->addRow(product_title(product), state);
    }
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    auto *status = new QLabel;
    status->setObjectName("initialDataStatus");
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(status);
    auto *progress = new QProgressBar;
    progress->setObjectName("initialDataProgress");
    layout->addWidget(progress);
    auto *actions = new QHBoxLayout;
    auto *retry = new QPushButton(ui::text("initial_data_setup.retry"));
    retry->setObjectName("initialDataRetry");
    retry->setProperty("primary", true);
    auto *manage = new QPushButton(ui::text("initial_data_setup.manage"));
    manage->setObjectName("initialDataImport");
    auto *copy = new QPushButton(ui::text("initial_data_setup.copy_diagnostics"));
    copy->setObjectName("initialDataCopyDiagnostics");
    auto *cancel = new QPushButton(ui::text("initial_data_setup.cancel"));
    cancel->setObjectName("initialDataCancel");
    actions->addWidget(retry);
    actions->addWidget(manage);
    actions->addWidget(copy);
    actions->addStretch();
    actions->addWidget(cancel);
    layout->addLayout(actions);
    responsive_columns(&dialog, actions, 460);
    bool paused = false;
    std::optional<DataProduct> owned;
    const auto refresh = [&] {
        for (std::size_t i = 0; i < required.size(); ++i) {
            const auto product = required[i];
            const bool running = catalog.downloading() && catalog.active_product() == product;
            const auto state = catalog.update_status(product).state;
            if (ready(product) && product == DataProduct::utc &&
                !catalog.has_full_download(product)) {
                const auto until =
                    QDate(1858, 11, 17)
                        .addDays(catalog.time_provider()->data_info().utc_valid_until_mjd);
                states[i]->setText(
                    ui::text("initial_data_setup.builtin_state").arg(until.toString(Qt::ISODate)));
                states[i]->setWordWrap(true);
            } else {
                states[i]->setText(ready(product) ? ui::text("initial_data_setup.state")
                                   : running      ? ui::text("initial_data_setup.label_9")
                                   : state == DataUpdateState::failed
                                       ? ui::text("initial_data_setup.label_10")
                                       : ui::text("initial_data_setup.label_11"));
            }
        }
        retry->setVisible(paused);
        manage->setEnabled(!catalog.downloading());
        copy->setVisible(!catalog.update_status(catalog.active_product()).diagnostics.isEmpty());
        progress->setVisible(catalog.downloading());
        if (catalog.downloading()) {
            const auto received = catalog.received_bytes(), total = catalog.total_bytes();
            if (total > 0 && received < total) {
                progress->setRange(0, 1000);
                progress->setValue(static_cast<int>(1000.0 * received / total));
            } else {
                progress->setRange(0, 0);
            }
            status->setText(
                ui::text("initial_data_setup.status.set_text")
                    .arg(product_title(catalog.active_product()), catalog.status())
                    .arg(received / (1024.0 * 1024), 0, 'f', 1)
                    .arg(total > 0
                             ? QStringLiteral(" / %1 MiB").arg(total / (1024.0 * 1024), 0, 'f', 1)
                             : QString{}));
        } else if (paused) {
            status->setText(catalog.status() + ui::text("initial_data_setup.status.set_text_2"));
        } else {
            status->setText(ui::text("initial_data_setup.status.set_text_3"));
        }
    };
    const auto advance = [&] {
        if (owned && !catalog.downloading()) {
            paused = !ready(*owned);
            owned.reset();
        }
        if (!paused && !catalog.downloading()) {
            const auto missing = std::find_if(required.begin(), required.end(),
                                              [&](auto product) { return !ready(product); });
            if (missing == required.end()) {
                dialog.accept();
                return;
            }
            owned = *missing;
            if (download)
                download(*missing);
            else
                catalog.start_download({}, *missing);
        }
        refresh();
    };
    QObject::connect(retry, &QPushButton::clicked, &dialog, [&] {
        paused = false;
        advance();
    });
    QObject::connect(manage, &QPushButton::clicked, &dialog, [&] {
        show_data_center(&dialog, catalog);
        paused = false;
        advance();
    });
    QObject::connect(copy, &QPushButton::clicked, &dialog, [&] {
        QApplication::clipboard()->setText(
            catalog.update_status(catalog.active_product()).diagnostics);
    });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &dialog, advance);
    poll.start(100);
    refresh();
    QTimer::singleShot(0, &dialog, advance);
    const auto available = dialog.screen()->availableGeometry().size();
    dialog.resize(std::min(640, available.width() - 32), std::min(540, available.height() - 64));
    if (visible_ready)
        QTimer::singleShot(3000, &dialog, [&dialog, visible_ready] {
            if (dialog.isVisible())
                visible_ready();
        });
    const bool accepted = dialog.exec() == QDialog::Accepted;
    poll.stop();
    if (owned && catalog.downloading() && catalog.active_product() == *owned)
        catalog.cancel_download();
    return accepted && complete();
}
} // namespace orbelle::desktop
