#include "preferences_dialog.hpp"
#include "../ui_text.hpp"
#include "data_center_widget.hpp"
#include "form_kit.hpp"
#include "network_settings.hpp"
#include "responsive_layout.hpp"
#include "workbench_spin_box.hpp"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>

namespace orbelle::desktop {
namespace {
constexpr std::array insertion_keys{"satellite", "ground-station", "sensor", "region"};
QLabel *paragraph(const QString &text) {
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}
} // namespace

PreferencesDialog::PreferencesDialog(NetworkSettings &settings, EopCatalog *catalog,
                                     QWidget *parent, PreferencesPage initial,
                                     std::optional<DataProduct> initial_product)
    : QDialog(parent), settings_(settings), catalog_(catalog) {
    setObjectName("applicationPreferencesDialog");
    setWindowTitle(ui::text("network_settings.title"));
    setWindowIcon(workbench_icon(WorkbenchIcon::properties));
    setSizeGripEnabled(true);
    setMinimumSize(360, 320);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *content = new QWidget;
    auto *columns = new QHBoxLayout(content);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);
    auto *navigation = new QTreeWidget;
    navigation->setObjectName("propertyNavigation");
    navigation->setProperty("preferencesTree", true);
    navigation->setAccessibleName(ui::text("preferences.navigation"));
    navigation->setHeaderHidden(true);
    navigation->setRootIsDecorated(true);
    navigation->setFixedWidth(220);
    navigation->setIndentation(14);
    navigation->setUniformRowHeights(false);
    auto *compact = new QWidget;
    auto *compact_layout = new QHBoxLayout(compact);
    compact_layout->setContentsMargins(16, 12, 16, 0);
    auto *selector = new QComboBox;
    selector->setObjectName("preferencesPageSelector");
    selector->setAccessibleName(ui::text("preferences.navigation"));
    compact_layout->addWidget(selector);
    const QStringList labels{ui::text("network_settings.heading"), ui::text("updates.title"),
                             ui::text("preferences.data"), ui::text("preferences.insertion")};
    for (int i = 0; i < labels.size(); ++i) {
        auto *item = new QTreeWidgetItem(navigation, {labels[i]});
        item->setData(0, Qt::UserRole + 20, i);
        item->setSizeHint(0, QSize(0, 40));
    }
    pages_ = new QStackedWidget;
    pages_->setObjectName("preferencesPages");
    pages_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    columns->addWidget(compact);
    columns->addWidget(navigation);
    columns->addWidget(pages_, 1);
    layout->addWidget(content, 1);
    new WidthObserver(content, [=, mode = WidthMode{}](int width) mutable {
        const bool narrow = mode.narrow(width, 680);
        navigation->setVisible(!narrow);
        compact->setVisible(narrow);
        columns->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    });
    QWidget *body = nullptr;
    QVBoxLayout *rows = nullptr;
    const auto page = [&](const char *name, const QString &heading) {
        auto *scroll = new QScrollArea;
        scroll->setObjectName(name);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        body = new QWidget;
        body->setMaximumWidth(760);
        rows = new QVBoxLayout(body);
        rows->setContentsMargins(theme::space::page);
        rows->setSpacing(theme::space::md);
        auto *title = new QLabel(heading);
        title->setObjectName("panelTitle");
        rows->addWidget(title);
        scroll->setWidget(body);
        pages_->addWidget(scroll);
    };
    const auto group = [&](const QString &heading) {
        auto *frame = new FormGroup(heading);
        frame->setProperty("leading", true);
        rows->addWidget(frame);
        responsive_form(frame->form(), 440);
        return frame;
    };
    QSettings saved(settings.preferences_file(), QSettings::IniFormat);
    page("networkSettingsScroll", labels[0]);
    rows->addWidget(paragraph(ui::text("network_settings.scope")));
    auto *proxy = group(ui::text("network_settings.proxy"));
    proxy->add_help(ui::text("network_settings.system_help"));
    mode_ = new QComboBox;
    mode_->setObjectName("networkProxyMode");
    mode_->setMaximumWidth(440);
    mode_->setAccessibleName(ui::text("network_settings.mode"));
    mode_->addItem(ui::text("network_settings.system"), static_cast<int>(ProxyMode::system));
    mode_->addItem(ui::text("network_settings.direct"), static_cast<int>(ProxyMode::direct));
    mode_->addItem(ui::text("network_settings.manual"), static_cast<int>(ProxyMode::manual));
    mode_->setCurrentIndex(mode_->findData(static_cast<int>(settings.value().mode)));
    proxy->form()->addRow(ui::text("network_settings.mode"), mode_);
    address_ = new QLineEdit(settings.value().address);
    address_->setObjectName("networkProxyAddress");
    address_->setMaximumWidth(440);
    address_->setAccessibleName(ui::text("network_settings.address"));
    address_->setPlaceholderText(ui::text("network_settings.address_example"));
    address_->setClearButtonEnabled(true);
    proxy->form()->addRow(ui::text("network_settings.address"), address_);
    auto *hint = paragraph(ui::text("network_settings.address_help"));
    hint->setObjectName("muted");
    proxy->form()->addRow(hint);
    rows->addWidget(paragraph(ui::text("network_settings.effective")));
    rows->addStretch();
    const auto refresh_proxy = [this, hint] {
        const bool manual = mode_->currentData().toInt() == static_cast<int>(ProxyMode::manual);
        address_->setEnabled(manual);
        hint->setEnabled(manual);
    };
    connect(mode_, &QComboBox::currentIndexChanged, this, [this, refresh_proxy] {
        changed();
        refresh_proxy();
    });
    connect(address_, &QLineEdit::textChanged, this, [this] { changed(); });
    refresh_proxy();

    page("updatePreferencesScroll", labels[1]);
    rows->addWidget(paragraph(ui::text("preferences.updates_help")));
    auto *software = group(ui::text("preferences.startup"));
    startup_ = new QCheckBox(ui::text("updates.startup"));
    startup_->setObjectName("preferencesStartupUpdates");
    startup_->setChecked(saved.value("updates/startup", true).toBool());
    software->form()->addRow(startup_);
    rows->addStretch();
    connect(startup_, &QCheckBox::toggled, this, [this] { changed(); });

    auto *data_root = navigation->topLevelItem(static_cast<int>(PreferencesPage::data));
    if (catalog_) {
        data_draft_ = catalog_->preferences();
        auto *data_page = create_data_center_widget(
            pages_, *catalog_, {&data_draft_, [this] { changed(); }, navigation, data_root});
        pages_->addWidget(data_page);
    } else {
        page("dataPreferencesScroll", labels[2]);
        rows->addWidget(paragraph(ui::text("preferences.data_unavailable")));
        rows->addStretch();
    }

    page("insertionPreferencesScroll", labels[3]);
    rows->addWidget(paragraph(ui::text("preferences.insertion_help")));
    auto *creation = group(ui::text("preferences.default_methods"));
    const QStringList types{ui::text("insert_object_dialog.satellite"),
                            ui::text("insert_object_dialog.station"),
                            ui::text("insert_object_dialog.sensor"), ui::text("region.title")};
    for (int i = 0; i < 4; ++i) {
        auto *method = new QComboBox;
        method->setObjectName(QStringLiteral("preferencesInsertMethod%1").arg(i));
        method->setMaximumWidth(380);
        method->setAccessibleName(types[i]);
        method->addItem(ui::text("insert_object_dialog.properties"), "properties");
        method->addItem(ui::text("insert_object_dialog.label_3"), "defaults");
        if (i == 0) {
            method->addItem(ui::text("insert_object_dialog.wizard"), "orbit-wizard");
            method->addItem(ui::text("insert_object_dialog.tle"), "tle-file");
        }
        const auto value =
            saved
                .value(QStringLiteral("insertion/default-method/") + insertion_keys[i],
                       "properties")
                .toString();
        method->setCurrentIndex(std::max(0, method->findData(value)));
        creation->form()->addRow(types[i], method);
        insertion_[i] = method;
        connect(method, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    }
    auto *behavior = group(ui::text("preferences.insertion_behavior"));
    keep_open_ = new QCheckBox(ui::text("insert_object_dialog.keep_open"));
    keep_open_->setObjectName("preferencesKeepInsertOpen");
    keep_open_->setChecked(saved.value("insertion/keep-open", false).toBool());
    behavior->form()->addRow(keep_open_);
    behavior->add_help(ui::text("insert_object_dialog.keep_open_hint"));
    rows->addStretch();
    connect(keep_open_, &QCheckBox::toggled, this, [this] { changed(); });

    auto *footer = new QWidget;
    footer->setObjectName("preferencesFooter");
    auto *footer_layout = new QVBoxLayout(footer);
    footer_layout->setContentsMargins(theme::space::footer);
    error_ = paragraph(settings.load_error());
    error_->setObjectName("inputError");
    error_->setVisible(!error_->text().isEmpty());
    footer_layout->addWidget(error_);
    auto *buttons = new QDialogButtonBox;
    auto *ok = buttons->addButton(ui::text("common.ok"), QDialogButtonBox::AcceptRole);
    ok->setObjectName("savePreferences");
    ok->setProperty("primary", true);
    ok->setDefault(true);
    auto *cancel = buttons->addButton(ui::text("common.cancel"), QDialogButtonBox::RejectRole);
    cancel->setObjectName("cancelPreferences");
    apply_ = buttons->addButton(ui::text("common.apply"), QDialogButtonBox::ApplyRole);
    apply_->setObjectName("applyPreferences");
    apply_->setAutoDefault(false);
    apply_->setEnabled(false);
    footer_layout->addWidget(buttons);
    layout->addWidget(footer);
    connect(ok, &QPushButton::clicked, this, [this] {
        if (apply())
            accept();
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(apply_, &QPushButton::clicked, this, [this] { (void)apply(); });
    QVector<QTreeWidgetItem *> destinations;
    QTreeWidgetItem *initial_item = navigation->topLevelItem(static_cast<int>(initial));
    for (QTreeWidgetItemIterator item(navigation); *item; ++item) {
        auto *root = *item;
        while (root->parent())
            root = root->parent();
        (*item)->setData(0, Qt::UserRole + 20, root->data(0, Qt::UserRole + 20));
        (*item)->setData(0, Qt::UserRole + 21, selector->count());
        if ((*item)->parent())
            (*item)->setSizeHint(0, QSize(0, 32));
        selector->addItem((*item)->parent() ? labels[2] + " · " + (*item)->text(0)
                                            : (*item)->text(0));
        destinations.push_back(*item);
        if (initial == PreferencesPage::data && initial_product &&
            (*item)->data(0, Qt::UserRole).isValid() &&
            (*item)->data(0, Qt::UserRole).toInt() == static_cast<int>(*initial_product))
            initial_item = *item;
    }
    connect(navigation, &QTreeWidget::currentItemChanged, this, [=, this](QTreeWidgetItem *item) {
        if (!item)
            return;
        pages_->setCurrentIndex(item->data(0, Qt::UserRole + 20).toInt());
        const QSignalBlocker blocker(selector);
        selector->setCurrentIndex(item->data(0, Qt::UserRole + 21).toInt());
        if (item == data_root)
            item->setExpanded(true);
    });
    connect(selector, &QComboBox::currentIndexChanged, this, [=](int index) {
        if (index >= 0 && index < destinations.size())
            navigation->setCurrentItem(destinations[index]);
    });
    connect(pages_, &QStackedWidget::currentChanged, this, [=](int index) {
        if (!navigation->currentItem() ||
            navigation->currentItem()->data(0, Qt::UserRole + 20).toInt() != index)
            navigation->setCurrentItem(navigation->topLevelItem(index));
    });
    data_root->setExpanded(initial == PreferencesPage::data);
    navigation->setCurrentItem(initial_item);
    pages_->setCurrentIndex(static_cast<int>(initial));
    selector->setCurrentIndex(initial_item->data(0, Qt::UserRole + 21).toInt());
    resize(QSize(1040, 720).boundedTo(screen()->availableGeometry().size() - QSize(32, 64)));
}

void PreferencesDialog::changed() {
    if (error_)
        error_->hide();
    if (apply_)
        apply_->setEnabled(true);
}
bool PreferencesDialog::apply() {
    const ProxySettings proxy{static_cast<ProxyMode>(mode_->currentData().toInt()),
                              address_->text()};
    if (proxy.mode == ProxyMode::manual && !NetworkSettings::parse_address(proxy.address)) {
        pages_->setCurrentIndex(static_cast<int>(PreferencesPage::network));
        error_->setText(ui::text("network_settings.invalid_address"));
        error_->show();
        address_->setFocus();
        return false;
    }
    QVariantMap other{{"updates/startup", startup_->isChecked()},
                      {"insertion/keep-open", keep_open_->isChecked()}};
    for (int i = 0; i < 4; ++i)
        other.insert(QStringLiteral("insertion/default-method/") + insertion_keys[i],
                     insertion_[i]->currentData());
    std::optional<DataPreferences> previous;
    QString error;
    try {
        if (catalog_) {
            previous = catalog_->preferences();
            catalog_->set_preferences(data_draft_);
        }
        if (!settings_.save(proxy, error, other)) {
            if (previous)
                catalog_->set_preferences(*previous);
            error_->setText(error);
            error_->show();
            return false;
        }
    } catch (const std::exception &failure) {
        error_->setText(ui::text("preferences.save_failed").arg(QString::fromUtf8(failure.what())));
        error_->show();
        return false;
    }
    address_->setText(settings_.value().address);
    error_->hide();
    apply_->setEnabled(false);
    return true;
}
} // namespace orbelle::desktop
