#include "hpop_settings.hpp"
#include "eop_catalog.hpp"
#include "quantity_editor.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <limits>
#include <orbitforge/runtime/ephemeris.hpp>

namespace orbitforge::desktop {
bool edit_hpop_settings(
    core::HpopOptions &options, QWidget *parent, const std::vector<runtime::CatalogGravity> &models,
    const std::vector<runtime::CatalogEphemeris> &ephemerides,
    const std::function<std::vector<runtime::CatalogEphemeris>()> &manage_ephemerides,
    EopCatalog *catalog) {
    QPointer<EopCatalog> data = catalog;
    auto installed_models = data ? data->data().gravity_models() : models;
    if (data)
        std::erase_if(installed_models, [](const auto &entry) {
            std::error_code error;
            return !std::filesystem::is_regular_file(entry.file, error);
        });
    auto installed_ephemerides = ephemerides;
    QDialog dialog(parent);
    dialog.setObjectName("hpopSettingsDialog");
    dialog.setWindowTitle(QStringLiteral("HPOP · 数值传播"));
    dialog.setStyleSheet(R"(
        QFrame#hpopResource { background: #202d39; border: 1px solid #354958; border-radius: 10px; }
        QLabel#hpopEphemerisStatus { color: #9fb8c9; font-size: 12px; }
        QLabel#hpopResourceTitle { color: #d9edf7; font-size: 16px; font-weight: 600; }
        QLabel#hpopResourceBadge { color: #9fcde5; background: #294353; border-radius: 5px; padding: 3px 9px; font-size: 11px; }
        QFrame[hpopSection="true"] { background: #202b36; border: 1px solid #334654; border-radius: 8px; }
        QLabel#hpopSectionTitle { color: #d9edf7; font-size: 14px; font-weight: 600; }
        QLabel#hpopPendingBadge { color: #c7bac8; background: #38323d; border-radius: 5px; padding: 3px 9px; font-size: 11px; }
        QTabWidget#hpopForceModelTabs QTabBar::tab { background: transparent; padding: 5px 10px; border-bottom-color: #334654; }
        QTabWidget#hpopForceModelTabs QTabBar::tab:selected { background: #243847; border-bottom-color: #8cbfd5; }
        QLineEdit[readOnly="true"] { color: #a8bdcd; }
    )");
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(16);
    auto *heading = new QLabel(QStringLiteral("HPOP 轨道传播"));
    heading->setObjectName("panelTitle");
    layout->addWidget(heading);
    auto *subtitle = new QLabel(QStringLiteral("力模型、积分器与协方差设置"));
    subtitle->setObjectName("hint");
    layout->addWidget(subtitle);
    auto *tabs = new QTabWidget;
    tabs->setObjectName("hpopSettingsTabs");
    layout->addWidget(tabs, 1);
    auto *force_page = new QWidget;
    force_page->setObjectName("hpopForceModelsPage");
    auto *force_layout = new QVBoxLayout(force_page);
    force_layout->setContentsMargins(0, 10, 0, 0);
    auto *force_tabs = new QTabWidget;
    force_tabs->setObjectName("hpopForceModelTabs");
    force_layout->addWidget(force_tabs);
    tabs->addTab(force_page, QStringLiteral("力模型"));
    const auto page = [&](QTabWidget *owner, const QString &title, const char *name) {
        auto *scroll = new QScrollArea;
        scroll->setObjectName(name);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *content = new QWidget;
        auto *form = new QFormLayout(content);
        form->setContentsMargins(0, 20, 6, 8);
        form->setHorizontalSpacing(20);
        form->setVerticalSpacing(16);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        scroll->setWidget(content);
        owner->addTab(scroll, title);
        return form;
    };
    const auto note = [](QFormLayout *form, const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("hint");
        label->setWordWrap(true);
        form->addRow(label);
    };
    const auto section = [](QFormLayout *parent_form, const QString &title, const char *name) {
        auto *frame = new QFrame;
        frame->setObjectName(name);
        frame->setProperty("hpopSection", true);
        auto *form = new QFormLayout(frame);
        form->setContentsMargins(14, 12, 14, 12);
        form->setHorizontalSpacing(14);
        form->setVerticalSpacing(8);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto *section_title = new QLabel(title);
        section_title->setObjectName("hpopSectionTitle");
        form->addRow(section_title);
        parent_form->addRow(frame);
        return form;
    };
    auto *gravity_page = page(force_tabs, QStringLiteral("引力"), "hpopGravityPage");
    gravity_page->setVerticalSpacing(12);
    gravity_page->setContentsMargins(0, 12, 6, 8);
    auto *gravity =
        section(gravity_page, QStringLiteral("中心天体引力"), "hpopCentralGravitySection");
    auto *model = new QComboBox;
    model->setObjectName("hpopGravityModel");
    model->addItems({QStringLiteral("EGM96 · 内置示例 20 × 20"),
                     QStringLiteral("点质量 · 对照计算"), QStringLiteral("EGM2008"),
                     QStringLiteral("EGM96")});
    model->setItemData(0, QString{});
    model->setItemData(1, QString{});
    model->setItemData(2, QStringLiteral("egm2008"));
    model->setItemData(3, QStringLiteral("egm96"));
    const auto refresh_models = [&] {
        for (const auto &entry : installed_models) {
            const auto id = QString::fromStdString(entry.selection);
            const auto title = QString::fromStdString(entry.info.model) +
                               (entry.selection == "egm2008" || entry.selection == "egm96"
                                    ? QString{}
                                    : QStringLiteral(" · 外部文件"));
            const auto index = model->findData(id);
            if (index < 0)
                model->addItem(title, id);
            else
                model->setItemText(index, title);
        }
    };
    refresh_models();
    model->setCurrentIndex(options.gravity_degree == 0 ? 1 : 0);
    if (!options.gravity_model.empty()) {
        const auto id = QString::fromStdString(options.gravity_model);
        if (model->findData(id) < 0)
            model->addItem(id, id);
        model->setCurrentIndex(model->findData(id));
    }
    gravity->addRow(QStringLiteral("地球引力"), model);
    auto *degree = new QSpinBox;
    degree->setObjectName("hpopGravityDegree");
    degree->setRange(2, core::gravity_max_degree);
    degree->setValue(std::max(2, options.gravity_degree));
    auto *order = new QSpinBox;
    order->setObjectName("hpopGravityOrder");
    order->setRange(0, degree->value());
    order->setValue(options.gravity_order);
    gravity->addRow(QStringLiteral("最大阶 Degree"), degree);
    gravity->addRow(QStringLiteral("最大次 Order"), order);
    QObject::connect(degree, &QSpinBox::valueChanged, &dialog,
                     [order](int value) { order->setMaximum(value); });
    const auto update_gravity = [&] {
        degree->setEnabled(model->currentIndex() != 1);
        order->setEnabled(model->currentIndex() != 1);
        int limit = model->currentData().toString().isEmpty() ? core::egm96_max_degree
                                                              : core::gravity_max_degree;
        for (const auto &entry : installed_models)
            if (entry.selection == model->currentData().toString().toStdString())
                limit = std::min(entry.info.degree, core::gravity_max_degree);
        degree->setMaximum(limit);
        order->setMaximum(degree->value());
    };
    QObject::connect(model, &QComboBox::currentIndexChanged, &dialog, update_gravity);
    update_gravity();
    auto *gravity_status = new QLabel;
    gravity_status->setObjectName("hpopGravityStatus");
    gravity_status->setWordWrap(true);
    gravity->addRow(gravity_status);
    auto *gravity_progress = new QProgressBar;
    gravity_progress->setObjectName("hpopGravityProgress");
    gravity_progress->hide();
    gravity->addRow(gravity_progress);
    auto *gravity_actions = new QWidget;
    auto *gravity_action_layout = new QHBoxLayout(gravity_actions);
    gravity_action_layout->setContentsMargins(0, 0, 0, 0);
    auto *download_gravity = new QPushButton(QStringLiteral("下载重力场"));
    download_gravity->setObjectName("hpopDownloadGravity");
    auto *cancel_gravity = new QPushButton(QStringLiteral("取消下载"));
    cancel_gravity->setObjectName("hpopCancelGravityDownload");
    gravity_action_layout->addWidget(download_gravity);
    gravity_action_layout->addWidget(cancel_gravity);
    gravity_action_layout->addStretch();
    gravity->addRow(gravity_actions);
    note(gravity, QStringLiteral("文件模型计算支持最高 360 × 360。Order 为 0 时只保留带谐项。"
                                 "外部重力场可在数据中心导入。"));
    const auto quantity = [](QFormLayout *form, const QString &label, const char *id, Unit unit,
                             double value, double low, double high) {
        auto *field = new QuantityEditor(unit);
        field->setObjectName(id);
        field->set_label(label);
        field->set_range(low, high, unit);
        field->set_value(value, unit);
        form->addRow(label, field);
        return field;
    };
    note(gravity, QStringLiteral("当前使用静态重力场，长期变化、固体潮与海洋潮尚未实现。"));
    auto *third = section(gravity_page, QStringLiteral("第三体引力"), "hpopThirdBodySection");
    third->setVerticalSpacing(8);
    auto *resource = new QFrame;
    resource->setObjectName("hpopResource");
    auto *resource_layout = new QVBoxLayout(resource);
    resource_layout->setContentsMargins(14, 10, 14, 10);
    resource_layout->setSpacing(6);
    auto *resource_header = new QHBoxLayout;
    auto *resource_title = new QLabel(options.ephemeris_model == "de440s"
                                          ? QStringLiteral("JPL DE440s")
                                          : QString::fromStdString(options.ephemeris_model));
    resource_title->setObjectName("hpopResourceTitle");
    auto *resource_badge = new QLabel;
    resource_badge->setObjectName("hpopResourceBadge");
    resource_header->addWidget(resource_title);
    resource_header->addStretch();
    resource_header->addWidget(resource_badge);
    resource_layout->addLayout(resource_header);
    auto *ephemeris_status = new QLabel;
    ephemeris_status->setObjectName("hpopEphemerisStatus");
    ephemeris_status->setWordWrap(true);
    resource_layout->addWidget(ephemeris_status);
    auto *manage = new QToolButton;
    manage->setObjectName("hpopManageEphemerides");
    manage->setText(QStringLiteral("在数据中心管理"));
    manage->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    manage->setIcon(workbench_icon(WorkbenchIcon::database));
    manage->setVisible(static_cast<bool>(manage_ephemerides));
    resource_layout->addWidget(manage, 0, Qt::AlignLeft);
    const auto refresh_ephemeris = [&] {
        const auto found =
            std::find_if(installed_ephemerides.begin(), installed_ephemerides.end(),
                         [&](const auto &v) { return v.selection == options.ephemeris_model; });
        const bool ready = found != installed_ephemerides.end();
        resource_badge->setText(ready ? QStringLiteral("已就绪") : QStringLiteral("未安装"));
        const auto date = [](core::Epoch epoch) {
            return QDateTime(QDate(2000, 1, 1), QTime(12, 0), QTimeZone::UTC)
                .addSecs(epoch.tai_nanoseconds() / 1000000000LL)
                .toString("yyyy-MM-dd");
        };
        ephemeris_status->setText(
            ready ? QStringLiteral("%1 — %2 TAI\n日月与行星共用此星历")
                        .arg(date(found->info.first), date(found->info.last))
                  : QStringLiteral("完整星历约 32 MiB，支持 1850—2150 "
                                   "年。\n启用第三体引力前，请在数据中心下载。"));
    };
    QObject::connect(manage, &QToolButton::clicked, &dialog, [&] {
        try {
            installed_ephemerides = manage_ephemerides();
            refresh_ephemeris();
        } catch (const std::exception &e) {
            ephemeris_status->setText(QString::fromUtf8(e.what()));
        }
    });
    refresh_ephemeris();
    third->addRow(resource);
    struct ManualDraft {
        QString text;
        Unit unit;
        std::optional<double> si;
    };
    struct BodyFields {
        QCheckBox *enabled;
        QComboBox *source;
        QuantityEditor *mu;
        std::shared_ptr<ManualDraft> manual;
        std::shared_ptr<std::optional<core::CbGravity>> cb;
        QString default_manual;
        std::function<void()> update;
    };
    auto *search = new QLineEdit;
    search->setObjectName("hpopThirdBodySearch");
    search->setPlaceholderText(QStringLiteral("搜索天体名称…"));
    third->addRow(search);
    auto *table = new QTableWidget(static_cast<int>(core::third_body_count), 4);
    table->setObjectName("hpopThirdBodyTable");
    table->setHorizontalHeaderLabels({QStringLiteral("天体 Name"), QStringLiteral("使用"),
                                      QStringLiteral("来源 Source"),
                                      QStringLiteral("引力参数 GM")});
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(42);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setMinimumHeight(200);
    table->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table->horizontalHeader()->setMinimumSectionSize(36);
    table->setColumnWidth(0, 110);
    table->setColumnWidth(1, 48);
    table->setColumnWidth(2, 140);
    table->setColumnWidth(3, 220);
    table->horizontalHeader()->setStretchLastSection(true);
    // Table scrolls internally at narrow widths; fields stay alive across resizing.
    third->addRow(table);
    constexpr std::array names{"太阳", "月球", "木星",   "金星",   "土星",
                               "火星", "水星", "天王星", "海王星", "冥王星"};
    core::ThirdBodyDataInfo constants;
    constants.sun_mu = runtime::de440_sun_mu;
    constants.moon_mu = runtime::de440_moon_mu;
    constants.planet_mu = runtime::de440_planet_mu;
    std::array<BodyFields, core::third_body_count> fields;
    for (auto body : core::third_body_list) {
        const auto row = static_cast<int>(body);
        const auto english = QString::fromUtf8(core::third_body_name(body).data());
        const auto title = QString::fromUtf8(names[row]);
        auto *name = new QTableWidgetItem(title + " " + english);
        name->setData(Qt::UserRole, title + " " + english);
        name->setToolTip(core::third_body_naif_id(body) < 10
                             ? QStringLiteral("系统质心位置与系统 GM，包含卫星质量")
                             : QStringLiteral("天体中心位置与天体 GM"));
        table->setItem(row, 0, name);
        const auto &settings = options.third_body(body);
        auto *check = new QCheckBox;
        check->setObjectName("hpop" + english + "Gravity");
        check->setAccessibleName(title + QStringLiteral("引力"));
        check->setChecked(settings.enabled);
        auto *check_cell = new QWidget;
        auto *check_layout = new QHBoxLayout(check_cell);
        check_layout->setContentsMargins(0, 0, 0, 0);
        check_layout->addWidget(check, 0, Qt::AlignCenter);
        table->setCellWidget(row, 1, check_cell);
        auto *source = new QComboBox;
        source->setObjectName("hpop" + english + "Source");
        source->addItems({QStringLiteral("Cb file"), QStringLiteral("JPL DE"),
                          QStringLiteral("User Specified")});
        source->setMinimumWidth(0);
        source->setStyleSheet(QStringLiteral(
            "QComboBox { padding: 4px 5px; } QComboBox::drop-down { width: 18px; }"));
        source->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        source->setMinimumContentsLength(3);
        source->setCurrentIndex(static_cast<int>(settings.source));
        table->setCellWidget(row, 2, source);
        auto *mu = new QuantityEditor(Unit::kilometres_cubed_per_second_squared);
        mu->setObjectName("hpop" + english + "Mu");
        mu->set_label(title + QStringLiteral(" GM"));
        mu->setMaximumWidth(QWIDGETSIZE_MAX);
        mu->set_range(std::numeric_limits<double>::min(), std::numeric_limits<double>::max(),
                      Unit::metres_cubed_per_second_squared);
        mu->set_value(settings.user_mu.value_or(constants.mu(body)),
                      Unit::metres_cubed_per_second_squared);
        auto *input = mu->findChild<QLineEdit *>("quantityValue");
        auto manual = std::make_shared<ManualDraft>(
            ManualDraft{input->text(), mu->unit(), settings.user_mu.value_or(constants.mu(body))});
        auto cb = std::make_shared<std::optional<core::CbGravity>>(settings.cb);
        const auto update = [=] {
            source->setEnabled(check->isChecked());
            mu->setEnabled(check->isChecked());
            mu->set_read_only(source->currentIndex() != 2);
            if (source->currentIndex() == 1)
                mu->set_value(constants.mu(body), Unit::metres_cubed_per_second_squared);
            else if (source->currentIndex() == 0) {
                if (*cb)
                    mu->set_value((*cb)->mu, Unit::metres_cubed_per_second_squared);
                else
                    input->setText(QStringLiteral("待导入"));
            }
            if (source->currentIndex() != 2)
                input->setCursorPosition(0);
            mu->setToolTip(
                source->currentIndex() == 0 && *cb
                    ? QString::fromStdString((*cb)->name + "\nSHA-256: " + (*cb)->content_sha256)
                    : QStringLiteral("JPL DE 使用 DE440 配套常数；自定义值必须大于零"));
        };
        auto previous = std::make_shared<int>(source->currentIndex());
        QObject::connect(source, &QComboBox::currentIndexChanged, &dialog, [=](int index) {
            if (*previous == 2) {
                manual->text = input->text();
                manual->unit = mu->unit();
                try {
                    manual->si = mu->value(Unit::metres_cubed_per_second_squared);
                } catch (const std::exception &) {
                    manual->si.reset();
                }
            }
            if (index == 2) {
                // Reset an unavailable Cb display before converting units. Retain both
                // the exact SI value and literal text, including unfinished manual input.
                mu->set_value(manual->si.value_or(constants.mu(body)),
                              Unit::metres_cubed_per_second_squared);
                mu->set_unit(manual->unit);
                if (manual->si) {
                    const QSignalBlocker blocked(input);
                    input->setText(manual->text);
                } else
                    input->setText(manual->text);
            }
            *previous = index;
            update();
        });
        QObject::connect(check, &QCheckBox::toggled, &dialog, [=] { update(); });
        table->setCellWidget(row, 3, mu);
        fields[row] = {check, source, mu, manual, cb, manual->text, update};
        update();
    }
    const auto resize_columns = [=] {
        const bool compact = table->viewport()->width() < 520;
        const QSignalBlocker blocked(table->horizontalHeader());
        table->setColumnWidth(0, compact ? 54 : 110);
        table->setColumnWidth(1, 40);
        table->setColumnWidth(2, compact ? 92 : 140);
        table->setColumnWidth(3, compact ? 182 : 220);
        table->horizontalHeaderItem(0)->setText(compact ? QStringLiteral("天体")
                                                        : QStringLiteral("天体 Name"));
        table->horizontalHeaderItem(2)->setText(compact ? QStringLiteral("来源")
                                                        : QStringLiteral("来源 Source"));
        for (int row = 0; row < table->rowCount(); ++row) {
            table->item(row, 0)->setText(compact
                                             ? QString::fromUtf8(names[row])
                                             : table->item(row, 0)->data(Qt::UserRole).toString());
            fields[row].source->setItemText(2, compact ? QStringLiteral("自定义")
                                                       : QStringLiteral("User Specified"));
            fields[row].source->setToolTip(QStringLiteral("Cb file / JPL DE / User Specified"));
        }
    };
    QObject::connect(table->horizontalHeader(), &QHeaderView::geometriesChanged, &dialog,
                     resize_columns);
    resize_columns();
    QObject::connect(search, &QLineEdit::textChanged, &dialog, [=](const QString &text) {
        for (int row = 0; row < table->rowCount(); ++row)
            table->setRowHidden(row, !table->item(row, 0)
                                          ->data(Qt::UserRole)
                                          .toString()
                                          .contains(text.trimmed(), Qt::CaseInsensitive));
    });
    auto *cb_controls = new QWidget;
    auto *cb_layout = new QFormLayout(cb_controls);
    cb_layout->setContentsMargins(0, 0, 0, 0);
    cb_layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto *import_cb = new QToolButton;
    import_cb->setObjectName("hpopImportCb");
    import_cb->setText(QStringLiteral("导入所选天体 Cb 文件…"));
    auto *cb_units = new QComboBox;
    cb_units->setObjectName("hpopCbUnits");
    cb_units->addItems({QStringLiteral("文件 GM：m³/s²"), QStringLiteral("文件 GM：km³/s²")});
    cb_layout->addRow(import_cb, cb_units);
    third->addRow(cb_controls);
    auto *cb_status = new QLabel;
    cb_status->setObjectName("hpopCbStatus");
    cb_status->setWordWrap(true);
    third->addRow(cb_status);
    QObject::connect(import_cb, &QToolButton::clicked, &dialog, [&] {
        const auto row = table->currentRow();
        if (row < 0) {
            cb_status->setText(QStringLiteral("请先选择天体行。"));
            return;
        }
        const auto path = QFileDialog::getOpenFileName(&dialog, QStringLiteral("导入天体引力常数"),
                                                       {}, QStringLiteral("Central body (*.cb)"));
        if (path.isEmpty())
            return;
        try {
            auto value = runtime::load_cb_gravity(std::filesystem::path(path.toStdWString()),
                                                  core::third_body_list[row],
                                                  cb_units->currentIndex() == 0 ? 1.0 : 1e9);
            *fields[row].cb = std::move(value);
            fields[row].source->setCurrentIndex(0);
            fields[row].update();
            cb_status->setText(
                QStringLiteral("已导入 %1").arg(QString::fromStdString((*fields[row].cb)->name)));
        } catch (const std::exception &e) {
            cb_status->setText(QString::fromUtf8(e.what()));
        }
    });
    table->selectRow(0);
    note(third,
         QStringLiteral(
             "位置星历：DE440s。Cb file 读取文件中的 Gm / SystemGm；火星与外行星使用系统质心。"));
    const auto pending = [&](QTabWidget *owner, const QString &title, const char *name,
                             const QString &description, const QString &scope) {
        auto *form = page(owner, title, name);
        auto *status = section(form, title, (std::string(name) + "Status").c_str());
        auto *badge = new QLabel(QStringLiteral("尚未实现"));
        badge->setObjectName("hpopPendingBadge");
        status->addRow(badge);
        note(status, description);
        note(status, scope);
        note(form, QStringLiteral("当前传播不包含此项，暂不可启用。"));
    };
    pending(
        force_tabs, QStringLiteral("阻力"), "hpopDragPage",
        QStringLiteral("大气阻力模型尚未接入 HPOP。"),
        QStringLiteral("球形阻力、Cd 与面积质量比、大气密度、空间天气及低空拼接将集中在此配置。"));
    pending(
        force_tabs, QStringLiteral("太阳辐射压"), "hpopSrpPage",
        QStringLiteral("太阳辐射压及阴影模型尚未接入 HPOP。"),
        QStringLiteral("球形模型、Cr 与面积质量比、阴影、太阳位置方法及遮挡天体将集中在此配置。"));
    pending(force_tabs, QStringLiteral("其他"), "hpopAdditionalPage",
            QStringLiteral("附加力模型尚未接入 HPOP。"),
            QStringLiteral("地球反照与热辐射、相对论、卫星质量及自定义力模型将集中在此配置。"));
    auto *integration = page(tabs, QStringLiteral("积分器"), "hpopIntegratorPage");
    auto *method = new QComboBox;
    method->setObjectName("hpopIntegrator");
    method->addItems({QStringLiteral("RKF78 · 自适应步长"), QStringLiteral("RK4 · 固定步长")});
    method->setCurrentIndex(options.integrator == core::HpopIntegrator::rkf78 ? 0 : 1);
    integration->addRow(QStringLiteral("积分器"), method);
    auto *adaptive_widget = new QWidget;
    auto *adaptive = new QFormLayout(adaptive_widget);
    adaptive->setContentsMargins(0, 0, 0, 0);
    adaptive->setVerticalSpacing(16);
    adaptive->setHorizontalSpacing(20);
    adaptive->setRowWrapPolicy(QFormLayout::WrapLongRows);
    adaptive->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *minimum = quantity(adaptive, QStringLiteral("最小步长"), "hpopMinStep", Unit::second,
                             options.min_step_seconds, 1e-6, 3600);
    auto *maximum = quantity(adaptive, QStringLiteral("最大步长"), "hpopMaxStep", Unit::second,
                             options.max_step_seconds, 1e-6, 3600);
    auto *relative = quantity(adaptive, QStringLiteral("相对容差"), "hpopRelativeTolerance",
                              Unit::one, options.relative_tolerance, 1e-14, 1e-3);
    auto *position =
        quantity(adaptive, QStringLiteral("位置绝对容差"), "hpopPositionTolerance", Unit::metre,
                 options.position_tolerance_m, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    auto *velocity =
        quantity(adaptive, QStringLiteral("速度绝对容差"), "hpopVelocityTolerance",
                 Unit::metres_per_second, options.velocity_tolerance_mps,
                 std::numeric_limits<double>::min(), std::numeric_limits<double>::max());
    integration->addRow(adaptive_widget);
    auto *fixed = quantity(integration, QStringLiteral("固定步长"), "hpopFixedStep", Unit::second,
                           options.fixed_step_seconds, 1e-6, 3600);
    const auto update_method = [=] {
        adaptive_widget->setVisible(method->currentIndex() == 0);
        integration->setRowVisible(fixed, method->currentIndex() == 1);
    };
    QObject::connect(method, &QComboBox::currentIndexChanged, &dialog, update_method);
    update_method();
    note(integration, QStringLiteral("输出采样间隔独立设置。容差约束单步数值误差，不代表轨道预报的"
                                     "实际精度。RK4 不进行误差控制。"));
    auto *altitude =
        quantity(integration, QStringLiteral("最低地心高度"), "hpopMinimumAltitude", Unit::metre,
                 options.minimum_altitude_m, 0, std::numeric_limits<double>::max());
    note(integration, QStringLiteral("高度相对所选重力场参考球面；积分中低于此高度会停止并报错。"));
    pending(
        tabs, QStringLiteral("协方差"), "hpopCovariancePage",
        QStringLiteral("协方差传播尚未实现，当前只计算确定性轨迹。"),
        QStringLiteral("初始 6 × 6 矩阵、参考系、独立重力截断与 consider 参数将集中在此配置。"));
    auto *error = new QLabel;
    error->setObjectName("errorText");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    std::optional<DataProduct> queued_gravity, owned_gravity;
    QString gravity_message;
    auto catalog_revision = data ? data->revision() : 0;
    const auto selected_product = [&]() -> std::optional<DataProduct> {
        const auto id = model->currentData().toString();
        if (id == "egm2008")
            return DataProduct::egm2008;
        if (id == "egm96")
            return DataProduct::egm96;
        return {};
    };
    const auto selected_model = [&] {
        return std::find_if(installed_models.begin(), installed_models.end(), [&](const auto &v) {
            return v.selection == model->currentData().toString().toStdString();
        });
    };
    const auto refresh_gravity_status = [&] {
        const auto product = selected_product();
        const auto found = selected_model();
        const bool ready =
            model->currentData().toString().isEmpty() || found != installed_models.end();
        const bool running =
            data && product && data->downloading() && data->active_product() == *product;
        const bool waiting = product && queued_gravity == product;
        gravity_progress->setVisible(running);
        if (running) {
            const auto received = data->received_bytes(), total = data->total_bytes();
            // Once all bytes arrive, validation still has to finish before publication.
            if (total > 0 && received < total) {
                gravity_progress->setRange(0, 1000);
                gravity_progress->setValue(static_cast<int>(1000.0 * received / total));
            } else {
                gravity_progress->setRange(0, 0);
            }
            gravity_status->setText(
                QStringLiteral("%1\n已接收 %2 MiB%3")
                    .arg(data->status())
                    .arg(received / (1024.0 * 1024), 0, 'f', 1)
                    .arg(total > 0
                             ? QStringLiteral(" / %1 MiB").arg(total / (1024.0 * 1024), 0, 'f', 1)
                             : QString{}));
        } else if (waiting) {
            gravity_status->setText(QStringLiteral("等待数据中心当前任务完成后下载所选重力场…"));
        } else if (found != installed_models.end()) {
            gravity_status->setText(
                QStringLiteral("文件：%1 × %2 · 当前计算上限：%3 × %3")
                    .arg(found->info.degree)
                    .arg(found->info.order)
                    .arg(std::min(found->info.degree, core::gravity_max_degree)));
        } else if (model->currentIndex() == 0) {
            gravity_status->setText(QStringLiteral("内置 EGM96 示例，最高 20 × 20。"));
        } else if (model->currentIndex() == 1) {
            gravity_status->setText(QStringLiteral("使用内置地球引力常数进行点质量对照计算。"));
        } else {
            const auto message = !gravity_message.isEmpty() ? gravity_message
                                 : data && product          ? data->update_status(*product).message
                                                            : QString{};
            gravity_status->setText(
                message.isEmpty()
                    ? (product
                           ? QStringLiteral("未安装。首次打开自动下载完整文件：EGM2008 约 240 MiB，"
                                            "EGM96 约 5.4 MiB。")
                           : QStringLiteral("未安装。请在数据中心导入所选外部重力场。"))
                    : message + QStringLiteral("\n所选模型仍未安装，可重试或在数据中心导入。"));
        }
        download_gravity->setVisible(data && product && !ready && !running && !waiting);
        download_gravity->setText(QStringLiteral("下载 / 重试"));
        cancel_gravity->setVisible(queued_gravity.has_value() ||
                                   (data && owned_gravity && data->downloading() &&
                                    data->active_product() == *owned_gravity));
        gravity_actions->setVisible(!download_gravity->isHidden() || !cancel_gravity->isHidden());
        buttons->button(QDialogButtonBox::Ok)->setEnabled(ready || (!running && !waiting));
    };
    const auto request_gravity = [&] {
        const auto product = selected_product();
        queued_gravity.reset();
        gravity_message.clear();
        if (!data || !product || selected_model() != installed_models.end()) {
            refresh_gravity_status();
            return;
        }
        if (data->downloading()) {
            if (data->active_product() != *product)
                queued_gravity = product;
        } else {
            data->start_download({}, *product);
            if (data->downloading())
                owned_gravity = product;
        }
        refresh_gravity_status();
    };
    QObject::connect(model, &QComboBox::currentIndexChanged, &dialog, request_gravity);
    QObject::connect(download_gravity, &QPushButton::clicked, &dialog, request_gravity);
    QObject::connect(cancel_gravity, &QPushButton::clicked, &dialog, [&] {
        queued_gravity.reset();
        gravity_message = QStringLiteral("已取消下载。");
        if (data && owned_gravity && data->downloading() &&
            data->active_product() == *owned_gravity)
            data->cancel_download();
        refresh_gravity_status();
    });
    QTimer gravity_poll;
    QObject::connect(&gravity_poll, &QTimer::timeout, &dialog, [&] {
        if (data && data->revision() != catalog_revision) {
            catalog_revision = data->revision();
            installed_models = data->data().gravity_models();
            refresh_models();
            update_gravity();
        }
        if (data && !data->downloading()) {
            owned_gravity.reset();
            if (queued_gravity)
                request_gravity();
        }
        refresh_gravity_status();
    });
    if (data)
        gravity_poll.start(100);
    refresh_gravity_status();
    QTimer::singleShot(0, &dialog, request_gravity);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        int validation_page = 0;
        std::optional<core::ThirdBody> validation_body;
        QWidget *validation_field = model;
        const auto read = [&](QuantityEditor *field, Unit unit) {
            validation_field = field;
            return field->value(unit);
        };
        try {
            auto candidate = options;
            for (auto body : core::third_body_list) {
                const auto &field = fields[static_cast<std::size_t>(body)];
                validation_body = body;
                validation_field = field.source;
                auto &setting = candidate.third_body(body);
                setting.enabled = field.enabled->isChecked();
                setting.source =
                    static_cast<core::ThirdBodyGravitySource>(field.source->currentIndex());
                setting.cb = *field.cb;
                if (setting.enabled && setting.source == core::ThirdBodyGravitySource::cb_file &&
                    !setting.cb)
                    throw std::runtime_error(std::string(core::third_body_name(body)) +
                                             "：请先导入此天体的 Cb 引力常数文件。");
                if (setting.source == core::ThirdBodyGravitySource::user_specified)
                    setting.user_mu = read(field.mu, Unit::metres_cubed_per_second_squared);
                else if (field.manual->si &&
                         (setting.user_mu || field.manual->text != field.default_manual))
                    setting.user_mu = field.manual->si;
            }
            validation_body.reset();
            validation_field = model;
            candidate.gravity_model = model->currentData().toString().toStdString();
            candidate.gravity_mu = core::egm96_mu;
            if (!candidate.gravity_model.empty()) {
                const auto found = std::find_if(
                    installed_models.begin(), installed_models.end(),
                    [&](const auto &entry) { return entry.selection == candidate.gravity_model; });
                if (found == installed_models.end())
                    throw std::runtime_error("请先在数据中心下载或导入所选重力场。");
                candidate.gravity_mu = found->info.mu;
                if (order->value() > found->info.order) {
                    validation_field = order;
                    throw std::runtime_error("所选 Order 超出重力文件的可用范围。");
                }
            }
            candidate.gravity_degree = model->currentIndex() != 1 ? degree->value() : 0;
            candidate.gravity_order = model->currentIndex() != 1 ? order->value() : 0;
            candidate.integrator = method->currentIndex() == 0 ? core::HpopIntegrator::rkf78
                                                               : core::HpopIntegrator::rk4;
            validation_page = 1;
            candidate.minimum_altitude_m = read(altitude, Unit::metre);
            if (candidate.integrator == core::HpopIntegrator::rkf78) {
                candidate.min_step_seconds = read(minimum, Unit::second);
                candidate.max_step_seconds = read(maximum, Unit::second);
                if (candidate.min_step_seconds > candidate.max_step_seconds)
                    throw std::runtime_error("最大步长不能小于最小步长。");
                candidate.relative_tolerance = read(relative, Unit::one);
                candidate.position_tolerance_m = read(position, Unit::metre);
                candidate.velocity_tolerance_mps = read(velocity, Unit::metres_per_second);
            } else {
                candidate.fixed_step_seconds = read(fixed, Unit::second);
            }
            core::validate_hpop_options(candidate);
            options = candidate;
            dialog.accept();
        } catch (const std::exception &e) {
            tabs->setCurrentIndex(validation_page);
            auto *scroll = dialog.findChild<QScrollArea *>(
                validation_page == 0 ? "hpopGravityPage" : "hpopIntegratorPage");
            if (validation_page == 0) {
                force_tabs->setCurrentIndex(0);
                if (validation_body) {
                    search->clear();
                    const auto row = static_cast<int>(*validation_body);
                    table->selectRow(row);
                    table->scrollToItem(table->item(row, 0));
                }
            }
            error->setText(QString::fromUtf8(e.what()));
            error->show();
            if (scroll)
                scroll->ensureWidgetVisible(validation_field);
            validation_field->setFocus(Qt::OtherFocusReason);
        }
    });
    const auto available = dialog.screen()->availableGeometry().size();
    dialog.resize(std::min(720, available.width() - 32), std::min(700, available.height() - 64));
    const bool accepted = dialog.exec() == QDialog::Accepted;
    gravity_poll.stop();
    // Cancel only work this dialog started; shared data-center tasks keep their owner.
    if (data && owned_gravity && data->downloading() && data->active_product() == *owned_gravity)
        data->cancel_download();
    return accepted;
}
} // namespace orbitforge::desktop
