#include "hpop_settings.hpp"
#include "quantity_editor.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <limits>
#include <orbitforge/runtime/ephemeris.hpp>

namespace orbitforge::desktop {
bool edit_hpop_settings(
    core::HpopOptions &options, QWidget *parent, const std::vector<runtime::CatalogGravity> &models,
    const std::vector<runtime::CatalogEphemeris> &ephemerides,
    const std::function<std::vector<runtime::CatalogEphemeris>()> &manage_ephemerides) {
    auto installed_ephemerides = ephemerides;
    QDialog dialog(parent);
    dialog.setObjectName("hpopSettingsDialog");
    dialog.setWindowTitle(QStringLiteral("HPOP · 数值传播"));
    dialog.setStyleSheet(R"(
        QFrame#hpopResource { background: #202d39; border: 1px solid #354958; border-radius: 10px; }
        QLabel#hpopEphemerisStatus { color: #9fb8c9; font-size: 12px; }
        QLabel#hpopResourceTitle { color: #d9edf7; font-size: 16px; font-weight: 600; }
        QLabel#hpopResourceBadge { color: #9fcde5; background: #294353; border-radius: 5px; padding: 3px 9px; font-size: 11px; }
        QFrame#hpopBodyCard { background: #1c2732; border: 1px solid #2f404e; border-radius: 10px; }
        QCheckBox#hpopSunGravity, QCheckBox#hpopMoonGravity { font-size: 14px; font-weight: 600; }
        QLineEdit[readOnly="true"] { color: #a8bdcd; }
    )");
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(16);
    auto *heading = new QLabel(QStringLiteral("HPOP 轨道传播"));
    heading->setObjectName("panelTitle");
    layout->addWidget(heading);
    auto *subtitle = new QLabel(QStringLiteral("力模型与积分参数"));
    subtitle->setObjectName("hint");
    layout->addWidget(subtitle);
    auto *tabs = new QTabWidget;
    tabs->setObjectName("hpopSettingsTabs");
    layout->addWidget(tabs, 1);
    const auto page = [&](const QString &title) {
        auto *scroll = new QScrollArea;
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
        tabs->addTab(scroll, title);
        return form;
    };
    const auto note = [](QFormLayout *form, const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("hint");
        label->setWordWrap(true);
        form->addRow(label);
    };
    auto *gravity = page(QStringLiteral("地球引力"));
    auto *model = new QComboBox;
    model->setObjectName("hpopGravityModel");
    model->addItems({QStringLiteral("EGM96 · 球谐重力"), QStringLiteral("点质量 · 对照计算")});
    model->setItemText(0, QStringLiteral("EGM96 · 内置 20 × 20"));
    model->setItemData(0, QString{});
    model->setItemData(1, QString{});
    for (const auto &entry : models)
        model->addItem(QString::fromStdString(entry.info.model) + QStringLiteral(" · 数据中心"),
                       QString::fromStdString(entry.selection));
    model->setCurrentIndex(options.gravity_degree == 0 ? 1 : 0);
    if (!options.gravity_model.empty()) {
        const auto id = QString::fromStdString(options.gravity_model);
        if (model->findData(id) < 0)
            model->addItem(id + QStringLiteral(" · 未安装"), id);
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
    const auto update_gravity = [=] {
        degree->setEnabled(model->currentIndex() != 1);
        order->setEnabled(model->currentIndex() != 1);
        int limit = core::egm96_max_degree;
        for (const auto &entry : models)
            if (entry.selection == model->currentData().toString().toStdString())
                limit = std::min(entry.info.degree, core::gravity_max_degree);
        degree->setMaximum(limit);
        order->setMaximum(degree->value());
    };
    QObject::connect(model, &QComboBox::currentIndexChanged, &dialog, update_gravity);
    update_gravity();
    note(gravity,
         QStringLiteral("在数据中心下载 EGM2008 / EGM96，或导入外部重力文件。文件模型计算支持最高 "
                        "360 × 360；内置示例最高 20 × 20。Order 为 0 时只保留带谐项。"));
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
    auto *altitude =
        quantity(gravity, QStringLiteral("最低地心高度"), "hpopMinimumAltitude", Unit::metre,
                 options.minimum_altitude_m, 0, std::numeric_limits<double>::max());
    note(gravity, QStringLiteral("高度相对所选重力场参考球面；积分中低于此高度会停止并报错。"));
    auto *third = page(QStringLiteral("第三体引力"));
    auto *resource = new QFrame;
    resource->setObjectName("hpopResource");
    auto *resource_layout = new QVBoxLayout(resource);
    resource_layout->setContentsMargins(18, 16, 18, 16);
    resource_layout->setSpacing(10);
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
            ready ? QStringLiteral("%1 — %2 TAI\n太阳与月球共用此星历")
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
    struct BodyFields {
        QCheckBox *enabled;
        QComboBox *source;
        QuantityEditor *mu;
    };
    const auto body_fields = [&](const QString &title, const char *name, bool enabled,
                                 std::optional<double> override_mu, double default_mu) {
        auto *body = new QFrame;
        body->setObjectName("hpopBodyCard");
        auto *form = new QFormLayout(body);
        form->setContentsMargins(18, 16, 18, 16);
        form->setSpacing(12);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        auto *check = new QCheckBox(title);
        check->setObjectName(QString::fromLatin1(name) + "Gravity");
        check->setChecked(enabled);
        auto *source = new QComboBox;
        source->setObjectName(QString::fromLatin1(name) + "Source");
        source->addItems({QStringLiteral("星历配套常数（默认）"), QStringLiteral("手动输入")});
        source->setCurrentIndex(override_mu ? 1 : 0);
        form->addRow(check, source);
        source->setToolTip(QStringLiteral("GM 的来源，与位置星历分别设置"));
        auto *mu = new QuantityEditor(Unit::kilometres_cubed_per_second_squared);
        mu->setObjectName(QString::fromLatin1(name) + "Mu");
        mu->set_label(title + QStringLiteral(" GM"));
        mu->set_range(std::numeric_limits<double>::min(), std::numeric_limits<double>::max(),
                      Unit::metres_cubed_per_second_squared);
        mu->set_value(override_mu.value_or(default_mu), Unit::metres_cubed_per_second_squared);
        form->addRow(QStringLiteral("引力参数 GM"), mu);
        auto *input = mu->findChild<QLineEdit *>("quantityValue");
        // Preserve even an unfinished manual draft and its selected display unit.
        auto manual = std::make_shared<std::pair<QString, Unit>>(input->text(), mu->unit());
        const auto update = [=] {
            source->setEnabled(check->isChecked());
            mu->setEnabled(check->isChecked());
            mu->set_read_only(source->currentIndex() == 0);
        };
        QObject::connect(source, &QComboBox::currentIndexChanged, &dialog, [=](int index) {
            if (index == 0) {
                *manual = {input->text(), mu->unit()};
                mu->set_value(default_mu, Unit::metres_cubed_per_second_squared);
            } else {
                mu->set_unit(manual->second);
                input->setText(manual->first);
            }
            update();
        });
        QObject::connect(check, &QCheckBox::toggled, &dialog, update);
        update();
        third->addRow(body);
        return BodyFields{check, source, mu};
    };
    const auto sun = body_fields(QStringLiteral("太阳"), "hpopSun", options.sun_gravity,
                                 options.sun_mu, runtime::de440_sun_mu);
    const auto moon = body_fields(QStringLiteral("月球"), "hpopMoon", options.moon_gravity,
                                  options.moon_mu, runtime::de440_moon_mu);
    note(third,
         QStringLiteral(
             "GM 默认采用 DE440 配套常数。切换为手动输入时，可独立调整每个天体的引力强度。"));
    auto *integration = page(QStringLiteral("积分设置"));
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
                 options.position_tolerance_m, 0, std::numeric_limits<double>::max());
    auto *velocity = quantity(adaptive, QStringLiteral("速度绝对容差"), "hpopVelocityTolerance",
                              Unit::metres_per_second, options.velocity_tolerance_mps, 0,
                              std::numeric_limits<double>::max());
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
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            auto candidate = options;
            candidate.sun_gravity = sun.enabled->isChecked();
            candidate.moon_gravity = moon.enabled->isChecked();
            candidate.ephemeris_model = options.ephemeris_model;
            const auto read_mu = [](const BodyFields &fields) -> std::optional<double> {
                if (fields.source->currentIndex() == 0)
                    return std::nullopt;
                const auto mu = fields.mu->value(Unit::metres_cubed_per_second_squared);
                return mu;
            };
            candidate.sun_mu = read_mu(sun);
            candidate.moon_mu = read_mu(moon);
            candidate.gravity_model = model->currentData().toString().toStdString();
            candidate.gravity_mu = core::egm96_mu;
            if (!candidate.gravity_model.empty()) {
                const auto found =
                    std::find_if(models.begin(), models.end(), [&](const auto &entry) {
                        return entry.selection == candidate.gravity_model;
                    });
                if (found == models.end())
                    throw std::runtime_error("请先在数据中心下载或导入所选重力场。");
                candidate.gravity_mu = found->info.mu;
                if (order->value() > found->info.order)
                    throw std::runtime_error("所选 Order 超出重力文件的可用范围。");
            }
            candidate.gravity_degree = model->currentIndex() != 1 ? degree->value() : 0;
            candidate.gravity_order = model->currentIndex() != 1 ? order->value() : 0;
            candidate.integrator = method->currentIndex() == 0 ? core::HpopIntegrator::rkf78
                                                               : core::HpopIntegrator::rk4;
            candidate.minimum_altitude_m = altitude->value(Unit::metre);
            if (candidate.integrator == core::HpopIntegrator::rkf78) {
                candidate.min_step_seconds = minimum->value(Unit::second);
                candidate.max_step_seconds = maximum->value(Unit::second);
                candidate.relative_tolerance = relative->value(Unit::one);
                candidate.position_tolerance_m = position->value(Unit::metre);
                candidate.velocity_tolerance_mps = velocity->value(Unit::metres_per_second);
            } else {
                candidate.fixed_step_seconds = fixed->value(Unit::second);
            }
            core::validate_hpop_options(candidate);
            options = candidate;
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    });
    const auto available = dialog.screen()->availableGeometry().size();
    dialog.resize(std::min(640, available.width() - 32), std::min(700, available.height() - 64));
    return dialog.exec() == QDialog::Accepted;
}
} // namespace orbitforge::desktop
