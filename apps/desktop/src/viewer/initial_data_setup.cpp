#include "initial_data_setup.hpp"
#include "../ui_text.hpp"
#include "eop_catalog.hpp"
#include "responsive_layout.hpp"
#include "workbench_theme.hpp"
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
#include <orbitforge/runtime/ephemeris.hpp>

namespace orbitforge::desktop {
std::vector<DataProduct> initial_data_products() {
    // UTC must be ready before interpreting either EOP product.
    std::vector products{DataProduct::utc, DataProduct::c04, DataProduct::finals2000a,
                         DataProduct::egm2008};
    if (runtime::ephemeris_available())
        products.push_back(DataProduct::de440s);
    return products;
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
                          const std::function<void(DataProduct)> &download) {
    const auto complete = [&] {
        return std::all_of(required.begin(), required.end(),
                           [&](auto product) { return catalog.has_full_download(product); });
    };
    if (complete())
        return true;
    QDialog dialog(parent);
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
    auto *cancel = new QPushButton(ui::text("initial_data_setup.cancel"));
    cancel->setObjectName("initialDataCancel");
    actions->addWidget(retry);
    actions->addWidget(manage);
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
            states[i]->setText(
                catalog.has_full_download(product) ? ui::text("initial_data_setup.state")
                : running                          ? ui::text("initial_data_setup.label_9")
                : state == DataUpdateState::failed ? ui::text("initial_data_setup.label_10")
                                                   : ui::text("initial_data_setup.label_11"));
        }
        retry->setVisible(paused);
        manage->setEnabled(!catalog.downloading());
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
            paused = !catalog.has_full_download(*owned);
            owned.reset();
        }
        if (!paused && !catalog.downloading()) {
            const auto missing = std::find_if(required.begin(), required.end(), [&](auto product) {
                return !catalog.has_full_download(product);
            });
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
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &dialog, advance);
    poll.start(100);
    refresh();
    QTimer::singleShot(0, &dialog, advance);
    const auto available = dialog.screen()->availableGeometry().size();
    dialog.resize(std::min(640, available.width() - 32), std::min(540, available.height() - 64));
    const bool accepted = dialog.exec() == QDialog::Accepted;
    poll.stop();
    if (owned && catalog.downloading() && catalog.active_product() == *owned)
        catalog.cancel_download();
    return accepted && complete();
}
} // namespace orbitforge::desktop
