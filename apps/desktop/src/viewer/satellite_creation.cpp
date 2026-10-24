#include "satellite_creation.hpp"
#include "../ui_text.hpp"
#include "form_kit.hpp"
#include "orbit_design_preview.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QComboBox>
#include <QFile>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <orbelle/runtime/two_body.hpp>
#include <stdexcept>

namespace orbelle::desktop {
core::Tle parse_single_tle(const QString &text) {
    if (text.size() > 4096)
        throw std::invalid_argument(ui::utf8("orbit_editor.text"));
    auto lines = text.split('\n', Qt::SkipEmptyParts);
    for (auto &line : lines)
        line = line.trimmed();
    lines.removeAll({});
    if (!lines.isEmpty() && lines.front().startsWith(QChar(0xfeff)))
        lines.front().remove(0, 1);
    if (lines.size() == 3 && !lines[0].startsWith("1 ") && !lines[0].startsWith("2 "))
        lines.removeFirst();
    if (lines.size() != 2)
        throw std::invalid_argument(ui::utf8("orbit_editor.validation_2"));
    core::Tle tle{lines[0].toStdString(), lines[1].toStdString()};
    (void)core::parse_tle(tle);
    return tle;
}
core::Tle read_single_tle_file(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096)
        throw std::invalid_argument(ui::utf8("orbit_editor.label_28"));
    return parse_single_tle(QString::fromUtf8(file.read(4097)));
}
namespace {
using Type = core::EarthOrbitType;
class ParameterScroll final : public QScrollArea {
  public:
    explicit ParameterScroll(QWidget *parameters) {
        setObjectName("orbitWizardParameterScroll");
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true);
        setWidget(parameters);
        parameters->installEventFilter(this);
        new WidthObserver(viewport(), [this](int) { fit_height(); });
    }
    void set_stacked(bool stacked) {
        stacked_ = stacked;
        fit_height();
    }

  protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == widget() && event->type() == QEvent::LayoutRequest)
            QTimer::singleShot(0, this, [this] { fit_height(); });
        return QScrollArea::eventFilter(object, event);
    }

  private:
    void fit_height() {
        int height = 0;
        if (stacked_) {
            height = widget()->minimumSizeHint().height();
            if (widget()->layout()->hasHeightForWidth())
                height =
                    std::max(height, widget()->layout()->totalHeightForWidth(viewport()->width()));
        }
        if (minimumHeight() != height)
            setMinimumHeight(height);
    }
    bool stacked_ = false;
};
class ParameterSlider final : public QSlider {
  public:
    explicit ParameterSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent) {
        setFocusPolicy(Qt::StrongFocus);
        setProperty("orbitParameter", true);
    }

  protected:
    void wheelEvent(QWheelEvent *event) override {
        // Scrolling a long form must not silently edit an unfocused parameter.
        if (hasFocus())
            QSlider::wheelEvent(event);
        else
            event->ignore();
    }
};
QWidget *parameter_row(QuantityEditor *field) { return field->parentWidget(); }

class ParameterRow final : public QWidget {
  public:
    ParameterRow(const char *name, const QString &label, Unit unit, double minimum, double maximum,
                 double value, double slider_maximum)
        : unit_(unit), minimum_(minimum), maximum_(maximum),
          upper_(slider_maximum > minimum ? slider_maximum : maximum),
          step_(unit == Unit::degree ? 0.1
                : unit == Unit::one  ? 0.001
                                     : 1) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        field = new QuantityEditor(unit, this);
        field->setObjectName(name);
        field->set_label(label);
        field->set_range(minimum, maximum, unit);
        field->set_value(value, unit);
        setFocusProxy(field);
        layout->addWidget(field);
        slider_ = new ParameterSlider(this);
        slider_->setObjectName(QString::fromLatin1(name) + "Slider");
        slider_->setAccessibleName(label);
        slider_->setFixedHeight(16);
        layout->addWidget(slider_);
        connect(field, &QuantityEditor::valueChanged, this, [this] { sync(); });
        connect(field, &QuantityEditor::unitChanged, this, [this] { sync(); });
        connect(slider_, &QSlider::valueChanged, this, [this](int position) {
            field->set_value(std::min(upper_, minimum_ + position * step_), unit_);
        });
        sync();
    }
    QuantityEditor *field;

  private:
    void sync() {
        try {
            const double value = field->value(unit_);
            // A precise typed value can extend the useful slider range without being
            // clamped or rounded back to the slider's deliberately coarser resolution.
            if (value > upper_)
                upper_ = std::min(maximum_,
                                  minimum_ + std::ceil((value - minimum_) / (upper_ - minimum_)) *
                                                 (upper_ - minimum_));
            const QSignalBlocker blocker(slider_);
            slider_->setRange(0, static_cast<int>(std::ceil((upper_ - minimum_) / step_)));
            slider_->setPageStep(std::max(1, slider_->maximum() / 20));
            slider_->setValue(static_cast<int>(std::lround((value - minimum_) / step_)));
            slider_->setToolTip(
                ui::text("orbit_wizard.slider_hint")
                    .arg(convert_quantity(minimum_, unit_, field->unit()), 0, 'g', 8)
                    .arg(convert_quantity(upper_, unit_, field->unit()), 0, 'g', 8)
                    .arg(unit_symbol(field->unit())));
        } catch (const std::exception &) {
            // Keep an incomplete text edit intact; dragging can supply a valid value.
        }
    }
    ParameterSlider *slider_;
    Unit unit_;
    double minimum_, maximum_, upper_, step_;
};
struct DesignChoice {
    Type type;
    const char *label;
    const char *description;
};
constexpr std::array choices{
    DesignChoice{Type::circular, "orbit_wizard.circular", "orbit_wizard.explain_circular"},
    DesignChoice{Type::critically_inclined, "orbit_wizard.critical",
                 "orbit_wizard.explain_critical"},
    DesignChoice{Type::critically_inclined_sun_sync, "orbit_wizard.critical_sun",
                 "orbit_wizard.explain_critical_sun"},
    DesignChoice{Type::geosynchronous, "orbit_wizard.geo", "orbit_wizard.explain_geo"},
    DesignChoice{Type::molniya, "orbit_wizard.molniya", "orbit_wizard.explain_molniya"},
    DesignChoice{Type::orbit_designer, "orbit_wizard.elliptical", "orbit_wizard.explain_designer"},
    DesignChoice{Type::repeating_ground_trace, "orbit_wizard.repeat",
                 "orbit_wizard.explain_repeat"},
    DesignChoice{Type::repeating_sun_sync, "orbit_wizard.repeat_sun",
                 "orbit_wizard.explain_repeat_sun"},
    DesignChoice{Type::sun_synchronous, "orbit_wizard.sun", "orbit_wizard.explain_sun"}};
QString design_error(const core::OrbitDesignError &error) {
    switch (error.code) {
    case core::OrbitDesignErrorCode::below_surface:
        return ui::text("orbit_wizard.below_surface");
    case core::OrbitDesignErrorCode::no_solution:
        return ui::text("orbit_wizard.no_solution");
    case core::OrbitDesignErrorCode::repeat_ratio:
        return ui::text("orbit_wizard.invalid_repeat");
    default:
        return ui::text("orbit_wizard.invalid_parameter");
    }
}
} // namespace
OrbitWizardDialog::OrbitWizardDialog(core::Epoch epoch, QWidget *parent)
    : QDialog(parent), epoch_(epoch) {
    setObjectName("orbitWizard");
    setWindowTitle(ui::text("insert_object_dialog.wizard"));
    setWindowIcon(workbench_icon(WorkbenchIcon::orbit));
    resize(980, 590);
    setMinimumSize(360, 300);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(16);
    auto *top = new QHBoxLayout;
    top->setSpacing(12);
    top->addWidget(new QLabel(ui::text("orbit_wizard.preset")));
    type_ = new QComboBox;
    type_->setObjectName("orbitWizardPreset");
    for (const auto &choice : choices)
        type_->addItem(ui::text(choice.label), static_cast<int>(choice.type));
    type_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    type_->setMinimumContentsLength(12);
    top->addWidget(type_, 1);
    top->addStretch(1);
    auto *epoch_hint = new QLabel(ui::text("orbit_wizard.epoch_hint"));
    epoch_hint->setObjectName("hint");
    epoch_hint->setWordWrap(true);
    top->addWidget(epoch_hint);
    new WidthObserver(this, [epoch_hint](int width) { epoch_hint->setVisible(width >= 600); });
    layout->addLayout(top);
    auto *scroll = new QScrollArea;
    scroll->setObjectName("orbitWizardScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget;
    auto *columns = new QBoxLayout(QBoxLayout::LeftToRight, body);
    columns->setContentsMargins(0, 0, 8, 0);
    columns->setSpacing(24);
    auto *parameters = new QWidget;
    parameters->setObjectName("orbitWizardParameters");
    auto *parameter_layout = new QVBoxLayout(parameters);
    parameter_layout->setContentsMargins(0, 0, 0, 0);
    parameter_layout->setSpacing(16);
    auto *parameter_title = new QLabel(ui::text("orbit_wizard.parameters"));
    parameter_title->setObjectName("sectionTitle");
    parameter_layout->addWidget(parameter_title);
    auto *form_body = new QWidget;
    auto *form = form_ = new QFormLayout(form_body);
    parameter_layout->addWidget(form_body);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setContentsMargins(0, 0, 0, 0);
    form->setVerticalSpacing(14);
    const auto quantity = [form](const char *name, const char *label, Unit unit, double minimum,
                                 double maximum, double value, double slider_maximum = 0) {
        auto *row =
            new ParameterRow(name, ui::text(label), unit, minimum, maximum, value, slider_maximum);
        form->addRow(ui::text(label), row);
        return row->field;
    };
    sun_driver_ = new QComboBox;
    sun_driver_->setObjectName("wizardSunDriver");
    sun_driver_->addItems(
        {ui::text("orbit_wizard.by_altitude"), ui::text("orbit_wizard.by_inclination")});
    form->addRow(ui::text("orbit_wizard.solve_by"), sun_driver_);
    direction_ = new QComboBox;
    direction_->setObjectName("wizardDirection");
    direction_->addItems({ui::text("orbit_wizard.prograde"), ui::text("orbit_wizard.retrograde")});
    form->addRow(ui::text("orbit_wizard.direction"), direction_);
    altitude_ =
        quantity("wizardAltitude", "orbit_wizard.altitude", Unit::kilometre, 0, 100000, 500, 2000);
    perigee_height_ = quantity("wizardPerigeeHeight", "orbit_wizard.perigee_height",
                               Unit::kilometre, 0, 100000, 500, 2000);
    apogee_height_ = quantity("wizardApogeeHeight", "orbit_wizard.apogee_height", Unit::kilometre,
                              0, 100000, 40000, 50000);
    constexpr std::array labels{"orbit_wizard.semimajor",   "orbit_wizard.eccentricity",
                                "orbit_wizard.inclination", "orbit_wizard.raan",
                                "orbit_wizard.periapsis",   "orbit_wizard.anomaly"};
    constexpr std::array names{"wizardSemimajor", "wizardEccentricity", "wizardInclination",
                               "wizardRaan",      "wizardPeriapsis",    "wizardAnomaly"};
    const double radius = core::central_body(core::OriginId::earth).equatorial_radius_m / 1000;
    const std::array values{radius + 1250, 0.05, 53., 0., 0., 0.};
    for (std::size_t index = 0; index < fields_.size(); ++index)
        fields_[index] = quantity(names[index], labels[index],
                                  index == 0   ? Unit::kilometre
                                  : index == 1 ? Unit::one
                                               : Unit::degree,
                                  index == 0 ? radius : 0,
                                  index == 0   ? radius + 100000
                                  : index == 1 ? 0.99999
                                  : index == 2 ? 180
                                               : 360,
                                  values[index], index == 0 ? radius + 50000 : 0);
    geo_inclination_ =
        quantity("wizardGeoInclination", "orbit_wizard.inclination", Unit::degree, 0, 89.999, 0);
    sun_inclination_ =
        quantity("wizardSunInclination", "orbit_wizard.inclination", Unit::degree, 90, 180, 98);
    molniya_periapsis_ =
        quantity("wizardMolniyaPeriapsis", "orbit_wizard.periapsis", Unit::degree, 0, 360, 270);
    repeat_revolutions_ = new QSpinBox;
    repeat_revolutions_->setObjectName("wizardRepeatRevolutions");
    repeat_days_ = new QSpinBox;
    repeat_days_->setObjectName("wizardRepeatDays");
    for (auto *box : {repeat_revolutions_, repeat_days_}) {
        box->setRange(1, 1000000);
        box->setButtonSymbols(QAbstractSpinBox::NoButtons);
        box->setKeyboardTracking(false);
    }
    repeat_revolutions_->setValue(15);
    repeat_days_->setValue(1);
    form->addRow(ui::text("orbit_wizard.revolutions"), repeat_revolutions_);
    form->addRow(ui::text("orbit_wizard.days"), repeat_days_);
    repeat_days_->setToolTip(ui::text("orbit_wizard.days_hint"));
    const auto move_before = [form](QWidget *field, QWidget *before) {
        auto row = form->takeRow(field);
        int index;
        QFormLayout::ItemRole role;
        form->getWidgetPosition(before, &index, &role);
        form->insertRow(index, row.labelItem->widget(), row.fieldItem->widget());
        delete row.labelItem;
        delete row.fieldItem;
    };
    for (QWidget *field :
         std::array<QWidget *, 4>{parameter_row(geo_inclination_), parameter_row(sun_inclination_),
                                  repeat_revolutions_, repeat_days_})
        move_before(field, parameter_row(fields_[3]));
    move_before(parameter_row(molniya_periapsis_), parameter_row(fields_[5]));
    auto *parameter_scroll = new ParameterScroll(parameters);
    columns->addWidget(parameter_scroll, 3);
    auto *summary = new QWidget;
    summary->setObjectName("orbitWizardSummary");
    auto *summary_layout = new QVBoxLayout(summary);
    summary_layout->setContentsMargins(0, 0, 0, 0);
    summary_layout->setSpacing(12);
    description_ = new QLabel;
    description_->setWordWrap(true);
    description_->setObjectName("orbitWizardDescription");
    description_->setProperty("muted", true);
    parameter_layout->addWidget(description_);
    map_preview_ = new OrbitDesignPreview;
    summary_layout->addWidget(map_preview_, 1);
    preview_ = new QLabel;
    preview_->setObjectName("orbitWizardPreview");
    // A changing number must not wrap a result row and resize the map above it.
    preview_->setWordWrap(false);
    preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    preview_->setTextFormat(Qt::RichText);
    preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary_layout->addWidget(preview_);
    new WidthObserver(summary, [this, mode = WidthMode{}](int width) mutable {
        const bool compact = mode.narrow(width, 450);
        if (compact_results_ == compact)
            return;
        compact_results_ = compact;
        render_results();
    });
    convention_ = new QLabel;
    convention_->setObjectName("hint");
    convention_->setWordWrap(true);
    convention_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    parameter_layout->addWidget(convention_);
    error_ = new QLabel;
    error_->setObjectName("orbitWizardError");
    error_->setTextFormat(Qt::PlainText);
    error_->setWordWrap(true);
    error_->ensurePolished();
    // Reserve validation space even for an empty draft, including stacked layouts.
    error_->setMinimumHeight(3 * error_->fontMetrics().lineSpacing() + 8);
    parameter_layout->addWidget(error_);
    parameter_layout->addStretch();
    columns->addWidget(summary, 7);
    responsive_form(form, 250);
    columns->setDirection(QBoxLayout::TopToBottom);
    new WidthObserver(body, [columns, parameter_scroll, mode = WidthMode{}](int width) mutable {
        const bool narrow = mode.narrow(width, 720);
        parameter_scroll->set_stacked(narrow);
        columns->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        columns->setStretch(0, narrow ? 0 : 3);
    });
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *cancel = new QPushButton(ui::text("common.cancel"));
    cancel->setAutoDefault(false);
    auto *next = next_ = new QPushButton(ui::text("orbit_wizard.continue"));
    next->setProperty("primary", true);
    next->setObjectName("acceptOrbitWizard");
    next->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(next);
    layout->addLayout(buttons);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(next, &QPushButton::clicked, this, [this] {
        try {
            input_ = read_input();
            accept();
        } catch (const core::OrbitDesignError &error) {
            error_->setText(design_error(error));
        } catch (const std::exception &error) {
            error_->setText(QString::fromUtf8(error.what()));
        }
    });
    connect(type_, &QComboBox::currentIndexChanged, this, [this] { update_type(); });
    connect(sun_driver_, &QComboBox::currentIndexChanged, this, [this] { update_type(); });
    connect(direction_, &QComboBox::currentIndexChanged, this, [this] { refresh_preview(); });
    for (auto *box : {repeat_days_, repeat_revolutions_})
        connect(box, &QSpinBox::valueChanged, this, [this] { refresh_preview(); });
    for (auto *field : body->findChildren<QuantityEditor *>()) {
        connect(field->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged, this,
                [this] { refresh_preview(); });
        connect(field, &QuantityEditor::valueChanged, this, [this] { refresh_preview(); });
        connect(field, &QuantityEditor::unitChanged, this, [this] { refresh_preview(); });
    }
    update_type();
}
void OrbitWizardDialog::set_preview_context(
    QImage imagery, std::shared_ptr<const core::IFrameProvider> frames,
    std::shared_ptr<const core::IEarthOrientationProvider> eop) {
    map_preview_->set_context(std::move(imagery), std::move(frames), std::move(eop));
}
void OrbitWizardDialog::update_type() {
    const auto type = static_cast<Type>(type_->currentData().toInt());
    const bool custom = type == Type::orbit_designer;
    const bool critical = type == Type::critically_inclined;
    const bool critical_sun = type == Type::critically_inclined_sun_sync;
    const bool molniya = type == Type::molniya;
    const bool sun = type == Type::sun_synchronous;
    const bool repeat = type == Type::repeating_ground_trace || type == Type::repeating_sun_sync;
    const bool elliptic = custom || critical || critical_sun || molniya;
    const bool j2 = type != Type::circular && !custom && type != Type::geosynchronous;
    form_->setRowVisible(sun_driver_, sun);
    form_->setRowVisible(direction_, critical);
    form_->setRowVisible(parameter_row(altitude_),
                         type == Type::circular || (sun && sun_driver_->currentIndex() == 0));
    form_->setRowVisible(parameter_row(perigee_height_), critical || critical_sun || molniya);
    form_->setRowVisible(parameter_row(apogee_height_), critical);
    form_->setRowVisible(parameter_row(fields_[0]), custom);
    form_->setRowVisible(parameter_row(fields_[1]), custom);
    form_->setRowVisible(parameter_row(fields_[2]),
                         type == Type::circular || custom || type == Type::repeating_ground_trace);
    form_->setRowVisible(parameter_row(sun_inclination_), sun && sun_driver_->currentIndex() == 1);
    form_->setRowVisible(parameter_row(fields_[4]), custom || critical || critical_sun);
    form_->setRowVisible(parameter_row(geo_inclination_), type == Type::geosynchronous);
    form_->setRowVisible(parameter_row(molniya_periapsis_), molniya);
    form_->setRowVisible(repeat_revolutions_, repeat);
    form_->setRowVisible(repeat_days_, repeat);
    const auto phase = ui::text(elliptic ? "orbit_wizard.anomaly" : "orbit_wizard.phase");
    qobject_cast<QLabel *>(form_->labelForField(parameter_row(fields_[5])))->setText(phase);
    fields_[5]->set_label(phase);
    parameter_row(fields_[5])->findChild<QSlider *>()->setAccessibleName(phase);
    description_->setText(
        ui::text(choices[static_cast<std::size_t>(type_->currentIndex())].description));
    convention_->setText(ui::text(j2 ? "orbit_wizard.model_j2" : "orbit_wizard.model_two_body"));
    convention_->setToolTip(
        ui::text(j2 ? "orbit_wizard.convention_j2" : "orbit_wizard.convention"));
    refresh_preview();
}
core::EarthOrbitDesign OrbitWizardDialog::read_design() const {
    core::EarthOrbitDesignRequest q;
    q.epoch = epoch_;
    q.type = static_cast<Type>(type_->currentData().toInt());
    // Read only active controls: an unfinished draft in another type is preserved
    // without preventing a valid design of the currently selected type.
    if (form_->isRowVisible(parameter_row(altitude_)))
        q.altitude_m = altitude_->value(Unit::metre);
    if (form_->isRowVisible(parameter_row(perigee_height_)))
        q.perigee_altitude_m = perigee_height_->value(Unit::metre);
    if (form_->isRowVisible(parameter_row(apogee_height_)))
        q.apogee_altitude_m = apogee_height_->value(Unit::metre);
    if (form_->isRowVisible(parameter_row(fields_[0])))
        q.semi_major_axis_m = fields_[0]->value(Unit::metre);
    if (form_->isRowVisible(parameter_row(fields_[1])))
        q.eccentricity = fields_[1]->value(Unit::one);
    if (form_->isRowVisible(parameter_row(fields_[2])))
        q.inclination_rad = fields_[2]->value(Unit::radian);
    if (form_->isRowVisible(parameter_row(geo_inclination_)))
        q.inclination_rad = geo_inclination_->value(Unit::radian);
    if (form_->isRowVisible(parameter_row(sun_inclination_)))
        q.inclination_rad = sun_inclination_->value(Unit::radian);
    q.raan_rad = fields_[3]->value(Unit::radian);
    if (form_->isRowVisible(parameter_row(fields_[4])))
        q.argument_of_periapsis_rad = fields_[4]->value(Unit::radian);
    if (form_->isRowVisible(parameter_row(molniya_periapsis_)))
        q.argument_of_periapsis_rad = molniya_periapsis_->value(Unit::radian);
    q.true_anomaly_rad = fields_[5]->value(Unit::radian);
    q.retrograde = direction_->currentIndex() == 1;
    q.sun_sync_by_inclination = sun_driver_->currentIndex() == 1;
    q.repeat_revolutions = repeat_revolutions_->value();
    q.repeat_days = repeat_days_->value();
    return core::design_earth_orbit(q);
}
application::OrbitInput OrbitWizardDialog::read_input() const {
    const auto design = read_design();
    application::OrbitInput candidate;
    candidate.propagator = design.secular_j2 ? application::OrbitPropagator::j2_secular
                                             : application::OrbitPropagator::two_body;
    // J2 stores the auxiliary mean ellipse, not the precessing trajectory velocity.
    candidate.initial =
        runtime::to_cartesian(design.elements, core::central_body(core::OriginId::earth).mu);
    return candidate;
}
void OrbitWizardDialog::refresh_preview() {
    error_->clear();
    try {
        const auto design = read_design();
        preview_design_ = design;
        map_preview_->set_design(design);
    } catch (const core::OrbitDesignError &error) {
        preview_design_.reset();
        map_preview_->set_design(std::nullopt);
        error_->setText(design_error(error));
    } catch (const std::exception &error) {
        preview_design_.reset();
        map_preview_->set_design(std::nullopt);
        error_->setText(QString::fromUtf8(error.what()));
    }
    next_->setEnabled(preview_design_.has_value());
    render_results();
}
void OrbitWizardDialog::render_results() {
    // Invalid input clears values, not the summary's structure. Retaining the same
    // rows also preserves the viewport while a number is temporarily incomplete.
    std::array<QString, 6> values;
    values.fill(QString(QChar(0x2014)));
    if (preview_design_) {
        const auto &design = *preview_design_;
        const auto &elements = design.elements;
        const auto &earth = core::central_body(core::OriginId::earth);
        values = {QString::number((elements.semi_major_axis_m * (1 - elements.eccentricity) -
                                   earth.equatorial_radius_m) /
                                      1000,
                                  'f', 2),
                  QString::number((elements.semi_major_axis_m * (1 + elements.eccentricity) -
                                   earth.equatorial_radius_m) /
                                      1000,
                                  'f', 2),
                  QString::number(design.nodal_period_seconds / 60, 'f', 3),
                  QString::number(elements.inclination_rad * 180 / std::numbers::pi, 'f', 6),
                  QString::number(elements.eccentricity, 'f', 7),
                  QString::number(design.raan_rate_rad_s * 86400 * 180 / std::numbers::pi, 'f', 6)};
    }
    auto text = ui::text(compact_results_ ? "orbit_wizard.preview_compact" : "orbit_wizard.preview")
                    .arg(values[0], values[1], values[2], values[3], values[4], values[5]);
    const auto type = static_cast<Type>(type_->currentData().toInt());
    if (type == Type::repeating_ground_trace || type == Type::repeating_sun_sync)
        text +=
            ui::text("orbit_wizard.repeat_preview")
                .arg(compact_results_ ? 2 : 3)
                .arg(preview_design_
                         ? QString::number(preview_design_->repeat_cycle_seconds / 86400, 'f', 6)
                         : QString(QChar(0x2014)));
    preview_->setText(text + QStringLiteral("</table>"));
}
} // namespace orbelle::desktop
