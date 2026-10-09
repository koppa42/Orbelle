#include "orbit_editor.hpp"
#include "epoch_editor.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>
#include <numbers>
#include <orbitforge/runtime/two_body.hpp>

namespace orbitforge::desktop {
namespace {
class ElementSpinBox final : public QDoubleSpinBox {
  public:
    QString textFromValue(double value) const override {
        auto display_locale = locale();
        display_locale.setNumberOptions(display_locale.numberOptions() |
                                        QLocale::OmitGroupSeparator);
        auto text = display_locale.toString(value, 'f', decimals());
        if (text.contains(display_locale.decimalPoint())) {
            while (text.endsWith('0'))
                text.chop(1);
            if (text.endsWith(display_locale.decimalPoint()))
                text.chop(1);
        }
        return text == "-0" ? "0" : text;
    }
};
double number(const QLineEdit *field) {
    bool ok = false;
    const double value = field->text().toDouble(&ok);
    if (!ok || !std::isfinite(value))
        throw std::invalid_argument("请输入有限数值，支持科学计数法。");
    return value;
}
QWidget *with_unit(QWidget *field, const QString &unit) {
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(field, 1);
    auto *caption = new QLabel(unit);
    caption->setObjectName("fieldUnit");
    caption->setFixedWidth(28);
    layout->addWidget(caption);
    return row;
}
QFrame *divider() {
    auto *line = new QFrame;
    line->setObjectName("sectionDivider");
    line->setFixedHeight(1);
    return line;
}
} // namespace
OrbitEditor::OrbitEditor(const OrbitInput &input, QWidget *parent) : QWidget(parent) {
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *navigation = new QTreeWidget;
    navigation_ = navigation;
    navigation->setObjectName("propertyNavigation");
    navigation->setFixedWidth(172);
    navigation->setHeaderHidden(true);
    navigation->setIndentation(16);
    navigation->setUniformRowHeights(true);
    navigation->setIconSize(QSize(17, 17));
    const auto category = [&](const QString &heading) {
        auto *group = new QTreeWidgetItem(navigation, {heading});
        group->setFlags(Qt::ItemIsEnabled);
        auto font = group->font(0);
        font.setWeight(QFont::DemiBold);
        group->setFont(0, font);
        group->setExpanded(true);
        return group;
    };
    const auto page = [](QTreeWidgetItem *group, const QString &name, int index) {
        auto *item = new QTreeWidgetItem(group, {name});
        item->setData(0, Qt::UserRole, index);
    };
    auto *basic = category(QStringLiteral("基本"));
    page(basic, QStringLiteral("轨道"), 0);
    page(basic, QStringLiteral("轨迹范围"), 1);
    page(category(QStringLiteral("二维视图")), QStringLiteral("显示"), 2);
    page(category(QStringLiteral("三维视图")), QStringLiteral("显示与模型"), 3);
    auto *tabs = new QStackedWidget;
    // Hidden pages must not force the whole document to their preferred height.
    // The orbit/model fields already provide their own scroll areas.
    tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    outer->addWidget(navigation);
    outer->addWidget(tabs, 1);
    connect(navigation, &QTreeWidget::currentItemChanged, tabs, [tabs](QTreeWidgetItem *item) {
        if (item && item->parent())
            tabs->setCurrentIndex(item->data(0, Qt::UserRole).toInt());
    });
    auto *orbit_tab = new QWidget;
    tabs->addWidget(orbit_tab);
    auto *layout = new QVBoxLayout(orbit_tab);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(14);
    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("轨道参数"));
    title->setObjectName("panelTitle");
    header->addWidget(title);
    header->addStretch();
    layout->addLayout(header);
    auto *identity = new QHBoxLayout;
    identity->setSpacing(12);
    identity->addWidget(new QLabel(QStringLiteral("对象 ID")));
    object_ = new QLineEdit;
    object_->setAccessibleName(QStringLiteral("卫星对象 ID"));
    object_->setMaximumWidth(250);
    identity->addWidget(object_);
    identity->addStretch();
    auto *body = new QLabel(QStringLiteral("中心天体  /  地球"));
    body->setObjectName("hint");
    identity->addWidget(body);
    layout->addLayout(identity);
    layout->addWidget(divider());
    fields_ = new QWidget;
    fields_->setObjectName("propertyFields");
    auto *columns = new QHBoxLayout(fields_);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(28);
    auto *settings = new QWidget;
    auto *initial_state = new QWidget;
    settings->setMinimumWidth(304);
    initial_state->setMinimumWidth(330);
    columns->addWidget(settings, 1, Qt::AlignTop);
    columns->addWidget(initial_state, 1, Qt::AlignTop);
    auto *form = new QFormLayout(settings);
    auto *state_form = new QFormLayout(initial_state);
    state_form->setContentsMargins(0, 0, 0, 0);
    state_form->setVerticalSpacing(10);
    state_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *state_title = new QLabel(QStringLiteral("初始状态"));
    state_title->setObjectName("sectionTitle");
    state_form->addRow(state_title);
    form->setContentsMargins(0, 0, 0, 0);
    form->setVerticalSpacing(10);
    form->setHorizontalSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto section = [&](const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("sectionTitle");
        form->addRow(label);
    };
    section(QStringLiteral("传播设置"));
    form->addRow(QStringLiteral("传播器"), new QLabel(QStringLiteral("二体 · Two Body")));
    form->addRow(QStringLiteral("参考系"), new QLabel(QStringLiteral("地心 GCRF")));
    epoch_ = new EpochEditor;
    form->addRow(new QLabel(QStringLiteral("初始历元")));
    form->addRow(epoch_);
    state_mode_ = new QComboBox;
    state_mode_->setObjectName("stateRepresentation");
    state_mode_->addItems(
        {QStringLiteral("位置 / 速度 · Cartesian"), QStringLiteral("经典轨道根数")});
    state_form->addRow(state_mode_);
    state_pages_ = new QStackedWidget;
    auto *cartesian_page = new QWidget;
    auto *cartesian_form = new QFormLayout(cartesian_page);
    cartesian_form->setContentsMargins(0, 0, 0, 0);
    state_pages_->addWidget(cartesian_page);
    const auto vector_row = [&](const QString &label, const QString &unit, auto &fields) {
        auto *caption = new QLabel(label);
        caption->setObjectName("muted");
        cartesian_form->addRow(caption);
        for (std::size_t i = 0; i < 3; ++i) {
            const auto axis = QString(QChar(static_cast<char>('X' + i)));
            fields[i] = new QLineEdit;
            fields[i]->setMinimumWidth(0);
            fields[i]->setObjectName("vectorInput");
            fields[i]->setAccessibleName(label + " " + axis);
            cartesian_form->addRow(axis, with_unit(fields[i], unit));
        }
    };
    vector_row(QStringLiteral("位置"), "km", position_);
    vector_row(QStringLiteral("速度"), "km/s", velocity_);
    auto *keplerian_page = new QWidget;
    auto *keplerian_form = new QFormLayout(keplerian_page);
    keplerian_form->setContentsMargins(0, 0, 0, 0);
    keplerian_form->setVerticalSpacing(10);
    const QStringList labels{QStringLiteral("半长轴 a"),     QStringLiteral("偏心率 e"),
                             QStringLiteral("倾角 i"),       QStringLiteral("升交点赤经 Ω"),
                             QStringLiteral("近地点幅角 ω"), QStringLiteral("真近点角 ν")};
    for (std::size_t i = 0; i < 6; ++i) {
        auto *field = new ElementSpinBox;
        field->setDecimals(i == 1 ? 12 : 9);
        field->setRange(i == 0   ? -1e9
                        : i == 5 ? -360
                                 : 0,
                        i == 0   ? 1e9
                        : i == 1 ? 1000
                        : i == 2 ? 180
                                 : 360);
        field->setSingleStep(i == 0 ? 10 : i == 1 ? 0.001 : 1);
        field->setAccessibleName(labels[static_cast<int>(i)]);
        elements_[i] = field;
        keplerian_form->addRow(labels[static_cast<int>(i)],
                               with_unit(field, i == 0   ? "km"
                                                : i == 1 ? ""
                                                         : QStringLiteral("°")));
    }
    state_mode_->setToolTip(QStringLiteral("GCRF 瞬时根数；抛物线轨道使用位置 / 速度。"));
    state_pages_->addWidget(keplerian_page);
    state_form->addRow(state_pages_);
    connect(state_mode_, &QComboBox::currentIndexChanged, this,
            [this](int mode) { set_state_mode(mode); });
    const auto time_row = [&](const QString &label, EpochEditor *&field) {
        field = new EpochEditor;
        field->set_read_only(true);
        auto *caption = new QLabel(label);
        caption->setObjectName("hint");
        form->addRow(caption);
        form->addRow(field);
    };
    section(QStringLiteral("所属场景时段"));
    time_row(QStringLiteral("起点"), first_);
    time_row(QStringLiteral("终点"), last_);
    first_->setToolTip(QStringLiteral("分析时段由所属 Scenario 统一管理"));
    last_->setToolTip(first_->toolTip());
    step_ = new QSpinBox;
    step_->setObjectName("displayStep");
    step_->setRange(1, 60);
    form->addRow(QStringLiteral("显示步长"), with_unit(step_, "s"));
    auto *advanced = new QToolButton;
    advanced->setText(QStringLiteral("更多参数"));
    advanced->setCheckable(true);
    advanced->setArrowType(Qt::RightArrow);
    advanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto *advanced_fields = new QWidget;
    auto *advanced_form = new QFormLayout(advanced_fields);
    advanced_form->setContentsMargins(0, 0, 0, 0);
    mu_ = new QLineEdit;
    advanced_form->addRow(QStringLiteral("μ / km³/s²"), mu_);
    advanced_fields->hide();
    connect(advanced, &QToolButton::toggled, advanced_fields,
            [advanced, advanced_fields](bool open) {
                advanced_fields->setVisible(open);
                advanced->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            });
    form->addRow(advanced);
    form->addRow(advanced_fields);
    form->setSizeConstraint(QLayout::SetMinAndMaxSize);
    auto *parameters = new QScrollArea;
    parameters->setWidgetResizable(true);
    parameters->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    parameters->setWidget(fields_);
    layout->addWidget(parameters, 1);
    layout->addWidget(divider());
    message_ = new QLabel;
    message_->setObjectName("orbitResult");
    message_->setWordWrap(true);
    message_->setTextFormat(Qt::PlainText);
    message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(message_);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    apply_ = new QPushButton(QStringLiteral("应用轨道"));
    apply_->setToolTip(QStringLiteral("根据当前参数重新计算并更新场景"));
    apply_->setObjectName("applyOrbit");
    cancel_ = new QPushButton(QStringLiteral("取消计算"));
    cancel_->setEnabled(false);
    cancel_->hide();
    buttons->addWidget(cancel_);
    auto *close = new QPushButton(QStringLiteral("关闭"));
    close->setAutoDefault(false);
    connect(close, &QPushButton::clicked, this, [this] {
        if (close_requested)
            close_requested();
    });
    buttons->addWidget(close);
    buttons->addWidget(apply_);
    layout->addLayout(buttons);
    connect(apply_, &QPushButton::clicked, this, [this] { submit(); });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (cancel)
            cancel();
    });
    const auto display_page = [&](const QString &heading) {
        auto *page = new QWidget;
        tabs->addWidget(page);
        auto *stack = new QVBoxLayout(page);
        stack->setContentsMargins(24, 22, 24, 18);
        stack->setSpacing(14);
        auto *title = new QLabel(heading);
        title->setObjectName("panelTitle");
        stack->addWidget(title);
        return stack;
    };
    const auto apply_display = [&](QVBoxLayout *stack) {
        auto *apply = new QPushButton(QStringLiteral("应用显示设置"));
        apply->setObjectName("applyDisplay");
        display_buttons_.push_back(apply);
        connect(apply, &QPushButton::clicked, this, [this] { submit_display(); });
        stack->addStretch();
        stack->addWidget(divider());
        stack->addWidget(apply, 0, Qt::AlignRight);
    };
    auto *common = display_page(QStringLiteral("轨迹范围"));
    auto *common_form = new QFormLayout;
    common_form->setContentsMargins(0, 0, 0, 0);
    track_extent_ = new QComboBox;
    track_extent_->setObjectName("trackExtent");
    track_extent_->addItems({QStringLiteral("一圈轨迹"), QStringLiteral("整个场景时段"),
                             QStringLiteral("自定义前后时长")});
    common_form->addRow(QStringLiteral("显示范围"), track_extent_);
    track_extent_->setToolTip(QStringLiteral("同时用于二维与三维；非闭合轨道的一圈按一小时显示。"));
    const auto duration = [&] {
        auto *field = new QDoubleSpinBox;
        field->setButtonSymbols(QAbstractSpinBox::NoButtons);
        field->setRange(0, 10080);
        field->setDecimals(1);
        field->setSuffix(QStringLiteral(" 分钟"));
        field->setValue(45);
        return field;
    };
    past_ = duration();
    future_ = duration();
    common_form->addRow(QStringLiteral("当前时刻之前"), past_);
    common_form->addRow(QStringLiteral("当前时刻之后"), future_);
    connect(track_extent_, &QComboBox::currentIndexChanged, this, [this, common_form](int index) {
        common_form->setRowVisible(past_, index == 2);
        common_form->setRowVisible(future_, index == 2);
    });
    common_form->setRowVisible(past_, false);
    common_form->setRowVisible(future_, false);
    auto *range_fields = new QWidget;
    range_fields->setObjectName("propertyFields");
    range_fields->setLayout(common_form);
    auto *range_scroll = new QScrollArea;
    range_scroll->setWidgetResizable(true);
    range_scroll->setWidget(range_fields);
    common->addWidget(range_scroll, 1);
    apply_display(common);
    auto *map = display_page(QStringLiteral("二维视图设置"));
    track_2d_ = new QCheckBox(QStringLiteral("显示地面轨迹"));
    label_2d_ = new QCheckBox(QStringLiteral("显示对象名称"));
    track_2d_->setChecked(true);
    label_2d_->setChecked(true);
    map->addWidget(track_2d_);
    map->addWidget(label_2d_);
    apply_display(map);
    auto *appearance_tab = new QWidget;
    tabs->addWidget(appearance_tab);
    auto *appearance_layout = new QVBoxLayout(appearance_tab);
    appearance_layout->setContentsMargins(24, 22, 24, 18);
    appearance_layout->setSpacing(14);
    auto *appearance_title = new QLabel(QStringLiteral("三维视图设置"));
    appearance_title->setObjectName("panelTitle");
    appearance_layout->addWidget(appearance_title);
    auto *appearance_fields = new QWidget;
    appearance_fields->setObjectName("propertyFields");
    auto *appearance_content = new QVBoxLayout(appearance_fields);
    appearance_content->setContentsMargins(0, 0, 0, 0);
    appearance_content->setSpacing(14);
    track_3d_ = new QCheckBox(QStringLiteral("显示空间轨迹"));
    track_3d_->setChecked(true);
    appearance_content->addWidget(track_3d_);
    auto *apply_track = new QPushButton(QStringLiteral("应用显示设置"));
    apply_track->setObjectName("applyDisplay");
    display_buttons_.push_back(apply_track);
    connect(apply_track, &QPushButton::clicked, this, [this] { submit_display(); });
    appearance_content->addWidget(apply_track, 0, Qt::AlignRight);
    appearance_content->addWidget(divider());
    model_fields_ = new QWidget;
    auto *form_model = new QFormLayout(model_fields_);
    form_model->setContentsMargins(0, 0, 0, 0);
    form_model->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    model_kind_ = new QComboBox;
    model_kind_->addItems({QStringLiteral("点标记（默认）"), QStringLiteral("内置卫星模型"),
                           QStringLiteral("本地三维模型")});
    form_model->addRow(QStringLiteral("显示方式"), model_kind_);
    model_path_ = new QLineEdit;
    model_path_->setPlaceholderText(QStringLiteral(".glb / .gltf / .vsgb / .vsgt"));
    auto *browse = new QPushButton(QStringLiteral("选择模型文件…"));
    form_model->addRow(QStringLiteral("模型文件"), model_path_);
    form_model->addRow(QString{}, browse);
    model_diameter_ = new QDoubleSpinBox;
    model_diameter_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    model_diameter_->setDecimals(3);
    model_diameter_->setRange(0.001, 10000);
    model_diameter_->setValue(1000);
    model_diameter_->setSuffix(" km");
    form_model->addRow(QStringLiteral("示意显示直径"), model_diameter_);
    appearance_content->addWidget(model_fields_);
    model_diameter_->setToolTip(QStringLiteral("模型示意尺寸；朝向固定于 ITRF。"));
    appearance_content->addStretch();
    auto *appearance_scroll = new QScrollArea;
    appearance_scroll->setWidgetResizable(true);
    appearance_scroll->setWidget(appearance_fields);
    appearance_layout->addWidget(appearance_scroll, 1);
    apply_model_ = new QPushButton(QStringLiteral("应用外观"));
    apply_model_->setObjectName("applyOrbit");
    appearance_layout->addWidget(divider());
    model_message_ = new QLabel;
    model_message_->hide();
    model_message_->setWordWrap(true);
    model_message_->setTextFormat(Qt::PlainText);
    appearance_layout->addWidget(model_message_);
    auto *appearance_buttons = new QHBoxLayout;
    appearance_buttons->addStretch();
    auto *close_appearance = new QPushButton(QStringLiteral("关闭"));
    close_appearance->setAutoDefault(false);
    connect(close_appearance, &QPushButton::clicked, this, [this] {
        if (close_requested)
            close_requested();
    });
    appearance_buttons->addWidget(close_appearance);
    appearance_buttons->addWidget(apply_model_);
    appearance_layout->addLayout(appearance_buttons);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("选择卫星模型"), {},
                                                       "3D models (*.glb *.gltf *.vsgb *.vsgt)");
        if (!path.isEmpty()) {
            model_path_->setText(path);
            model_kind_->setCurrentIndex(2);
        }
    });
    connect(model_kind_, &QComboBox::currentIndexChanged, this,
            [this, browse, form_model](int kind) {
                model_path_->setEnabled(kind == 2);
                browse->setEnabled(kind == 2);
                model_diameter_->setEnabled(kind != 0);
                form_model->setRowVisible(model_path_, kind == 2);
                form_model->setRowVisible(browse, kind == 2);
                form_model->setRowVisible(model_diameter_, kind != 0);
            });
    model_path_->setEnabled(false);
    browse->setEnabled(false);
    model_diameter_->setEnabled(false);
    form_model->setRowVisible(model_path_, false);
    form_model->setRowVisible(browse, false);
    form_model->setRowVisible(model_diameter_, false);
    connect(apply_model_, &QPushButton::clicked, this, [this] {
        if (appearance)
            appearance(model_kind_->currentIndex(), model_path_->text(), model_diameter_->value());
    });
    navigation->setCurrentItem(navigation->topLevelItem(0)->child(0));
    set_input(input);
}
void OrbitEditor::set_input(const OrbitInput &input) {
    object_->setText(QString::fromStdString(input.object_id));
    for (std::size_t k = 0; k < 3; ++k) {
        position_[k]->setText(QString::number(input.initial.position_m[k] / 1000, 'g', 15));
        velocity_[k]->setText(QString::number(input.initial.velocity_mps[k] / 1000, 'g', 15));
    }
    mu_->setText(QString::number(input.mu / 1e9, 'g', 15));
    epoch_->set_epoch(input.initial.epoch);
    first_->set_epoch(input.first);
    last_->set_epoch(input.last);
    step_->setValue(input.step_seconds);
    const QSignalBlocker block(state_mode_);
    state_mode_index_ = 0;
    try {
        write_elements(input.initial);
        state_mode_index_ = 1;
    } catch (const std::exception &) { /* Valid parabolic states retain Cartesian input. */
    }
    state_mode_->setCurrentIndex(state_mode_index_);
    state_pages_->setCurrentIndex(state_mode_index_);
}
core::CartesianState OrbitEditor::read_state() const {
    const core::GravitationalParameter mu{number(mu_) * 1e9};
    if (state_mode_index_ == 1) {
        const double rad = std::numbers::pi / 180;
        return runtime::to_cartesian({epoch_->epoch(), elements_[0]->value() * 1000,
                                      elements_[1]->value(), elements_[2]->value() * rad,
                                      elements_[3]->value() * rad, elements_[4]->value() * rad,
                                      elements_[5]->value() * rad},
                                     mu);
    }
    core::CartesianState state{epoch_->epoch(), core::OriginId::earth, core::FrameId::gcrf, {}, {}};
    for (std::size_t k = 0; k < 3; ++k) {
        state.position_m[k] = number(position_[k]) * 1000;
        state.velocity_mps[k] = number(velocity_[k]) * 1000;
    }
    return state;
}
void OrbitEditor::write_elements(const core::CartesianState &state) {
    const auto elements =
        runtime::to_keplerian(state, core::GravitationalParameter{number(mu_) * 1e9});
    const double deg = 180 / std::numbers::pi;
    const std::array<double, 6> values{
        elements.semi_major_axis_m / 1000,        elements.eccentricity,
        elements.inclination_rad * deg,           elements.raan_rad * deg,
        elements.argument_of_periapsis_rad * deg, elements.true_anomaly_rad * deg};
    for (std::size_t k = 0; k < 6; ++k)
        if (values[k] < elements_[k]->minimum() || values[k] > elements_[k]->maximum())
            throw std::out_of_range("轨道根数超出当前编辑器范围，请保留 Cartesian 状态。");
    for (std::size_t k = 0; k < 6; ++k)
        elements_[k]->setValue(values[k]);
}
void OrbitEditor::set_state_mode(int mode) {
    if (mode == state_mode_index_)
        return;
    try {
        const auto state = read_state();
        if (mode == 1)
            write_elements(state);
        else
            for (std::size_t k = 0; k < 3; ++k) {
                position_[k]->setText(QString::number(state.position_m[k] / 1000, 'g', 15));
                velocity_[k]->setText(QString::number(state.velocity_mps[k] / 1000, 'g', 15));
            }
        state_mode_index_ = mode;
        const QSignalBlocker block(state_mode_);
        state_mode_->setCurrentIndex(mode);
        state_pages_->setCurrentIndex(mode);
        show_result(QStringLiteral("已转换状态表示，点击应用后更新对象。"));
    } catch (const std::exception &exception) {
        const QSignalBlocker block(state_mode_);
        state_mode_->setCurrentIndex(state_mode_index_);
        show_result(QString::fromUtf8(exception.what()), true);
    }
}
void OrbitEditor::set_create_mode(bool create) {
    create_mode_ = create;
    object_->setReadOnly(!create);
    apply_->setText(create ? QStringLiteral("插入卫星") : QStringLiteral("应用轨道"));
    navigation_->setCurrentItem(navigation_->topLevelItem(0)->child(0));
    for (int i = 0; i < navigation_->topLevelItemCount(); ++i)
        for (int j = 0; j < navigation_->topLevelItem(i)->childCount(); ++j) {
            auto *item = navigation_->topLevelItem(i)->child(j);
            if (item->data(0, Qt::UserRole).toInt() == 0)
                continue;
            item->setDisabled(create);
            item->setToolTip(0, create ? QStringLiteral("插入卫星后可配置") : QString{});
        }
    refresh_enabled();
}
void OrbitEditor::submit() {
    if (!apply_->isEnabled())
        return;
    try {
        OrbitInput input;
        input.object_id = object_->text().trimmed().toStdString();
        input.mu = number(mu_) * 1e9;
        input.initial = read_state();
        input.first = first_->epoch();
        input.last = last_->epoch();
        input.step_seconds = step_->value();
        if (apply)
            apply(std::move(input));
    } catch (const std::exception &error) {
        show_result(QString::fromUtf8(error.what()), true);
    }
}
void OrbitEditor::set_busy(bool busy) {
    orbit_busy_ = busy;
    refresh_enabled();
    cancel_->setEnabled(busy);
    cancel_->setVisible(busy);
    if (busy)
        show_result(QStringLiteral("正在计算…"));
}
void OrbitEditor::show_result(const QString &message, bool error) {
    message_->setStyleSheet(error ? "color: #e6a197; font-size: 11px"
                                  : "color: #74899c; font-size: 11px");
    message_->setText(message);
    message_->setVisible(!message.isEmpty());
}
void OrbitEditor::configure_appearance(int kind, const QString &path, double diameter_km) {
    set_appearance(kind, path, diameter_km);
    apply_model_->click();
}
void OrbitEditor::set_appearance(int kind, const QString &path, double diameter_km) {
    model_kind_->setCurrentIndex(kind);
    model_path_->setText(path);
    model_diameter_->setValue(diameter_km);
}
void OrbitEditor::set_appearance_busy(bool busy) {
    model_busy_ = busy;
    refresh_enabled();
    if (busy)
        show_appearance_result(QStringLiteral("正在准备三维模型…"));
}
void OrbitEditor::refresh_enabled() {
    for (auto *button : display_buttons_)
        button->setEnabled(!create_mode_ && !orbit_busy_ && !model_busy_);
    const bool idle = !orbit_busy_ && !model_busy_;
    object_->setEnabled(idle);
    fields_->setEnabled(idle);
    apply_->setEnabled(idle);
    model_fields_->setEnabled(idle && !create_mode_);
    apply_model_->setEnabled(idle && !create_mode_);
}
void OrbitEditor::show_appearance_result(const QString &message, bool error) {
    model_message_->setStyleSheet(error ? "color: #e6a197; font-size: 11px"
                                        : "color: #74899c; font-size: 11px");
    model_message_->setText(message);
    model_message_->setVisible(!message.isEmpty());
}
void OrbitEditor::set_display(const DisplaySettings &settings) {
    track_extent_->setCurrentIndex(settings.extent);
    past_->setValue(settings.past_minutes);
    future_->setValue(settings.future_minutes);
    track_2d_->setChecked(settings.track_2d);
    label_2d_->setChecked(settings.labels_2d);
    track_3d_->setChecked(settings.track_3d);
}
void OrbitEditor::submit_display() {
    if (display)
        display({track_extent_->currentIndex(), past_->value(), future_->value(),
                 track_2d_->isChecked(), label_2d_->isChecked(), track_3d_->isChecked()});
}
} // namespace orbitforge::desktop
