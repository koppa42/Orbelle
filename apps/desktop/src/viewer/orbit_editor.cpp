#include "orbit_editor.hpp"
#include "epoch_editor.hpp"
#include "quantity_editor.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>
#include <orbitforge/runtime/j2.hpp>

namespace orbitforge::desktop {
namespace {
constexpr std::array element_units{Unit::metre,  Unit::one,    Unit::radian,
                                   Unit::radian, Unit::radian, Unit::radian};
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
    navigation->setFixedWidth(156);
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
    layout->setContentsMargins(20, 16, 20, 14);
    layout->setSpacing(12);
    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("轨道参数"));
    title->setObjectName("panelTitle");
    header->addWidget(title);
    header->addSpacing(24);
    auto *identity = header;
    identity->setSpacing(10);
    identity->addWidget(new QLabel(QStringLiteral("对象 ID")));
    object_ = new QLineEdit;
    object_->setObjectName("objectId");
    object_->setAccessibleName(QStringLiteral("卫星对象 ID"));
    object_->setMaximumWidth(250);
    identity->addWidget(object_);
    identity->addStretch();
    auto *body = new QLabel(QStringLiteral("中心天体  /  地球"));
    body->setObjectName("hint");
    identity->addWidget(body);
    layout->addLayout(header);
    layout->addWidget(divider());
    fields_ = new QWidget;
    fields_->setObjectName("propertyFields");
    fields_->setMaximumWidth(800);
    auto *columns = new QHBoxLayout(fields_);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(24);
    auto *settings = new QWidget;
    auto *initial_state = new QWidget;
    settings->setMinimumWidth(304);
    initial_state->setMinimumWidth(330);
    columns->addWidget(settings, 1, Qt::AlignTop);
    columns->addWidget(initial_state, 1, Qt::AlignTop);
    auto *form = new QFormLayout(settings);
    auto *state_form = new QFormLayout(initial_state);
    state_form->setContentsMargins(0, 0, 0, 0);
    state_form->setVerticalSpacing(8);
    state_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *state_title = new QLabel(QStringLiteral("初始状态"));
    state_title_ = state_title;
    state_title->setObjectName("sectionTitle");
    state_form->addRow(state_title);
    form->setContentsMargins(0, 0, 0, 0);
    form->setVerticalSpacing(8);
    form->setHorizontalSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto section = [&](const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("sectionTitle");
        form->addRow(label);
    };
    section(QStringLiteral("传播设置"));
    auto *propagator_row = new QHBoxLayout;
    propagator_row->setSpacing(6);
    propagator_ = new QComboBox;
    propagator_->setObjectName("orbitPropagator");
    propagator_->setAccessibleName(QStringLiteral("轨道传播器"));
    propagator_->addItems({QStringLiteral("二体 · Two Body"), QStringLiteral("J2 · 长期摄动")});
    propagator_row->addWidget(propagator_, 1);
    auto *model_button = new QPushButton(QStringLiteral("模型…"));
    model_button->setObjectName("propagatorDetails");
    model_button->setToolTip(QStringLiteral("查看传播模型与中心天体常数"));
    model_button->setAutoDefault(false);
    propagator_row->addWidget(model_button);
    form->addRow(QStringLiteral("传播器"), propagator_row);
    connect(model_button, &QPushButton::clicked, this, &OrbitEditor::open_propagator_details);
    connect(propagator_, &QComboBox::currentIndexChanged, this, [this](int index) {
        set_propagator(index == 0 ? OrbitPropagator::two_body : OrbitPropagator::j2_secular);
    });
    epoch_ = new EpochEditor;
    form->addRow(new QLabel(QStringLiteral("初始历元")));
    form->addRow(epoch_);
    reference_frame_ = new QComboBox;
    reference_frame_->setObjectName("initialReferenceFrame");
    reference_frame_->setAccessibleName(QStringLiteral("初始状态参考系"));
    reference_frame_->addItems({QStringLiteral("GCRF · 地心惯性"), QStringLiteral("ITRF · 地固")});
    reference_frame_->setToolTip(QStringLiteral("ITRF 使用位置 / 速度；轨道根数在 GCRF 下定义。"));
    form->insertRow(2, QStringLiteral("参考系"), reference_frame_);
    connect(reference_frame_, &QComboBox::currentIndexChanged, this, [this](int index) {
        set_reference_frame(index == 0 ? core::FrameId::gcrf : core::FrameId::itrf);
    });
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
    const auto vector_row = [&](const QString &label, Unit unit, const QString &name,
                                auto &fields) {
        auto *caption = new QLabel(label);
        caption->setObjectName("muted");
        cartesian_form->addRow(caption);
        for (std::size_t i = 0; i < 3; ++i) {
            const auto axis = QString(QChar(static_cast<char>('X' + i)));
            fields[i] = new QuantityEditor(unit);
            fields[i]->setObjectName(name + axis);
            fields[i]->set_label(label + " " + axis);
            fields[i]->set_single_step(1, unit);
            cartesian_form->addRow(axis, fields[i]);
        }
    };
    vector_row(QStringLiteral("位置"), Unit::kilometre, "position", position_);
    vector_row(QStringLiteral("速度"), Unit::kilometres_per_second, "velocity", velocity_);
    auto *keplerian_page = new QWidget;
    auto *keplerian_form = new QFormLayout(keplerian_page);
    keplerian_form->setContentsMargins(0, 0, 0, 0);
    keplerian_form->setVerticalSpacing(10);
    const QStringList labels{QStringLiteral("半长轴 a"),     QStringLiteral("偏心率 e"),
                             QStringLiteral("倾角 i"),       QStringLiteral("升交点赤经 Ω"),
                             QStringLiteral("近地点幅角 ω"), QStringLiteral("真近点角 ν")};
    const QStringList names{"semiMajorAxis", "eccentricity",        "inclination",
                            "raan",          "argumentOfPeriapsis", "trueAnomaly"};
    for (std::size_t i = 0; i < 6; ++i) {
        const auto unit = i == 0 ? Unit::kilometre : i == 1 ? Unit::one : Unit::degree;
        auto *field = new QuantityEditor(unit);
        field->set_range(i == 0   ? -1e9
                         : i == 5 ? -360
                                  : 0,
                         i == 0   ? 1e9
                         : i == 1 ? 1000
                         : i == 2 ? 180
                                  : 360,
                         unit);
        field->set_single_step(i == 0 ? 10 : i == 1 ? 0.001 : 1, unit);
        field->set_label(labels[static_cast<int>(i)]);
        field->setObjectName(names[static_cast<int>(i)]);
        elements_[i] = field;
        keplerian_form->addRow(labels[static_cast<int>(i)], field);
        if (i == 5)
            anomaly_label_ = qobject_cast<QLabel *>(keplerian_form->labelForField(field));
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
    step_ = new QuantityEditor(Unit::second);
    step_->setObjectName("displayStep");
    step_->set_range(1, 60, Unit::second);
    step_->set_quantum(1, Unit::second);
    step_->set_label(QStringLiteral("显示步长"));
    form->addRow(QStringLiteral("显示步长"), step_);
    form->setSizeConstraint(QLayout::SetMinAndMaxSize);
    auto *parameters = new QScrollArea;
    parameters->setWidgetResizable(true);
    parameters->setAlignment(Qt::AlignLeft | Qt::AlignTop);
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
        stack->setContentsMargins(20, 16, 20, 14);
        stack->setSpacing(12);
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
        auto *error = new QLabel;
        error->setObjectName("inputError");
        error->setWordWrap(true);
        error->setTextFormat(Qt::PlainText);
        error->hide();
        display_messages_.push_back(error);
        stack->addWidget(error);
        stack->addWidget(apply, 0, Qt::AlignRight);
    };
    auto *common = display_page(QStringLiteral("轨迹范围"));
    auto *common_form = new QFormLayout;
    common_form->setContentsMargins(0, 0, 0, 0);
    track_extent_ = new QComboBox;
    track_extent_->setObjectName("trackExtent");
    track_extent_->setMaximumWidth(320);
    track_extent_->addItems({QStringLiteral("一圈轨迹"), QStringLiteral("整个场景时段"),
                             QStringLiteral("自定义前后时长")});
    common_form->addRow(QStringLiteral("显示范围"), track_extent_);
    track_extent_->setToolTip(QStringLiteral("同时用于二维与三维；非闭合轨道的一圈按一小时显示。"));
    const auto duration = [&] {
        auto *field = new QuantityEditor(Unit::minute);
        field->set_range(0, 10080, Unit::minute);
        field->set_single_step(1, Unit::minute);
        field->set_value(45, Unit::minute);
        return field;
    };
    past_ = duration();
    future_ = duration();
    past_->setObjectName("pastDuration");
    past_->set_label(QStringLiteral("当前时刻之前"));
    future_->setObjectName("futureDuration");
    future_->set_label(QStringLiteral("当前时刻之后"));
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
    range_fields->setMaximumWidth(480);
    range_fields->setLayout(common_form);
    auto *range_scroll = new QScrollArea;
    range_scroll->setWidgetResizable(true);
    range_scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
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
    appearance_layout->setContentsMargins(20, 16, 20, 14);
    appearance_layout->setSpacing(12);
    auto *appearance_title = new QLabel(QStringLiteral("三维视图设置"));
    appearance_title->setObjectName("panelTitle");
    appearance_layout->addWidget(appearance_title);
    auto *appearance_fields = new QWidget;
    appearance_fields->setObjectName("propertyFields");
    appearance_fields->setMaximumWidth(560);
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
    auto *display_error = new QLabel;
    display_error->setObjectName("inputError");
    display_error->setWordWrap(true);
    display_error->setTextFormat(Qt::PlainText);
    display_error->hide();
    display_messages_.push_back(display_error);
    appearance_content->addWidget(display_error);
    appearance_content->addWidget(divider());
    model_fields_ = new QWidget;
    auto *form_model = new QFormLayout(model_fields_);
    form_model->setContentsMargins(0, 0, 0, 0);
    form_model->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    model_kind_ = new QComboBox;
    model_kind_->setMaximumWidth(320);
    model_kind_->addItems({QStringLiteral("点标记（默认）"), QStringLiteral("内置卫星模型"),
                           QStringLiteral("本地三维模型")});
    form_model->addRow(QStringLiteral("显示方式"), model_kind_);
    model_path_ = new QLineEdit;
    model_path_->setMaximumWidth(440);
    model_path_->setPlaceholderText(QStringLiteral(".glb / .gltf / .vsgb / .vsgt"));
    auto *browse = new QPushButton(QStringLiteral("选择模型文件…"));
    form_model->addRow(QStringLiteral("模型文件"), model_path_);
    form_model->addRow(QString{}, browse);
    model_diameter_ = new QuantityEditor(Unit::kilometre);
    model_diameter_->setObjectName("modelDiameter");
    model_diameter_->set_label(QStringLiteral("示意显示直径"));
    model_diameter_->set_range(0.001, 10000, Unit::kilometre);
    model_diameter_->set_single_step(1, Unit::kilometre);
    model_diameter_->set_value(1000, Unit::kilometre);
    form_model->addRow(QStringLiteral("示意显示直径"), model_diameter_);
    appearance_content->addWidget(model_fields_);
    model_diameter_->setToolTip(QStringLiteral("模型示意尺寸；朝向固定于 ITRF。"));
    appearance_content->addStretch();
    auto *appearance_scroll = new QScrollArea;
    appearance_scroll->setWidgetResizable(true);
    appearance_scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
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
        try {
            if (appearance)
                appearance(model_kind_->currentIndex(), model_path_->text(),
                           model_diameter_->value(Unit::kilometre));
        } catch (const std::exception &error) {
            show_appearance_result(QString::fromUtf8(error.what()), true);
        }
    });
    navigation->setCurrentItem(navigation->topLevelItem(0)->child(0));
    set_input(input);
}
void OrbitEditor::set_input(const OrbitInput &input) {
    const QSignalBlocker state_block(state_mode_), frame_block(reference_frame_),
        propagator_block(propagator_);
    body_ = input.initial.origin;
    propagator_kind_ = input.propagator;
    input_frame_ = input.input_frame;
    propagator_->setCurrentIndex(propagator_kind_ == OrbitPropagator::two_body ? 0 : 1);
    reference_frame_->setCurrentIndex(input_frame_ == core::FrameId::gcrf ? 0 : 1);
    update_state_labels();
    object_->setText(QString::fromStdString(input.object_id));
    epoch_->set_epoch(input.initial.epoch);
    first_->set_epoch(input.first);
    last_->set_epoch(input.last);
    step_->set_value(input.step_seconds, Unit::second);
    write_cartesian(in_frame(input.initial, input_frame_));
    state_mode_index_ = 0;
    if (input_frame_ == core::FrameId::gcrf)
        try {
            write_elements(input.initial);
            state_mode_index_ = 1;
        } catch (const std::exception &) { /* Parabolic states retain Cartesian input. */
        }
    state_mode_->setCurrentIndex(state_mode_index_);
    state_pages_->setCurrentIndex(state_mode_index_);
}
core::CartesianState OrbitEditor::read_state() const {
    if (state_mode_index_ == 1) {
        if (propagator_kind_ == OrbitPropagator::j2_secular)
            return runtime::j2_mean_ellipse_state(
                {epoch_->epoch(), elements_[0]->value(Unit::metre), elements_[1]->value(Unit::one),
                 elements_[2]->value(Unit::radian), elements_[3]->value(Unit::radian),
                 elements_[4]->value(Unit::radian), elements_[5]->value(Unit::radian)},
                body_);
        return runtime::to_cartesian(
            {epoch_->epoch(), elements_[0]->value(Unit::metre), elements_[1]->value(Unit::one),
             elements_[2]->value(Unit::radian), elements_[3]->value(Unit::radian),
             elements_[4]->value(Unit::radian), elements_[5]->value(Unit::radian)},
            core::central_body(body_).mu);
    }
    core::CartesianState state{epoch_->epoch(), body_, input_frame_, {}, {}};
    for (std::size_t k = 0; k < 3; ++k) {
        state.position_m[k] = position_[k]->value(Unit::metre);
        state.velocity_mps[k] = velocity_[k]->value(Unit::metres_per_second);
    }
    return in_frame(state, core::FrameId::gcrf);
}
core::CartesianState OrbitEditor::in_frame(core::CartesianState state, core::FrameId frame) const {
    if (state.frame == frame)
        return state;
    if (!transform_state)
        throw std::runtime_error("参考系转换需要场景的地球定向数据。");
    return transform_state(std::move(state), frame);
}
void OrbitEditor::write_cartesian(const core::CartesianState &state) {
    for (std::size_t k = 0; k < 3; ++k) {
        position_[k]->validate_value(state.position_m[k], Unit::metre);
        velocity_[k]->validate_value(state.velocity_mps[k], Unit::metres_per_second);
    }
    for (std::size_t k = 0; k < 3; ++k) {
        position_[k]->set_value(state.position_m[k], Unit::metre);
        velocity_[k]->set_value(state.velocity_mps[k], Unit::metres_per_second);
    }
}
void OrbitEditor::write_elements(const core::CartesianState &state) {
    std::array<double, 6> values;
    if (propagator_kind_ == OrbitPropagator::j2_secular) {
        const auto elements = runtime::j2_mean_ellipse_elements(state);
        values = {elements.semi_major_axis_m,         elements.eccentricity,
                  elements.inclination_rad,           elements.raan_rad,
                  elements.argument_of_periapsis_rad, elements.mean_anomaly_rad};
    } else {
        const auto elements = runtime::to_keplerian(state, core::central_body(body_).mu);
        values = {elements.semi_major_axis_m,         elements.eccentricity,
                  elements.inclination_rad,           elements.raan_rad,
                  elements.argument_of_periapsis_rad, elements.true_anomaly_rad};
    }
    for (std::size_t k = 0; k < 6; ++k)
        elements_[k]->validate_value(values[k], element_units[k]);
    for (std::size_t k = 0; k < 6; ++k)
        elements_[k]->set_value(values[k], element_units[k]);
}
void OrbitEditor::set_state_mode(int mode) {
    if (mode == state_mode_index_)
        return;
    try {
        const auto state = read_state();
        if (mode == 1)
            write_elements(state);
        else
            write_cartesian(in_frame(state, input_frame_));
        if (mode == 1) {
            const QSignalBlocker frame_block(reference_frame_);
            input_frame_ = core::FrameId::gcrf;
            reference_frame_->setCurrentIndex(0);
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
void OrbitEditor::set_reference_frame(core::FrameId frame) {
    if (frame == input_frame_)
        return;
    const QSignalBlocker frame_block(reference_frame_), mode_block(state_mode_);
    try {
        if (frame != core::FrameId::gcrf && frame != core::FrameId::itrf)
            throw std::invalid_argument("Unsupported initial-state reference frame");
        const auto state = in_frame(read_state(), frame);
        write_cartesian(state);
        input_frame_ = frame;
        state_mode_index_ = 0;
        state_mode_->setCurrentIndex(0);
        state_pages_->setCurrentIndex(0);
        show_result({});
    } catch (const std::exception &error) {
        show_result(QString::fromUtf8(error.what()), true);
    }
    reference_frame_->setCurrentIndex(input_frame_ == core::FrameId::gcrf ? 0 : 1);
}
void OrbitEditor::set_propagator(OrbitPropagator propagator) {
    if (propagator == propagator_kind_)
        return;
    const auto previous = propagator_kind_;
    const QSignalBlocker block(propagator_);
    try {
        const auto state = read_state();
        if (propagator == OrbitPropagator::j2_secular)
            (void)runtime::j2_mean_ellipse_elements(state);
        propagator_kind_ = propagator;
        if (state_mode_index_ == 1)
            write_elements(state);
        update_state_labels();
        show_result(propagator == OrbitPropagator::j2_secular
                        ? QStringLiteral("以当前椭圆定义 J2 平均根数，不含周期项转换。")
                        : QStringLiteral("以当前椭圆定义二体初始状态。"));
    } catch (const std::exception &error) {
        propagator_kind_ = previous;
        show_result(QString::fromUtf8(error.what()), true);
    }
    propagator_->setCurrentIndex(propagator_kind_ == OrbitPropagator::two_body ? 0 : 1);
}
void OrbitEditor::update_state_labels() {
    const bool j2 = propagator_kind_ == OrbitPropagator::j2_secular;
    state_title_->setText(j2 ? QStringLiteral("初始平均椭圆 · J2") : QStringLiteral("初始状态"));
    state_mode_->setItemText(0, j2 ? QStringLiteral("平均椭圆位置 / 速度")
                                   : QStringLiteral("位置 / 速度 · Cartesian"));
    state_mode_->setItemText(1, j2 ? QStringLiteral("J2 平均轨道根数")
                                   : QStringLiteral("经典轨道根数 · 瞬时"));
    state_mode_->setToolTip(j2 ? QStringLiteral("一阶长期项平均椭圆；位置/速度是辅助开普勒状态，"
                                                "不代表瞬时根数或传播轨迹的实际速度。")
                               : QStringLiteral("GCRF 瞬时根数；抛物线轨道使用位置 / 速度。"));
    anomaly_label_->setText(j2 ? QStringLiteral("平近点角 M") : QStringLiteral("真近点角 ν"));
    elements_[5]->set_label(anomaly_label_->text());
    elements_[5]->setObjectName(j2 ? "meanAnomaly" : "trueAnomaly");
}
void OrbitEditor::open_propagator_details() {
    try {
        QDialog dialog(this);
        dialog.setObjectName("propagatorDetailsDialog");
        const bool j2 = propagator_kind_ == OrbitPropagator::j2_secular;
        dialog.setWindowTitle(j2 ? QStringLiteral("J2 · 模型信息")
                                 : QStringLiteral("二体 · 模型信息"));
        dialog.setMinimumWidth(400);
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(20, 18, 20, 16);
        layout->setSpacing(14);
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(18);
        form->setVerticalSpacing(10);
        const auto row = [&](const QString &label, const QString &value) {
            auto *text = new QLabel(value);
            text->setTextFormat(Qt::PlainText);
            text->setTextInteractionFlags(Qt::TextSelectableByMouse);
            form->addRow(label, text);
        };
        const auto &body = core::central_body(body_);
        row(QStringLiteral("传播模型"),
            j2 ? QStringLiteral("一阶 J2 长期项") : QStringLiteral("二体"));
        row(QStringLiteral("中心天体"), QStringLiteral("地球 · IERS 2003"));
        row(QStringLiteral("引力参数 μ"),
            QString::number(body.mu.metres_cubed_per_second_squared / 1e9, 'g', 15) +
                QStringLiteral(" km³/s²"));
        row(QStringLiteral("传播参考系"), QStringLiteral("GCRF"));
        if (j2) {
            row(QStringLiteral("参考半径"),
                QString::number(body.equatorial_radius_m / 1000, 'g', 15) + " km");
            row(QStringLiteral("J2 系数"), QString::number(body.j2, 'g', 15));
            row(QStringLiteral("长期项"), QStringLiteral("Ω、ω、M（全部计入）"));
        }
        layout->addLayout(form);
        layout->addWidget(divider());
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        buttons->button(QDialogButtonBox::Close)->setDefault(true);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.exec();
    } catch (const std::exception &error) {
        show_result(QString::fromUtf8(error.what()), true);
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
        input.initial = read_state();
        input.propagator = propagator_kind_;
        input.input_frame = input_frame_;
        input.first = first_->epoch();
        input.last = last_->epoch();
        input.step_seconds = static_cast<int>(step_->value(Unit::second));
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
    model_diameter_->set_value(diameter_km, Unit::kilometre);
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
    past_->set_value(settings.past_minutes, Unit::minute);
    future_->set_value(settings.future_minutes, Unit::minute);
    track_2d_->setChecked(settings.track_2d);
    label_2d_->setChecked(settings.labels_2d);
    track_3d_->setChecked(settings.track_3d);
}
void OrbitEditor::submit_display() {
    try {
        if (display)
            display({track_extent_->currentIndex(), past_->value(Unit::minute),
                     future_->value(Unit::minute), track_2d_->isChecked(), label_2d_->isChecked(),
                     track_3d_->isChecked()});
        for (auto *error : display_messages_)
            error->hide();
    } catch (const std::exception &exception) {
        for (auto *error : display_messages_) {
            error->setText(QString::fromUtf8(exception.what()));
            error->show();
        }
    }
}
} // namespace orbitforge::desktop
