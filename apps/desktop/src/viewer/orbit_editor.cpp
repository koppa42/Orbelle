#include "orbit_editor.hpp"
#include "../ui_text.hpp"
#include "epoch_editor.hpp"
#include "form_kit.hpp"
#include "hpop_settings.hpp"
#include "mission_editor.hpp"
#include "project_update.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "satellite_creation.hpp"
#include "track_style_editor.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>
#include <orbelle/runtime/j2.hpp>
#include <orbelle/runtime/j4.hpp>

namespace orbelle::desktop {
namespace {
// Result lines use the themed muted/danger tones from the application stylesheet.
void restyle_message(QLabel *label, bool error) {
    label->setProperty("error", error);
    label->style()->unpolish(label);
    label->style()->polish(label);
}
void reveal_message(QLabel *label) {
    // Result text scrolls with the form so long errors cannot push actions out of view.
    for (auto *parent = label->parentWidget(); parent; parent = parent->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(parent)) {
            scroll->ensureWidgetVisible(label, 0, 0);
            break;
        }
}
constexpr std::array element_units{Unit::metre,  Unit::one,    Unit::radian,
                                   Unit::radian, Unit::radian, Unit::radian};
class StatePages final : public QStackedWidget {
  public:
    StatePages() {
        connect(this, &QStackedWidget::currentChanged, this, [this] { updateGeometry(); });
    }
    QSize sizeHint() const override {
        return currentWidget() ? currentWidget()->sizeHint() : QStackedWidget::sizeHint();
    }
    QSize minimumSizeHint() const override {
        return currentWidget() ? currentWidget()->minimumSizeHint()
                               : QStackedWidget::minimumSizeHint();
    }
};
QFrame *divider() {
    auto *line = new QFrame;
    line->setObjectName("sectionDivider");
    line->setFixedHeight(1);
    return line;
}
QScrollArea *scrolling_content(QVBoxLayout *layout, QWidget *content) {
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    // Reserve space for the page footer even when the workspace is very short.
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    return scroll;
}
} // namespace
void OrbitEditor::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    const bool compact = height() < 300;
    for (auto *layout : page_layouts_) {
        layout->setContentsMargins(compact ? QMargins{24, 8, 24, 8} : theme::space::page);
        layout->setSpacing(compact ? 8 : 12);
    }
}
void OrbitEditor::set_time_provider(std::shared_ptr<const core::ITimeProvider> time) {
    time_ = time;
    for (auto *field : {epoch_, first_, last_})
        field->set_time_provider(time);
    update_tle_preview();
}
OrbitEditor::OrbitEditor(const OrbitInput &input, QWidget *parent) : QWidget(parent) {
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // Page navigation adapts to the width: a tree beside wide pages, a row of tabs in an
    // inspector-width pane, and a selector when even tabs would not fit.
    auto *compact_bar = new QWidget(this);
    compact_bar->setObjectName("propertyCompactBar");
    auto *compact_layout = new QHBoxLayout(compact_bar);
    compact_layout->setContentsMargins(24, 12, 24, 0);
    compact_navigation_ = new QComboBox(compact_bar);
    compact_navigation_->setObjectName("propertyPageSelector");
    compact_navigation_->setAccessibleName(
        ui::text("orbit_editor.compact_navigation.set_accessible_name"));
    compact_navigation_->addItems({ui::text("orbit_editor.label"), ui::text("orbit_editor.label_2"),
                                   ui::text("workspace.view_2d"),
                                   ui::text("orbit_editor.label_3")});
    compact_layout->addWidget(compact_navigation_, 1);
    page_tabs_ = new QTabBar(compact_bar);
    page_tabs_->setObjectName("propertyPageTabs");
    page_tabs_->setAccessibleName(ui::text("orbit_editor.compact_navigation.set_accessible_name"));
    page_tabs_->setDrawBase(false);
    page_tabs_->setExpanding(false);
    page_tabs_->setUsesScrollButtons(true);
    for (const auto &[label, tip] :
         {std::pair{ui::text("orbit_editor.label"), ui::text("orbit_editor.label_4")},
          std::pair{ui::text("orbit_editor.label_5"), ui::text("orbit_editor.label_2")},
          std::pair{ui::text("orbit_editor.label_6"), ui::text("workspace.view_2d")},
          std::pair{ui::text("orbit_editor.label_7"), ui::text("orbit_editor.label_3")}})
        page_tabs_->setTabToolTip(page_tabs_->addTab(label), tip);
    compact_layout->addWidget(page_tabs_);
    compact_layout->addStretch();
    outer->addWidget(compact_bar);
    connect(page_tabs_, &QTabBar::currentChanged, compact_navigation_, &QComboBox::setCurrentIndex);
    connect(compact_navigation_, &QComboBox::currentIndexChanged, page_tabs_, [this](int index) {
        const QSignalBlocker block(page_tabs_);
        page_tabs_->setCurrentIndex(index);
    });
    auto *navigation = new QTreeWidget;
    navigation_ = navigation;
    navigation->setObjectName("propertyNavigation");
    navigation->setFixedWidth(164);
    navigation->setHeaderHidden(true);
    navigation->setRootIsDecorated(false);
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
        const std::array icons{WorkbenchIcon::orbit, WorkbenchIcon::properties, WorkbenchIcon::map,
                               WorkbenchIcon::satellite};
        item->setIcon(0, workbench_icon(icons.at(static_cast<std::size_t>(index))));
        item->setData(0, Qt::UserRole, index);
    };
    auto *basic = category(ui::text("orbit_editor.basic"));
    page(basic, ui::text("orbit_editor.label"), 0);
    page(basic, ui::text("orbit_editor.label_2"), 1);
    page(category(ui::text("workspace.view_2d")), ui::text("orbit_editor.label_8"), 2);
    page(category(ui::text("workspace.view_3d")), ui::text("orbit_editor.label_9"), 3);
    auto *tabs = new QStackedWidget;
    // Hidden pages must not force the whole document to their preferred height.
    // The orbit/model fields already provide their own scroll areas.
    tabs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    outer->addWidget(navigation);
    outer->addWidget(tabs, 1);
    navigation->hide();
    outer->setDirection(QBoxLayout::TopToBottom);
    connect(navigation, &QTreeWidget::currentItemChanged, tabs, [tabs](QTreeWidgetItem *item) {
        if (item && item->parent())
            tabs->setCurrentIndex(item->data(0, Qt::UserRole).toInt());
    });
    connect(tabs, &QStackedWidget::currentChanged, compact_navigation_,
            &QComboBox::setCurrentIndex);
    connect(compact_navigation_, &QComboBox::currentIndexChanged, navigation,
            [navigation](int index) {
                for (int i = 0; i < navigation->topLevelItemCount(); ++i)
                    for (int j = 0; j < navigation->topLevelItem(i)->childCount(); ++j) {
                        auto *item = navigation->topLevelItem(i)->child(j);
                        if (item->data(0, Qt::UserRole).toInt() == index)
                            navigation->setCurrentItem(item);
                    }
            });
    new WidthObserver(this, [this, outer, compact_bar, mode = WidthMode{},
                             tight = WidthMode{}](int width) mutable {
        const bool narrow = mode.narrow(width, 680);
        const bool selector = tight.narrow(width, 400);
        navigation_->setVisible(!narrow);
        compact_bar->setVisible(narrow);
        compact_navigation_->setVisible(narrow && selector);
        page_tabs_->setVisible(narrow && !selector);
        outer->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    });
    auto *orbit_tab = new QWidget;
    tabs->addWidget(orbit_tab);
    auto *layout = new QVBoxLayout(orbit_tab);
    page_layouts_.push_back(layout);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(12);
    auto *header = new QHBoxLayout;
    auto *title = new QLabel(ui::text("orbit_editor.label_4"));
    title->setObjectName("panelTitle");
    header->addWidget(title);
    auto *identity_row = new QWidget;
    identity_row->setMaximumWidth(350);
    auto *identity = new QHBoxLayout(identity_row);
    identity->setContentsMargins(0, 0, 0, 0);
    identity->setSpacing(10);
    auto *identity_label = new QLabel(ui::text("orbit_editor.identity_label"));
    identity_label->setObjectName("muted");
    identity_label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    identity->addWidget(identity_label);
    object_ = new QLineEdit;
    object_->setObjectName("objectName");
    object_->setMaxLength(16384);
    object_->setAccessibleName(ui::text("orbit_editor.object.set_accessible_name"));
    object_->setMaximumWidth(250);
    object_->setMinimumWidth(60);
    identity->addWidget(object_, 1);
    header->addSpacing(8);
    // Width-limited rows are centred by default once the header stacks vertically.
    header->addWidget(identity_row, 0, Qt::AlignLeft);
    header->addStretch();
    auto *body = new QLabel(ui::text("orbit_editor.body"));
    body->setObjectName("hint");
    header->addWidget(body);
    auto *orbit_content = new QWidget;
    auto *orbit_content_layout = new QVBoxLayout(orbit_content);
    orbit_content_layout->setContentsMargins(0, 0, 0, 0);
    orbit_content_layout->setSpacing(12);
    orbit_content_layout->addLayout(header);
    responsive_columns(orbit_content, header, 480);
    new WidthObserver(orbit_tab, [body](int width) { body->setVisible(width >= 600); });
    // The first group's top rule separates the header; no extra divider here.
    fields_ = new QWidget;
    fields_->setObjectName("propertyFields");
    fields_->setMaximumWidth(900);
    auto *columns = new QHBoxLayout(fields_);
    // Room for the overlay scroll bar so it never covers field edges.
    columns->setContentsMargins(0, 0, 10, 0);
    columns->setSpacing(24);
    // Left: propagation settings and the read-only scenario span; right: the initial state.
    auto *settings = new QWidget;
    auto *settings_layout = new QVBoxLayout(settings);
    settings_layout->setContentsMargins(0, 0, 0, 0);
    settings_layout->setSpacing(0);
    auto *propagation_group = new FormGroup(ui::text("orbit_editor.propagation_group"));
    auto *span_group = new FormGroup(ui::text("orbit_editor.span_group"));
    settings_layout->addWidget(propagation_group);
    settings_layout->addWidget(span_group);
    auto *state_group = new FormGroup(ui::text("orbit_editor.state_group"));
    columns->addWidget(settings, 1, Qt::AlignTop);
    columns->addWidget(state_group, 1, Qt::AlignTop);
    auto *form = propagation_group->form();
    auto *span_form = span_group->form();
    auto *state_form = state_group->form();
    state_title_ = state_group->title();
    auto *propagator_row = new QHBoxLayout;
    propagator_row->setSpacing(6);
    propagator_ = new QComboBox;
    propagator_->setObjectName("orbitPropagator");
    propagator_->setAccessibleName(ui::text("orbit_editor.propagator.set_accessible_name"));
    propagator_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    propagator_->setMinimumContentsLength(8);
    propagator_->addItems({ui::text("orbit_editor.label_10"), ui::text("orbit_editor.label_11"),
                           ui::text("orbit_editor.label_12"), ui::text("propagator.sgp4_tle"),
                           ui::text("orbit_editor.label_13"), ui::text("orbit_editor.label_14")});
    propagator_row->addWidget(propagator_, 1);
    auto *model_button = new QPushButton(ui::text("orbit_editor.model_button"));
    model_button_ = model_button;
    model_button->setObjectName("propagatorDetails");
    model_button->setToolTip(ui::text("orbit_editor.model_button.set_tool_tip"));
    model_button->setAutoDefault(false);
    propagator_row->addWidget(model_button, 0, Qt::AlignRight);
    form->addRow(ui::text("orbit_editor.form.add_row"), propagator_row);
    hpop_summary_ = new QLabel;
    hpop_summary_->setObjectName("hint");
    hpop_summary_->setWordWrap(true);
    form->addRow(hpop_summary_);
    connect(model_button, &QPushButton::clicked, this, &OrbitEditor::open_propagator_details);
    connect(propagator_, &QComboBox::currentIndexChanged, this,
            [this](int index) { set_propagator(static_cast<OrbitPropagator>(index)); });
    epoch_ = new EpochEditor;
    epoch_label_ = new QLabel(ui::text("orbit_editor.label_15"));
    form->addRow(epoch_label_, epoch_);
    reference_frame_ = new QComboBox;
    reference_frame_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    reference_frame_->setMinimumContentsLength(8);
    reference_frame_->setObjectName("initialReferenceFrame");
    reference_frame_->setAccessibleName(
        ui::text("orbit_editor.reference_frame.set_accessible_name"));
    reference_frame_->addItems(
        {ui::text("orbit_editor.label_16"), ui::text("orbit_editor.label_17")});
    reference_frame_->setToolTip(ui::text("orbit_editor.reference_frame.set_tool_tip"));
    form->insertRow(1, ui::text("orbit_editor.label_18"), reference_frame_);
    connect(reference_frame_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (propagator_kind_ == OrbitPropagator::sgp4)
            return;
        if (index == 2) {
            const QSignalBlocker block(reference_frame_);
            reference_frame_->setCurrentIndex(input_frame_ == core::FrameId::gcrf ? 0 : 1);
            return;
        }
        set_reference_frame(index == 0 ? core::FrameId::gcrf : core::FrameId::itrf);
    });
    state_mode_ = new QComboBox;
    state_mode_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    state_mode_->setMinimumContentsLength(8);
    state_mode_->setObjectName("stateRepresentation");
    state_mode_->addItems({ui::text("orbit_editor.label_19"), ui::text("orbit_editor.label_20")});
    state_form->addRow(state_mode_);
    state_pages_ = new StatePages;
    auto *cartesian_page = new QWidget;
    auto *cartesian_form = new QFormLayout(cartesian_page);
    cartesian_form->setContentsMargins(0, 0, 0, 0);
    cartesian_form->setHorizontalSpacing(theme::space::md);
    cartesian_form->setVerticalSpacing(theme::space::row);
    cartesian_form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
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
            fields[i]->setMaximumWidth(QWIDGETSIZE_MAX);
            cartesian_form->addRow(axis, fields[i]);
        }
    };
    vector_row(ui::text("common.position"), Unit::kilometre, "position", position_);
    vector_row(ui::text("orbit_editor.label_21"), Unit::kilometres_per_second, "velocity",
               velocity_);
    auto *keplerian_page = new QWidget;
    auto *keplerian_form = new QFormLayout(keplerian_page);
    keplerian_form->setContentsMargins(0, 0, 0, 0);
    keplerian_form->setHorizontalSpacing(theme::space::md);
    keplerian_form->setVerticalSpacing(theme::space::row);
    keplerian_form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const QStringList labels{ui::text("orbit_editor.label_22"), ui::text("orbit_editor.label_23"),
                             ui::text("orbit_editor.label_24"), ui::text("orbit_editor.label_25"),
                             ui::text("orbit_editor.label_26"), ui::text("orbit_editor.label_27")};
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
        // Fields fill their column so every right edge in the column lines up.
        field->setMaximumWidth(QWIDGETSIZE_MAX);
        elements_[i] = field;
        keplerian_form->addRow(labels[static_cast<int>(i)], field);
        if (i == 5)
            anomaly_label_ = qobject_cast<QLabel *>(keplerian_form->labelForField(field));
    }
    state_mode_->setToolTip(ui::text("orbit_editor.state_mode.set_tool_tip"));
    state_pages_->addWidget(keplerian_page);
    auto *tle_page = new QWidget;
    auto *tle_layout = new QVBoxLayout(tle_page);
    tle_layout->setContentsMargins(0, 0, 0, 0);
    tle_layout->setSpacing(12);
    auto *tle_hint = new QLabel(ui::text("orbit_editor.tle_hint"));
    tle_hint->setObjectName("hint");
    tle_hint->setWordWrap(true);
    tle_layout->addWidget(tle_hint);
    tle_text_ = new QPlainTextEdit;
    tle_text_->setObjectName("tleInput");
    tle_text_->setAccessibleName(ui::text("orbit_editor.tle_text.set_accessible_name"));
    tle_text_->setPlaceholderText(ui::utf8("orbit.tle_placeholder"));
    tle_text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    tle_text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    tle_text_->setMinimumHeight(92);
    tle_text_->setMaximumHeight(112);
    tle_text_->setStyleSheet("font-family: Consolas; font-size: 12px;");
    tle_layout->addWidget(tle_text_);
    auto *import_tle = new QPushButton(ui::text("orbit_editor.import_tle"));
    import_tle->setObjectName("importTle");
    import_tle->setAutoDefault(false);
    tle_layout->addWidget(import_tle, 0, Qt::AlignLeft);
    tle_preview_ = new QLabel;
    tle_preview_->setObjectName("tlePreview");
    tle_preview_->setWordWrap(true);
    tle_preview_->setTextFormat(Qt::PlainText);
    tle_preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tle_layout->addWidget(tle_preview_);
    tle_layout->addStretch();
    connect(tle_text_, &QPlainTextEdit::textChanged, this, &OrbitEditor::update_tle_preview);
    connect(import_tle, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, ui::text("orbit_editor.path"), {},
                                                       ui::text("orbit_editor.path_2"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) {
            show_result(ui::text("orbit_editor.label_28"), true);
            return;
        }
        const auto previous = tle_text_->toPlainText();
        tle_text_->setPlainText(QString::fromUtf8(file.readAll()));
        try {
            (void)core::parse_tle(read_tle());
        } catch (const std::exception &error) {
            tle_text_->setPlainText(previous);
            show_result(QString::fromUtf8(error.what()), true);
        }
    });
    state_pages_->addWidget(tle_page);
    state_form->addRow(state_pages_);
    connect(state_mode_, &QComboBox::currentIndexChanged, this,
            [this](int mode) { set_state_mode(mode); });
    const auto time_row = [&](const QString &label, EpochEditor *&field) {
        field = new EpochEditor;
        field->set_read_only(true);
        auto *caption = new QLabel(label);
        caption->setObjectName("hint");
        span_form->addRow(caption, field);
    };
    // The display step is the propagation output sampling, so it belongs with the
    // propagation settings; the scenario span below is read-only context.
    step_ = new QuantityEditor(Unit::second);
    step_->setObjectName("displayStep");
    step_->set_range(1, 60, Unit::second);
    step_->set_quantum(1, Unit::second);
    step_->set_label(ui::text("orbit_editor.step.set_label"));
    step_->setToolTip(ui::text("orbit_editor.step.help"));
    step_->setMaximumWidth(QWIDGETSIZE_MAX);
    form->addRow(ui::text("orbit_editor.step.set_label"), step_);
    span_group->add_help(ui::text("orbit_editor.span_group.add_help"));
    time_row(ui::text("orbit_editor.label_29"), first_);
    time_row(ui::text("orbit_editor.label_30"), last_);
    first_->setToolTip(ui::text("orbit_editor.first.set_tool_tip"));
    last_->setToolTip(first_->toolTip());
    orbit_content_layout->addWidget(fields_);
    orbit_content_layout->addStretch();
    auto *parameters = scrolling_content(layout, orbit_content);
    parameters->setObjectName("orbitParametersScroll");
    responsive_columns(parameters->viewport(), columns, 720);
    // Labels stay beside fields down to inspector-column widths.
    responsive_form(form, 360);
    responsive_form(span_form, 360);
    responsive_form(keplerian_form, 350);
    align_form_labels(settings);
    layout->addWidget(divider());
    message_ = new QLabel;
    message_->setObjectName("orbitResult");
    message_->setWordWrap(true);
    message_->setTextFormat(Qt::PlainText);
    message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    message_->hide();
    orbit_content_layout->insertWidget(0, message_);
    auto *buttons = new QHBoxLayout;
    auto *reset = new QPushButton(ui::text("orbit_editor.reset"));
    reset_orbit_ = reset;
    reset->setObjectName("resetOrbit");
    reset->setToolTip(ui::text("orbit_editor.reset.set_tool_tip"));
    connect(reset, &QPushButton::clicked, this, [this] {
        if (!orbit_busy_ && !model_busy_) {
            set_input(bound_input_);
            show_result({});
        }
    });
    buttons->addWidget(reset);
    buttons->addStretch();
    apply_ = new QPushButton(ui::text("common.apply"));
    apply_->setToolTip(ui::text("orbit_editor.apply.set_tool_tip"));
    apply_->setAccessibleName(ui::text("orbit_editor.apply.set_accessible_name"));
    apply_->setObjectName("applyOrbit");
    cancel_ = new QPushButton(ui::text("orbit_editor.label_31"));
    cancel_->setEnabled(false);
    cancel_->hide();
    buttons->addWidget(cancel_);
    auto *close = new QPushButton(ui::text("common.close"));
    close->setAutoDefault(false);
    close->setToolTip(ui::text("orbit_editor.close.set_tool_tip"));
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
    const auto display_page = [&](const QString &heading, const QString &description) {
        auto *page = new QWidget;
        tabs->addWidget(page);
        auto *stack = new QVBoxLayout(page);
        page_layouts_.push_back(stack);
        stack->setContentsMargins(theme::space::page);
        stack->setSpacing(12);
        auto *content = new QWidget;
        auto *fields = new QVBoxLayout(content);
        fields->setContentsMargins(0, 0, 0, 0);
        fields->setSpacing(12);
        auto *title = new QLabel(heading);
        title->setObjectName("panelTitle");
        fields->addWidget(title);
        auto *caption = new QLabel(description);
        caption->setObjectName("dialogDescription");
        caption->setWordWrap(true);
        fields->addWidget(caption);
        scrolling_content(stack, content);
        return std::pair{stack, fields};
    };
    const auto apply_display = [&](QVBoxLayout *stack, QVBoxLayout *fields) {
        auto *apply = new QPushButton(ui::text("common.apply"));
        apply->setObjectName("applyDisplay");
        apply->setToolTip(ui::text("orbit_editor.apply.set_tool_tip_2"));
        apply->setAccessibleName(ui::text("orbit_editor.apply.set_accessible_name_2"));
        apply->setProperty("primary", true);
        display_buttons_.push_back(apply);
        connect(apply, &QPushButton::clicked, this, [this] { submit_display(); });
        stack->addWidget(divider());
        auto *error = new QLabel;
        error->setObjectName("inputError");
        error->setWordWrap(true);
        error->setTextFormat(Qt::PlainText);
        error->hide();
        display_messages_.push_back(error);
        fields->insertWidget(0, error);
        // Same footer as the orbit and 3D pages: Close, then the page's single Apply.
        auto *buttons = new QHBoxLayout;
        auto *reset = new QPushButton(ui::text("orbit_editor.reset"));
        reset->setObjectName("resetDisplay");
        reset->setToolTip(ui::text("orbit_editor.reset.set_tool_tip_2"));
        display_buttons_.push_back(reset);
        connect(reset, &QPushButton::clicked, this, [this] { set_display(bound_display_); });
        buttons->addWidget(reset);
        buttons->addStretch();
        auto *close = new QPushButton(ui::text("common.close"));
        close->setAutoDefault(false);
        connect(close, &QPushButton::clicked, this, [this] {
            if (close_requested)
                close_requested();
        });
        buttons->addWidget(close);
        buttons->addWidget(apply);
        stack->addLayout(buttons);
    };
    auto [common, common_fields] =
        display_page(ui::text("orbit_editor.label_2"), ui::text("orbit_editor.common"));
    auto *style_group = new FormGroup(ui::text("orbit_editor.style_group"));
    auto *extent_group = new FormGroup(ui::text("orbit_editor.extent_group"));
    auto *common_form = extent_group->form();
    track_extent_ = new QComboBox;
    track_extent_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    track_extent_->setMinimumContentsLength(8);
    track_extent_->setObjectName("trackExtent");
    track_extent_->setMaximumWidth(276);
    track_extent_->addItems({ui::text("orbit_editor.label_32"), ui::text("orbit_editor.label_33"),
                             ui::text("orbit_editor.label_34")});
    common_form->addRow(ui::text("orbit_editor.common_form.add_row"), track_extent_);
    track_extent_->setAccessibleName(ui::text("orbit_editor.extent_group"));
    extent_group->add_help(ui::text("orbit_editor.extent_group.add_help"));
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
    past_->set_label(ui::text("orbit_editor.past.set_label"));
    future_->setObjectName("futureDuration");
    future_->set_label(ui::text("orbit_editor.future.set_label"));
    common_form->addRow(ui::text("orbit_editor.past.set_label"), past_);
    common_form->addRow(ui::text("orbit_editor.future.set_label"), future_);
    connect(track_extent_, &QComboBox::currentIndexChanged, this, [this, common_form](int index) {
        common_form->setRowVisible(past_, index == 2);
        common_form->setRowVisible(future_, index == 2);
    });
    common_form->setRowVisible(past_, false);
    common_form->setRowVisible(future_, false);
    auto *range_fields = new QWidget;
    range_fields->setObjectName("propertyFields");
    range_fields->setMaximumWidth(520);
    auto *range_layout = new QVBoxLayout(range_fields);
    range_layout->setContentsMargins(0, 0, 4, 0);
    range_layout->setSpacing(0);
    track_style_ = new TrackStyleEditor;
    style_group->form()->addRow(track_style_);
    range_layout->addWidget(style_group);
    range_layout->addWidget(extent_group);
    range_layout->addStretch();
    responsive_form(common_form, 400);
    align_form_labels(range_fields);
    common_fields->addWidget(range_fields);
    common_fields->addStretch();
    apply_display(common, common_fields);
    auto [map, map_fields] =
        display_page(ui::text("orbit_editor.map"), ui::text("orbit_editor.map_2"));
    auto *map_options = new FormGroup(ui::text("orbit_editor.map_options"));
    map_options->setMaximumWidth(480);
    track_2d_ = new QCheckBox(ui::text("orbit_editor.map_options_2"));
    label_2d_ = new QCheckBox(ui::text("orbit_editor.label_35"));
    for (auto *toggle : {track_2d_, label_2d_}) {
        toggle->setChecked(true);
        make_switch(toggle);
        map_options->form()->addRow(toggle);
    }
    map_fields->addWidget(map_options);
    map_fields->addStretch();
    apply_display(map, map_fields);
    auto *appearance_tab = new QWidget;
    tabs->addWidget(appearance_tab);
    auto *appearance_layout = new QVBoxLayout(appearance_tab);
    page_layouts_.push_back(appearance_layout);
    appearance_layout->setContentsMargins(theme::space::page);
    appearance_layout->setSpacing(12);
    auto *appearance_title = new QLabel(ui::text("orbit_editor.appearance_title"));
    appearance_title->setObjectName("panelTitle");
    auto *appearance_description = new QLabel(ui::text("orbit_editor.appearance_description"));
    appearance_description->setObjectName("dialogDescription");
    appearance_description->setWordWrap(true);
    auto *appearance_body = new QWidget;
    auto *appearance_body_layout = new QVBoxLayout(appearance_body);
    appearance_body_layout->setContentsMargins(0, 0, 0, 0);
    appearance_body_layout->setSpacing(12);
    appearance_body_layout->addWidget(appearance_title);
    appearance_body_layout->addWidget(appearance_description);
    auto *appearance_fields = new QWidget;
    appearance_fields->setObjectName("propertyFields");
    appearance_fields->setMaximumWidth(560);
    auto *appearance_content = new QVBoxLayout(appearance_fields);
    appearance_content->setContentsMargins(0, 0, 0, 0);
    appearance_content->setSpacing(0);
    // Two inspector groups share the page's single Apply in the footer.
    const auto group = [appearance_content](const QString &title) {
        auto *frame = new FormGroup(title);
        appearance_content->addWidget(frame);
        return frame->form();
    };
    auto *track_group = group(ui::text("orbit_editor.track_group"));
    track_3d_ = new QCheckBox(ui::text("orbit_editor.track_group_2"));
    track_3d_->setChecked(true);
    make_switch(track_3d_);
    track_group->addRow(track_3d_);
    auto *display_error = new QLabel;
    display_error->setObjectName("inputError");
    display_error->setWordWrap(true);
    display_error->setTextFormat(Qt::PlainText);
    display_error->hide();
    display_messages_.push_back(display_error);
    track_group->addRow(display_error);
    auto *model_group = group(ui::text("orbit_editor.model_group"));
    model_fields_ = new QWidget;
    auto *form_model = new QFormLayout(model_fields_);
    form_model->setContentsMargins(0, 0, 0, 0);
    form_model->setHorizontalSpacing(theme::space::md);
    form_model->setVerticalSpacing(theme::space::row);
    form_model->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form_model->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    model_kind_ = new QComboBox;
    model_kind_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    model_kind_->setMinimumContentsLength(8);
    model_kind_->setMaximumWidth(320);
    model_kind_->addItems({ui::text("orbit_editor.label_36"), ui::text("orbit_editor.label_37"),
                           ui::text("orbit_editor.label_38")});
    form_model->addRow(ui::text("orbit_editor.form_model.add_row"), model_kind_);
    model_path_ = new QLineEdit;
    model_path_->setMaximumWidth(440);
    model_path_->setPlaceholderText(QStringLiteral(".glb / .gltf / .vsgb / .vsgt"));
    auto *browse = new QPushButton(ui::text("orbit_editor.browse"));
    form_model->addRow(ui::text("orbit_editor.form_model.add_row_2"), model_path_);
    form_model->addRow(QString{}, browse);
    model_diameter_ = new QuantityEditor(Unit::kilometre);
    model_diameter_->setObjectName("modelDiameter");
    model_diameter_->set_label(ui::text("orbit_editor.model_diameter.set_label"));
    model_diameter_->set_range(0.001, 10000, Unit::kilometre);
    model_diameter_->set_single_step(1, Unit::kilometre);
    model_diameter_->set_value(1000, Unit::kilometre);
    form_model->addRow(ui::text("orbit_editor.model_diameter.set_label"), model_diameter_);
    model_group->addRow(model_fields_);
    model_diameter_->setToolTip(ui::text("orbit_editor.model_diameter.set_tool_tip"));
    appearance_content->addStretch();
    appearance_body_layout->addWidget(appearance_fields);
    appearance_body_layout->addStretch();
    scrolling_content(appearance_layout, appearance_body);
    responsive_form(form_model, 400);
    align_form_labels(appearance_fields);
    apply_model_ = new QPushButton(ui::text("common.apply"));
    apply_model_->setObjectName("applyAppearance");
    apply_model_->setToolTip(ui::text("orbit_editor.apply_model.set_tool_tip"));
    apply_model_->setAccessibleName(ui::text("orbit_editor.apply_model.set_accessible_name"));
    apply_model_->setProperty("primary", true);
    appearance_layout->addWidget(divider());
    model_message_ = new QLabel;
    model_message_->setObjectName("modelResult");
    model_message_->hide();
    model_message_->setWordWrap(true);
    model_message_->setTextFormat(Qt::PlainText);
    appearance_body_layout->insertWidget(0, model_message_);
    auto *appearance_buttons = new QHBoxLayout;
    auto *reset_model = new QPushButton(ui::text("orbit_editor.reset"));
    reset_model->setObjectName("resetAppearance");
    display_buttons_.push_back(reset_model);
    connect(reset_model, &QPushButton::clicked, this, [this] {
        set_appearance(applied_model_kind_, applied_model_path_, applied_model_diameter_km_);
        track_3d_->setChecked(bound_display_.track_3d);
        show_appearance_result({});
    });
    appearance_buttons->addWidget(reset_model);
    appearance_buttons->addStretch();
    auto *close_appearance = new QPushButton(ui::text("common.close"));
    close_appearance->setAutoDefault(false);
    connect(close_appearance, &QPushButton::clicked, this, [this] {
        if (close_requested)
            close_requested();
    });
    appearance_buttons->addWidget(close_appearance);
    appearance_buttons->addWidget(apply_model_);
    appearance_layout->addLayout(appearance_buttons);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, ui::text("orbit_editor.path_3"), {},
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
    connect(apply_model_, &QPushButton::clicked, this, &OrbitEditor::apply_appearance_page);
    navigation->setCurrentItem(navigation->topLevelItem(0)->child(0));
    set_input(input);
    set_display({});
    set_appearance(0, {}, 1000);
}
void OrbitEditor::set_input(const OrbitInput &input) {
    const auto name = bound_name_.empty() || bound_input_.object_id != input.object_id
                          ? input.object_id
                          : bound_name_;
    bound_input_ = input;
    remembered_display_.reset();
    const QSignalBlocker state_block(state_mode_), frame_block(reference_frame_),
        propagator_block(propagator_);
    body_ = input.initial.origin;
    propagator_kind_ = input.propagator;
    hpop_ = input.hpop;
    covariance_ = input.covariance;
    mission_ = input.mission;
    hpop_initialized_ = uses_hpop() || hpop_ != core::HpopOptions{};
    manual_state_ = input.initial;
    tle_text_->setPlainText(QString::fromStdString(
        input.tle.line1 + (input.tle.line1.empty() ? "" : "\n") + input.tle.line2));
    // Reopening a TLE must not transform the inactive manual draft using EOP at
    // its old epoch. Its canonical state is retained and shown in GCRF if restored.
    input_frame_ =
        input.propagator == OrbitPropagator::sgp4 ? core::FrameId::gcrf : input.input_frame;
    propagator_->setCurrentIndex(static_cast<int>(propagator_kind_));
    reference_frame_->setCurrentIndex(input_frame_ == core::FrameId::gcrf ? 0 : 1);
    set_name(name);
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
    update_state_labels();
    update_tle_preview();
    remember_state(input.initial);
}
void OrbitEditor::accept_input(const OrbitInput &input) {
    bound_input_ = input;
    if (propagator_kind_ != OrbitPropagator::sgp4)
        remember_state(input.initial);
}
std::string OrbitEditor::object_name() const {
    const auto name = object_->text().trimmed().toStdString();
    if (name.empty())
        throw std::invalid_argument(ui::utf8("common.object_name_required"));
    return name;
}
void OrbitEditor::set_name(const std::string &name) {
    bound_name_ = name;
    object_->setText(QString::fromStdString(name));
}
void OrbitEditor::sync_name(const std::string &name) {
    if (object_->text().trimmed().toStdString() == bound_name_)
        object_->setText(QString::fromStdString(name));
    bound_name_ = name;
}
void OrbitEditor::set_analysis_interval(core::Epoch first, core::Epoch last) {
    bound_input_.first = first;
    bound_input_.last = last;
    first_->set_epoch(first);
    last_->set_epoch(last);
}
bool OrbitEditor::has_unapplied_changes() const {
    try {
        return object_name() != bound_name_ || !same_orbit_input(read_input(), bound_input_) ||
               (!create_mode_ &&
                (read_display() != bound_display_ ||
                 model_kind_->currentIndex() != applied_model_kind_ ||
                 model_path_->text() != applied_model_path_ ||
                 model_diameter_->value(Unit::kilometre) != applied_model_diameter_km_));
    } catch (const std::exception &) {
        // An incomplete value is still a draft worth preserving.
        return true;
    }
}
OrbitEditor::StateDisplay OrbitEditor::state_display() const {
    StateDisplay signature{epoch_->epoch(), body_, input_frame_, state_mode_index_,
                           uses_hpop()
                               ? hpop_.gravity_mu
                               : core::central_body(body_).mu.metres_cubed_per_second_squared};
    if (state_mode_index_ == 1) {
        for (std::size_t k = 0; k < 6; ++k)
            signature.values[k] = elements_[k]->value(element_units[k]);
    } else {
        for (std::size_t k = 0; k < 3; ++k) {
            signature.values[k] = position_[k]->value(Unit::metre);
            signature.values[k + 3] = velocity_[k]->value(Unit::metres_per_second);
        }
    }
    return signature;
}
void OrbitEditor::remember_state(const core::CartesianState &state) const {
    remembered_display_ = state_display();
    remembered_state_ = state;
}
core::CartesianState OrbitEditor::read_state() const {
    if (propagator_kind_ == OrbitPropagator::sgp4)
        return manual_state_;
    // Displaying elements, changing units or switching frames must not round-trip
    // an unchanged physical state into a new orbit on Apply.
    if (remembered_display_ && *remembered_display_ == state_display())
        return remembered_state_;
    if (state_mode_index_ == 1) {
        if (propagator_kind_ == OrbitPropagator::j4_secular)
            return runtime::j4_mean_ellipse_state(
                {epoch_->epoch(), elements_[0]->value(Unit::metre), elements_[1]->value(Unit::one),
                 elements_[2]->value(Unit::radian), elements_[3]->value(Unit::radian),
                 elements_[4]->value(Unit::radian), elements_[5]->value(Unit::radian)});
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
            uses_hpop() ? core::GravitationalParameter{hpop_.gravity_mu}
                        : core::central_body(body_).mu);
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
        throw std::runtime_error(ui::utf8("orbit_editor.validation"));
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
    if (propagator_kind_ == OrbitPropagator::j2_secular ||
        propagator_kind_ == OrbitPropagator::j4_secular) {
        const auto elements = propagator_kind_ == OrbitPropagator::j4_secular
                                  ? runtime::j4_mean_ellipse_elements(state)
                                  : runtime::j2_mean_ellipse_elements(state);
        values = {elements.semi_major_axis_m,         elements.eccentricity,
                  elements.inclination_rad,           elements.raan_rad,
                  elements.argument_of_periapsis_rad, elements.mean_anomaly_rad};
    } else {
        const auto elements = runtime::to_keplerian(
            state, uses_hpop() ? core::GravitationalParameter{hpop_.gravity_mu}
                               : core::central_body(body_).mu);
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
    if (propagator_kind_ == OrbitPropagator::sgp4)
        return;
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
        remember_state(state);
        show_result(ui::text("orbit_editor.label_39"));
    } catch (const std::exception &exception) {
        const QSignalBlocker block(state_mode_);
        state_mode_->setCurrentIndex(state_mode_index_);
        show_result(QString::fromUtf8(exception.what()), true);
    }
}
void OrbitEditor::set_reference_frame(core::FrameId frame) {
    if (propagator_kind_ == OrbitPropagator::sgp4)
        return;
    if (frame == input_frame_)
        return;
    const QSignalBlocker frame_block(reference_frame_), mode_block(state_mode_);
    try {
        if (frame != core::FrameId::gcrf && frame != core::FrameId::itrf)
            throw std::invalid_argument("Unsupported initial-state reference frame");
        const auto state = read_state();
        write_cartesian(in_frame(state, frame));
        input_frame_ = frame;
        state_mode_index_ = 0;
        state_mode_->setCurrentIndex(0);
        state_pages_->setCurrentIndex(0);
        remember_state(state);
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
        auto state = manual_state_;
        if (propagator == OrbitPropagator::sgp4) {
            try {
                state = read_state();
            } catch (const std::exception &) {
                // An incomplete manual form must not block independent TLE input.
            }
        } else {
            state = read_state();
        }
        if (propagator == OrbitPropagator::j2_secular)
            (void)runtime::j2_mean_ellipse_elements(state);
        else if (propagator == OrbitPropagator::j4_secular)
            (void)runtime::j4_mean_ellipse_elements(state);
        if (propagator == OrbitPropagator::sgp4)
            manual_state_ = state;
        propagator_kind_ = propagator;
        if (propagator == OrbitPropagator::hpop && !hpop_initialized_) {
            hpop_ = new_hpop_options();
            hpop_initialized_ = true;
        }
        if (propagator == OrbitPropagator::mission && mission_.segments.empty()) {
            const double seconds = last_->epoch().seconds_since(state.epoch);
            if (seconds > 0 && seconds <= core::hpop_max_duration_seconds)
                mission_.segments.push_back(core::MissionSegment::coast(std::chrono::nanoseconds{
                    last_->epoch().tai_nanoseconds() - state.epoch.tai_nanoseconds()}));
        }
        if (state_mode_index_ == 1 && propagator != OrbitPropagator::sgp4)
            write_elements(state);
        if (propagator != OrbitPropagator::sgp4) {
            try {
                remember_state(state);
            } catch (const std::exception &) {
                // Returning from TLE input may restore an unfinished manual draft.
                // Keep it editable; normal submission still validates every field.
                remembered_display_.reset();
            }
        }
        update_state_labels();
        show_result(propagator == OrbitPropagator::sgp4       ? ui::text("orbit_editor.label_40")
                    : propagator == OrbitPropagator::two_body ? ui::text("orbit_editor.label_41")
                    : propagator == OrbitPropagator::hpop     ? ui::text("orbit_editor.label_42")
                    : propagator == OrbitPropagator::mission  ? ui::text("orbit_editor.label_43")
                                                              : ui::text("orbit_editor.label_44"));
    } catch (const std::exception &error) {
        propagator_kind_ = previous;
        show_result(QString::fromUtf8(error.what()), true);
    }
    propagator_->setCurrentIndex(static_cast<int>(propagator_kind_));
}
void OrbitEditor::update_state_labels() {
    const bool hpop = propagator_kind_ == OrbitPropagator::hpop;
    model_button_->setText(hpop ? ui::text("orbit_editor.model_button.set_text")
                                : ui::text("orbit_editor.model_button"));
    model_button_->setToolTip(hpop ? ui::text("orbit_editor.model_button.set_tool_tip_2")
                                   : ui::text("orbit_editor.model_button.set_tool_tip"));
    hpop_summary_->setVisible(hpop);
    hpop_summary_->setText(
        (hpop_.gravity_degree == 0
             ? ui::text("orbit_editor.hpop_summary.set_text")
             : QStringLiteral("%1 %2 × %3")
                   .arg(hpop_.gravity_model.empty() ? ui::text("orbit_editor.label_45")
                                                    : QString::fromStdString(hpop_.gravity_model))
                   .arg(hpop_.gravity_degree)
                   .arg(hpop_.gravity_order)) +
        (hpop_.uses_third_bodies()
             ? ui::text("orbit_editor.label_46").arg(hpop_.enabled_third_bodies().size())
             : QString{}) +
        (hpop_.srp.enabled ? ui::text("orbit_editor.label_47") : QString{}) +
        (hpop_.drag.enabled ? ui::text("orbit_editor.label_48") : QString{}) +
        (hpop_.integrator == core::HpopIntegrator::rkf78
             ? QStringLiteral(" · ") + ui::text("integrator.rkf78")
         : hpop_.integrator == core::HpopIntegrator::rkv89
             ? QStringLiteral(" · ") + ui::text("integrator.rkv89")
         : hpop_.integrator == core::HpopIntegrator::bulirsch_stoer
             ? QStringLiteral(" · ") + ui::text("integrator.bulirsch_stoer")
         : hpop_.integrator == core::HpopIntegrator::gauss_jackson
             ? QStringLiteral(" · ") + ui::text("integrator.gauss_jackson")
             : QStringLiteral(" · ") + ui::text("integrator.rk4")) +
        (hpop_.uses_fixed_step() ? ui::text("orbit_editor.label_49")
                                 : ui::text("orbit_editor.label_50")));
    if (propagator_kind_ == OrbitPropagator::mission) {
        model_button_->setText(ui::text("orbit_editor.model_button.set_text_2"));
        model_button_->setToolTip(ui::text("orbit_editor.model_button.set_tool_tip_3"));
        hpop_summary_->setVisible(true);
        double total_delta_v = 0;
        for (const auto &segment : mission_.segments)
            if (segment.kind == core::MissionSegmentKind::impulse)
                total_delta_v += std::hypot(segment.delta_v_mps[0], segment.delta_v_mps[1],
                                            segment.delta_v_mps[2]);
        hpop_summary_->setText(
            ui::text("orbit_editor.hpop_summary.set_text_2")
                .arg(mission_.segments.size())
                .arg(uses_hpop() ? QStringLiteral("HPOP") : ui::text("orbit_editor.label_51"))
                .arg(total_delta_v, 0, 'g', 8));
    }
    const bool sgp4 = propagator_kind_ == OrbitPropagator::sgp4;
    const bool j2 = propagator_kind_ == OrbitPropagator::j2_secular ||
                    propagator_kind_ == OrbitPropagator::j4_secular;
    const auto model = propagator_kind_ == OrbitPropagator::j4_secular ? "J4" : "J2";
    state_mode_->setVisible(!sgp4);
    epoch_->setVisible(!sgp4);
    epoch_label_->setVisible(!sgp4);
    reference_frame_->setEnabled(!sgp4);
    const QSignalBlocker frame_block(reference_frame_);
    if (sgp4 && reference_frame_->count() == 2)
        reference_frame_->addItem(ui::text("orbit_editor.reference_frame.add_item"));
    else if (!sgp4 && reference_frame_->count() == 3)
        reference_frame_->removeItem(2);
    reference_frame_->setCurrentIndex(sgp4 ? 2 : input_frame_ == core::FrameId::gcrf ? 0 : 1);
    state_pages_->setCurrentIndex(sgp4 ? 2 : state_mode_index_);
    state_title_->setText(sgp4 ? ui::text("orbit_editor.state_title.set_text")
                          : j2 ? ui::text("orbit_editor.state_title.set_text_2").arg(model)
                               : ui::text("orbit_editor.state_group"));
    state_mode_->setItemText(0, j2 ? ui::text("orbit_editor.state_mode.set_item_text")
                                   : ui::text("orbit_editor.label_19"));
    state_mode_->setItemText(1, j2 ? ui::text("orbit_editor.state_mode.set_item_text_2").arg(model)
                                   : ui::text("orbit_editor.state_mode.set_item_text_3"));
    state_mode_->setToolTip(j2 ? ui::text("orbit_editor.state_mode.set_tool_tip_2")
                               : ui::text("orbit_editor.state_mode.set_tool_tip"));
    anomaly_label_->setText(j2 ? ui::text("orbit_editor.anomaly_label.set_text")
                               : ui::text("orbit_editor.label_27"));
    elements_[5]->set_label(anomaly_label_->text());
    elements_[5]->setObjectName(j2 ? "meanAnomaly" : "trueAnomaly");
}
core::Tle OrbitEditor::read_tle() const { return parse_single_tle(tle_text_->toPlainText()); }
void OrbitEditor::update_tle_preview() {
    if (!tle_preview_)
        return;
    if (tle_text_->toPlainText().trimmed().isEmpty()) {
        tle_preview_->setText(ui::text("orbit_editor.tle_preview.set_text"));
        return;
    }
    try {
        const auto parsed = core::parse_tle(read_tle());
        const auto &utc = parsed.epoch_utc;
        const auto date = QDate(utc.year, utc.month, utc.day).toString("yyyy-MM-dd");
        const auto clock = QTime(utc.hour, utc.minute, utc.second).toString("HH:mm:ss") + "." +
                           QString::number(utc.nanosecond).rightJustified(9, '0');
        QString text = ui::text("orbit_editor.text_2")
                           .arg(QString::fromStdString(parsed.catalog_id), date, clock)
                           .arg(1440 / parsed.mean_motion_revolutions_per_day, 0, 'f', 3);
        if (time_) {
            const auto epoch = time_->from_utc(utc);
            const double age = std::max(std::abs(first_->epoch().seconds_since(epoch)),
                                        std::abs(last_->epoch().seconds_since(epoch))) /
                               86400;
            if (age > 14)
                text += ui::text("orbit_editor.label_52").arg(age, 0, 'f', 1);
        }
        tle_preview_->setText(text);
    } catch (const std::exception &error) {
        tle_preview_->setText(
            ui::text("orbit_editor.tle_preview.set_text_2").arg(QString::fromUtf8(error.what())));
    }
}
void OrbitEditor::open_propagator_details() {
    if (propagator_kind_ == OrbitPropagator::mission) {
        try {
            const auto state = read_state();
            if (edit_mission(mission_, hpop_, state.epoch, first_->epoch(), last_->epoch(), this,
                             gravity_models ? gravity_models()
                                            : std::vector<runtime::CatalogGravity>{},
                             ephemerides ? ephemerides() : std::vector<runtime::CatalogEphemeris>{},
                             manage_ephemerides, data_catalog, !hpop_initialized_)) {
                hpop_initialized_ =
                    hpop_initialized_ || uses_hpop() || hpop_ != core::HpopOptions{};
                if (state_mode_index_ == 1)
                    write_elements(state);
                remember_state(state);
                update_state_labels();
            }
        } catch (const std::exception &error) {
            show_result(QString::fromUtf8(error.what()), true);
        }
        return;
    }
    if (propagator_kind_ == OrbitPropagator::hpop) {
        try {
            const auto state = read_state();
            if (edit_hpop_settings(
                    hpop_, this,
                    gravity_models ? gravity_models() : std::vector<runtime::CatalogGravity>{},
                    ephemerides ? ephemerides() : std::vector<runtime::CatalogEphemeris>{},
                    manage_ephemerides, data_catalog, &covariance_)) {
                if (state_mode_index_ == 1)
                    write_elements(state);
                remember_state(state);
                update_state_labels();
            }
        } catch (const std::exception &error) {
            show_result(QString::fromUtf8(error.what()), true);
        }
        return;
    }
    try {
        QDialog dialog(this);
        dialog.setObjectName("propagatorDetailsDialog");
        const bool j4 = propagator_kind_ == OrbitPropagator::j4_secular;
        const bool sgp4 = propagator_kind_ == OrbitPropagator::sgp4;
        const bool j2 = propagator_kind_ == OrbitPropagator::j2_secular || j4;
        dialog.setWindowTitle(propagator_->currentText() + ui::text("orbit_editor.label_53"));
        dialog.setMinimumWidth(400);
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(theme::space::page);
        layout->setSpacing(12);
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(10);
        const auto row = [&](const QString &label, const QString &value) {
            auto *text = new QLabel(value);
            text->setTextFormat(Qt::PlainText);
            text->setTextInteractionFlags(Qt::TextSelectableByMouse);
            form->addRow(label, text);
        };
        const auto &body = j4 ? core::j4_earth() : core::central_body(body_);
        row(ui::text("orbit_editor.body_2"), sgp4 ? ui::text("orbit_editor.body_3")
                                             : j4 ? ui::text("orbit_editor.label_54")
                                             : j2 ? ui::text("orbit_editor.label_55")
                                                  : ui::text("orbit_editor.label_51"));
        row(ui::text("orbit_editor.label_56"), sgp4 ? ui::text("orbit_editor.label_57")
                                               : j4 ? ui::text("orbit_editor.label_58")
                                                    : ui::text("orbit_editor.label_59"));
        row(ui::text("orbit_editor.label_60"),
            QString::number(sgp4 ? 398600.8 : body.mu.metres_cubed_per_second_squared / 1e9, 'g',
                            15) +
                QStringLiteral(" km³/s²"));
        row(ui::text("orbit_editor.label_61"),
            sgp4 ? QStringLiteral("TEME → GCRF / ITRF") : QStringLiteral("GCRF"));
        if (sgp4) {
            row(ui::text("orbit_editor.label_62"), ui::text("orbit_editor.label_63"));
            row(ui::text("orbit_editor.label_64"), ui::text("orbit_editor.label_65"));
        }
        if (j2) {
            row(ui::text("orbit_editor.label_66"),
                QString::number(body.equatorial_radius_m / 1000, 'g', 15) + " km");
            row(ui::text("orbit_editor.label_67"), QString::number(body.j2, 'g', 15));
            if (j4)
                row(ui::text("orbit_editor.label_68"), QString::number(body.j4, 'g', 15));
            row(ui::text("orbit_editor.label_69"), ui::text("orbit_editor.label_70"));
        }
        layout->addLayout(form);
        layout->addWidget(divider());
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        buttons->button(QDialogButtonBox::Close)->setText(ui::text("common.close"));
        buttons->button(QDialogButtonBox::Close)->setDefault(true);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.exec();
    } catch (const std::exception &error) {
        show_result(QString::fromUtf8(error.what()), true);
    }
}
void OrbitEditor::set_create_mode(bool create) {
    const bool changed = create_mode_ != create;
    create_mode_ = create;
    apply_->setText(create ? ui::text("orbit_editor.apply.set_text") : ui::text("common.apply"));
    if (changed)
        navigation_->setCurrentItem(navigation_->topLevelItem(0)->child(0));
    compact_navigation_->setEnabled(!create);
    page_tabs_->setEnabled(!create);
    for (int i = 0; i < navigation_->topLevelItemCount(); ++i)
        for (int j = 0; j < navigation_->topLevelItem(i)->childCount(); ++j) {
            auto *item = navigation_->topLevelItem(i)->child(j);
            if (item->data(0, Qt::UserRole).toInt() == 0)
                continue;
            item->setDisabled(create);
            item->setToolTip(0, create ? ui::text("orbit_editor.item.set_tool_tip") : QString{});
        }
    refresh_enabled();
}
void OrbitEditor::submit() {
    if (!apply_->isEnabled())
        return;
    try {
        (void)object_name();
        if (apply)
            apply(read_input());
    } catch (const std::exception &error) {
        show_result(QString::fromUtf8(error.what()), true);
    }
}
OrbitInput OrbitEditor::read_input() const {
    OrbitInput input = bound_input_;
    input.initial = read_state();
    input.propagator = propagator_kind_;
    input.hpop = hpop_;
    input.covariance = covariance_;
    if (propagator_kind_ != OrbitPropagator::hpop)
        input.covariance.enabled = false;
    input.mission = mission_;
    if (propagator_kind_ == OrbitPropagator::sgp4) {
        input.tle = read_tle();
        const auto parsed = core::parse_tle(input.tle);
        if (!time_)
            throw std::invalid_argument(ui::utf8("orbit_editor.parsed"));
        (void)time_->from_utc(parsed.epoch_utc);
    } else if (bound_input_.propagator == OrbitPropagator::sgp4) {
        input.tle = {};
    }
    input.input_frame = input_frame_;
    input.first = first_->epoch();
    input.last = last_->epoch();
    input.step_seconds = static_cast<int>(step_->value(Unit::second));
    return input;
}
void OrbitEditor::set_busy(bool busy) {
    orbit_busy_ = busy;
    refresh_enabled();
    cancel_->setEnabled(busy);
    cancel_->setVisible(busy);
    if (busy) {
        show_result(ui::text("orbit_editor.label_71"));
        orbit_busy_message_ = true;
    } else if (orbit_busy_message_) {
        show_result({});
    }
}
void OrbitEditor::show_result(const QString &message, bool error) {
    // Only the pending hint belongs to set_busy; preserve a final result even
    // when it arrives before the controller restores the controls.
    orbit_busy_message_ = false;
    restyle_message(message_, error);
    message_->setText(message);
    message_->setVisible(!message.isEmpty());
    if (error && !message.isEmpty())
        reveal_message(message_);
}
void OrbitEditor::configure_appearance(int kind, const QString &path, double diameter_km) {
    model_kind_->setCurrentIndex(kind);
    model_path_->setText(path);
    model_diameter_->set_value(diameter_km, Unit::kilometre);
    apply_appearance_page(true);
}
void OrbitEditor::set_appearance(int kind, const QString &path, double diameter_km) {
    model_kind_->setCurrentIndex(kind);
    model_path_->setText(path);
    model_diameter_->set_value(diameter_km, Unit::kilometre);
    accept_appearance(kind, path, diameter_km);
}
void OrbitEditor::accept_appearance(int kind, const QString &path, double diameter_km) {
    applied_model_kind_ = kind;
    applied_model_path_ = path;
    applied_model_diameter_km_ = diameter_km;
}
// Applies the 3D page: the track switch through the display settings, and the model only
// when it changed, so an unchanged appearance does not rebuild the model.
void OrbitEditor::apply_appearance_page(bool force) {
    if (track_3d_->isChecked() != applied_track_3d_)
        submit_display();
    try {
        const int kind = model_kind_->currentIndex();
        const auto path = model_path_->text();
        const double diameter = model_diameter_->value(Unit::kilometre);
        if (!force && kind == applied_model_kind_ && path == applied_model_path_ &&
            diameter == applied_model_diameter_km_)
            return;
        if (appearance)
            appearance(kind, path, diameter);
    } catch (const std::exception &error) {
        show_appearance_result(QString::fromUtf8(error.what()), true);
    }
}
void OrbitEditor::set_appearance_busy(bool busy) {
    model_busy_ = busy;
    refresh_enabled();
    if (busy) {
        show_appearance_result(ui::text("orbit_editor.label_72"));
        model_busy_message_ = true;
    } else if (model_busy_message_) {
        show_appearance_result({});
    }
}
void OrbitEditor::refresh_enabled() {
    for (auto *button : display_buttons_)
        button->setEnabled(!create_mode_ && !orbit_busy_ && !model_busy_);
    const bool idle = !orbit_busy_ && !model_busy_;
    reset_orbit_->setEnabled(idle);
    object_->setEnabled(idle);
    fields_->setEnabled(idle);
    apply_->setEnabled(idle);
    model_fields_->setEnabled(idle && !create_mode_);
    apply_model_->setEnabled(idle && !create_mode_);
}
void OrbitEditor::show_appearance_result(const QString &message, bool error) {
    model_busy_message_ = false;
    restyle_message(model_message_, error);
    model_message_->setText(message);
    model_message_->setVisible(!message.isEmpty());
    if (error && !message.isEmpty())
        reveal_message(model_message_);
}
void OrbitEditor::set_display(const DisplaySettings &settings) {
    track_style_->set_settings(settings);
    track_extent_->setCurrentIndex(settings.extent);
    past_->set_value(settings.past_minutes, Unit::minute);
    future_->set_value(settings.future_minutes, Unit::minute);
    track_2d_->setChecked(settings.track_2d);
    label_2d_->setChecked(settings.labels_2d);
    track_3d_->setChecked(settings.track_3d);
    accept_display(settings);
}
void OrbitEditor::accept_display(const DisplaySettings &settings) {
    bound_display_ = settings;
    applied_track_3d_ = settings.track_3d;
}
DisplaySettings OrbitEditor::read_display() const {
    DisplaySettings settings{track_extent_->currentIndex(), past_->value(Unit::minute),
                             future_->value(Unit::minute),  track_2d_->isChecked(),
                             label_2d_->isChecked(),        track_3d_->isChecked()};
    track_style_->write_settings(settings);
    return settings;
}
void OrbitEditor::submit_display() {
    try {
        if (display) {
            display(read_display());
        }
        for (auto *error : display_messages_)
            error->hide();
    } catch (const std::exception &exception) {
        for (auto *error : display_messages_) {
            error->setText(QString::fromUtf8(exception.what()));
            error->show();
            reveal_message(error);
        }
    }
}
} // namespace orbelle::desktop
