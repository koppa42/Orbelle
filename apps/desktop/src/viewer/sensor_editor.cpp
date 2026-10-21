#include "sensor_editor.hpp"
#include "../ui_text.hpp"
#include "object_selection.hpp"
#include "project_update.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QButtonGroup>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace orbitforge::desktop {
// A long satellite name must not widen a docked properties page.
class SensorParentLabel final : public QLabel {
  public:
    explicit SensorParentLabel(QWidget *parent) : QLabel(parent) {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
    }
    void set_name(const QString &name) {
        name_ = name;
        setToolTip(name);
        setAccessibleName(ui::text("sensor_editor.label").arg(name));
        update_text();
    }

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        update_text();
    }

  private:
    void update_text() { setText(fontMetrics().elidedText(name_, Qt::ElideRight, width())); }
    QString name_;
};

// Intrinsic cone cross-section only: no orbital position or ground coverage is implied.
class SensorAnglePreview final : public QWidget {
  public:
    explicit SensorAnglePreview(QWidget *parent) : QWidget(parent) {
        setObjectName("sensorAnglePreview");
        setFixedSize(100, 112);
        setAccessibleName(ui::text("sensor_editor.label_2"));
    }
    void set_angle(std::optional<double> degrees) {
        degrees_ = degrees;
        setAccessibleDescription(degrees ? ui::text("sensor_editor.label_3")
                                               .arg(*degrees, 0, 'g', 6)
                                               .arg(*degrees * 2, 0, 'g', 6)
                                         : ui::text("sensor_editor.label_4"));
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPointF apex(width() / 2.0, 20);
        constexpr double radius = 64;
        painter.setPen(QPen(theme::color(theme::border_strong), 1, Qt::DashLine));
        painter.drawLine(apex, QPointF(apex.x(), 87));
        if (degrees_) {
            // Fit a true-angle sector even for a 90-degree half-angle.
            const double extent =
                std::max(1.0, std::sin(*degrees_ * std::numbers::pi / 180) * radius);
            const double length = radius * std::min(1.0, (width() / 2.0 - 8) / extent);
            QPainterPath cone;
            cone.moveTo(apex);
            cone.arcTo(QRectF(apex.x() - length, apex.y() - length, length * 2, length * 2),
                       -90 + *degrees_, -*degrees_ * 2);
            cone.closeSubpath();
            painter.setPen(QPen(theme::color(theme::accent), 1.3));
            painter.setBrush(theme::color(theme::accent, 32));
            painter.drawPath(cone);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(theme::color(theme::sakura), 1.5));
            painter.drawArc(QRectF(apex.x() - 25, apex.y() - 25, 50, 50), -90 * 16,
                            qRound(*degrees_ * 16));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::color(theme::accent_bright));
        painter.drawEllipse(apex, 3, 3);
        painter.setPen(theme::color(theme::text_muted));
        auto font = painter.font();
        font.setPixelSize(10);
        painter.setFont(font);
        painter.drawText(QRect(0, 92, width(), 18), Qt::AlignCenter,
                         ui::text("sensor_editor.label_5"));
    }

  private:
    std::optional<double> degrees_;
};

SensorEditor::SensorEditor(QWidget *parent) : QDialog(parent) {
    setObjectName("sensorProperties");
    setWindowIcon(workbench_icon(WorkbenchIcon::focus));
    setMinimumWidth(320);
    resize(480, 640);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("sensorScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // The form fits the minimum pane width; only vertical scrolling is useful.
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto *body = new QWidget;
    body->setObjectName("sensorFields");
    body->setMaximumWidth(660);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(theme::space::lg);
    // Sensor editors often live in narrow side panes; the side margins tighten there.
    new WidthObserver(this, [layout, mode = WidthMode{}](int width) mutable {
        const auto margins = theme::space::page;
        layout->setContentsMargins(
            mode.narrow(width, 480) ? QMargins(16, margins.top(), 16, margins.bottom()) : margins);
    });

    auto *heading = new QHBoxLayout;
    heading->setSpacing(12);
    auto *icon = new QLabel(body);
    icon->setObjectName("sensorBadge");
    icon->setFixedSize(40, 40);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(workbench_icon(WorkbenchIcon::focus, theme::color(theme::accent))
                        .pixmap(QSize(24, 24), devicePixelRatioF()));
    auto *heading_text = new QVBoxLayout;
    heading_text->setSpacing(4);
    title_ = new QLabel(ui::text("sensor_editor.heading_text"), body);
    title_->setObjectName("sensorTitle");
    auto *subtitle = new QLabel(ui::text("sensor_editor.subtitle"), body);
    subtitle->setObjectName("sensorCaption");
    heading_text->addWidget(title_);
    heading_text->addWidget(subtitle);
    heading->addWidget(icon);
    heading->addLayout(heading_text, 1);
    layout->addLayout(heading);

    auto *parent_row = new QFrame(body);
    parent_row->setObjectName("sensorParent");
    auto *parent_layout = new QHBoxLayout(parent_row);
    parent_layout->setContentsMargins(theme::space::card);
    parent_layout->setSpacing(8);
    auto *parent_icon = new QLabel(parent_row);
    parent_icon->setPixmap(
        workbench_icon(WorkbenchIcon::satellite).pixmap(QSize(16, 16), devicePixelRatioF()));
    auto *parent_caption = new QLabel(ui::text("sensor_editor.parent_caption"), parent_row);
    parent_caption->setObjectName("sensorCaption");
    parent_ = new SensorParentLabel(parent_row);
    parent_->setObjectName("sensorParentName");
    parent_layout->addWidget(parent_icon);
    parent_layout->addWidget(parent_caption);
    parent_layout->addWidget(parent_, 1);
    layout->addWidget(parent_row);

    auto *name_row = new QVBoxLayout;
    name_row->setSpacing(8);
    auto *name_label = new QLabel(ui::text("sensor_editor.name_label"), body);
    name_ = new QLineEdit(body);
    name_->setObjectName("sensorName");
    name_->setMaxLength(16384);
    name_->setPlaceholderText(ui::text("sensor_editor.name.set_placeholder_text"));
    name_->setClearButtonEnabled(true);
    name_label->setBuddy(name_);
    name_row->addWidget(name_label);
    name_row->addWidget(name_);
    layout->addLayout(name_row);

    auto *fov = new QFrame(body);
    fov->setObjectName("sensorFov");
    auto *fov_layout = new QHBoxLayout(fov);
    fov_layout->setContentsMargins(16, 16, 16, 16);
    fov_layout->setSpacing(8);
    auto *angle_fields = new QVBoxLayout;
    angle_fields->setSpacing(8);
    auto *angle_label = new QLabel(ui::text("sensor_editor.angle_label"), fov);
    angle_ = new QuantityEditor(Unit::degree, fov);
    angle_->setObjectName("sensorHalfAngle");
    angle_->set_label(ui::text("sensor_editor.angle_label"));
    angle_->set_range(.001, 90, Unit::degree);
    angle_label->setBuddy(angle_->findChild<QLineEdit *>("quantityValue"));
    full_angle_ = new QLabel(fov);
    full_angle_->setObjectName("sensorFullAngle");
    full_angle_->setWordWrap(true);
    angle_fields->addStretch();
    angle_fields->addWidget(angle_label);
    angle_fields->addWidget(angle_);
    angle_fields->addWidget(full_angle_);
    angle_fields->addStretch();
    preview_ = new SensorAnglePreview(fov);
    fov_layout->addLayout(angle_fields, 1);
    fov_layout->addWidget(preview_);
    layout->addWidget(fov);

    auto *pointing_layout = new QVBoxLayout;
    pointing_layout->setSpacing(8);
    pointing_layout->addWidget(
        new QLabel(ui::text("sensor_editor.pointing_layout.add_widget"), body));
    auto *choices = new QHBoxLayout;
    choices->setSpacing(8);
    pointing_ = new QButtonGroup(this);
    auto *nadir = new QPushButton(workbench_icon(WorkbenchIcon::earth),
                                  ui::text("sensor_editor.nadir"), body);
    auto *targeted = new QPushButton(workbench_icon(WorkbenchIcon::ground_station),
                                     ui::text("sensor_editor.targeted"), body);
    nadir->setObjectName("sensorNadir");
    targeted->setObjectName("sensorTargeted");
    for (auto *button : {nadir, targeted}) {
        button->setProperty("sensorChoice", true);
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(40);
        choices->addWidget(button, 1);
    }
    pointing_->addButton(nadir, static_cast<int>(application::SensorPointing::nadir));
    pointing_->addButton(targeted, static_cast<int>(application::SensorPointing::targeted));
    pointing_layout->addLayout(choices);
    pointing_hint_ = new QLabel(body);
    pointing_hint_->setObjectName("sensorCaption");
    pointing_hint_->setWordWrap(true);
    pointing_layout->addWidget(pointing_hint_);

    target_row_ = new QWidget(body);
    target_row_->setObjectName("sensorTargetRow");
    auto *target_layout = new QVBoxLayout(target_row_);
    target_layout->setContentsMargins(0, 8, 0, 0);
    target_layout->setSpacing(8);
    auto *target_label = new QLabel(ui::text("sensor_editor.target_label"), target_row_);
    target_ = new ObjectSelectionField(target_row_);
    target_->setObjectName("sensorTarget");
    target_->set_options({ui::text("sensor_editor.label_6"), false, {}});
    target_->setAccessibleName(ui::text("sensor_editor.target_label"));
    target_label->setBuddy(target_);
    target_hint_ = new QLabel(target_row_);
    target_hint_->setObjectName("sensorTargetHint");
    target_hint_->setWordWrap(true);
    target_layout->addWidget(target_label);
    target_layout->addWidget(target_);
    target_layout->addWidget(target_hint_);
    pointing_layout->addWidget(target_row_);
    layout->addLayout(pointing_layout);
    layout->addStretch();
    scroll->setWidget(body);
    root->addWidget(scroll, 1);

    auto *footer = new QFrame(this);
    footer->setObjectName("sensorFooter");
    auto *footer_layout = new QVBoxLayout(footer);
    footer_layout->setContentsMargins(theme::space::footer);
    footer_layout->setSpacing(10);
    error_ = new QLabel(footer);
    error_->setObjectName("sensorError");
    error_->setTextFormat(Qt::PlainText);
    error_->setWordWrap(true);
    footer_layout->addWidget(error_);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    close_ = new QPushButton(ui::text("common.close"), footer);
    close_->setObjectName("closeSensor");
    close_->setAutoDefault(false);
    close_->setToolTip(ui::text("sensor_editor.close.set_tool_tip"));
    apply_ = new QPushButton(ui::text("sensor_editor.heading_text"), footer);
    apply_->setObjectName("applySensor");
    apply_->setProperty("primary", true);
    apply_->setDefault(true);
    auto *reset = new QPushButton(ui::text("sensor_editor.reset"), footer);
    reset->setObjectName("resetSensor");
    connect(reset, &QPushButton::clicked, this, [this] {
        name_->setText(QString::fromStdString(original_.name()));
        angle_->set_value(original_.half_angle_rad, Unit::radian);
        pointing_->button(static_cast<int>(original_.pointing))->setChecked(true);
        target_->set_id(original_.target_station_id);
        show_error({});
        update_pointing();
        update_preview();
    });
    buttons->addWidget(reset);
    buttons->addStretch();
    buttons->addWidget(close_);
    buttons->addWidget(apply_);
    footer_layout->addLayout(buttons);
    root->addWidget(footer);

    connect(close_, &QPushButton::clicked, this, &QDialog::close);
    connect(apply_, &QPushButton::clicked, this, [this] {
        try {
            const auto value = sensor();
            if (apply)
                apply(value);
            show_error({});
        } catch (const std::exception &e) {
            show_error(QString::fromUtf8(e.what()));
        }
    });
    connect(pointing_, &QButtonGroup::idToggled, this, [this](int, bool checked) {
        if (checked)
            update_pointing();
    });
    connect(target_, &ObjectSelectionField::selectionChanged, this, [this] { update_pointing(); });
    connect(angle_, &QuantityEditor::valueChanged, this, [this] { update_preview(); });
    connect(angle_, &QuantityEditor::unitChanged, this, [this] { update_preview(); });
    connect(
        angle_->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged, this,
        [this] { update_preview(); }, Qt::QueuedConnection);
    nadir->setChecked(true);
    show_error({});
    update_preview();
}

void SensorEditor::set_sensor(application::ProjectSensor sensor,
                              const application::Project &project, bool creating) {
    original_ = std::move(sensor);
    name_->setText(QString::fromStdString(original_.name()));
    QString parent_name = QString::fromStdString(original_.parent_satellite_id);
    for (const auto &sat : project.satellites)
        if (sat.orbit.object_id == original_.parent_satellite_id)
            parent_name = QString::fromStdString(sat.name());
    parent_->set_name(parent_name);
    angle_->set_value(original_.half_angle_rad, Unit::radian);
    target_->set_objects(project_object_choices(project, {ObjectKind::ground_station}));
    if (!original_.target_station_id.empty())
        target_->set_id(original_.target_station_id);
    else
        target_->set_id(
            project.ground_stations.empty() ? "" : project.ground_stations.front().object_id);
    if (auto *button = pointing_->button(static_cast<int>(original_.pointing)))
        button->setChecked(true);
    else
        throw std::invalid_argument(ui::utf8("sensor_editor.button"));
    title_->setText(creating ? ui::text("sensor_editor.heading_text")
                             : ui::text("sensor_editor.title.set_text"));
    apply_->setText(creating ? ui::text("sensor_editor.heading_text") : ui::text("common.apply"));
    close_->setText(ui::text("common.close"));
    setWindowTitle(creating ? ui::text("sensor_editor.heading_text")
                            : QString::fromStdString(original_.name()) +
                                  ui::text("sensor_editor.label_7"));
    show_error({});
    update_pointing();
    update_preview();
}

void SensorEditor::update_pointing() {
    const bool targeted =
        pointing_->checkedId() == static_cast<int>(application::SensorPointing::targeted);
    target_row_->setVisible(targeted);
    pointing_hint_->setText(targeted ? ui::text("sensor_editor.pointing_hint.set_text")
                                     : ui::text("sensor_editor.pointing_hint.set_text_2"));
    target_hint_->setText(target_->objects().empty()
                              ? ui::text("sensor_editor.target_hint.set_text")
                              : ui::text("sensor_editor.target_hint.set_text_2"));
    target_hint_->setVisible(!target_->contains(target_->id()));
}

void SensorEditor::update_preview() {
    try {
        const auto degrees = angle_->value(Unit::degree);
        full_angle_->setText(
            ui::text("sensor_editor.full_angle.set_text").arg(degrees * 2, 0, 'g', 6));
        preview_->set_angle(degrees);
    } catch (const std::exception &) {
        full_angle_->setText(ui::text("sensor_editor.full_angle.set_text_2"));
        preview_->set_angle(std::nullopt);
    }
}

void SensorEditor::show_error(const QString &text) {
    error_->setText(text);
    error_->setVisible(!text.isEmpty());
}

application::ProjectSensor SensorEditor::sensor() const {
    auto s = original_;
    s.display_name = name_->text().trimmed().toStdString();
    if (s.display_name.empty())
        throw std::invalid_argument(ui::utf8("sensor_editor.s"));
    s.half_angle_rad = angle_->value(Unit::radian);
    s.pointing = static_cast<application::SensorPointing>(pointing_->checkedId());
    s.target_station_id = s.pointing == application::SensorPointing::targeted ? target_->id() : "";
    if (s.pointing == application::SensorPointing::targeted &&
        !target_->contains(s.target_station_id))
        throw std::invalid_argument(target_->objects().empty()
                                        ? ui::utf8("sensor_editor.validation")
                                        : ui::utf8("sensor_editor.label_8"));
    return s;
}
bool SensorEditor::has_unapplied_changes() const {
    try {
        return !same_sensor_input(sensor(), original_);
    } catch (const std::exception &) {
        return true;
    }
}
void SensorEditor::accept_sensor(const application::ProjectSensor &sensor) {
    original_ = sensor;
    title_->setText(ui::text("sensor_editor.title.set_text"));
    apply_->setText(ui::text("common.apply"));
    close_->setText(ui::text("common.close"));
}
void SensorEditor::sync_name(const std::string &name) {
    if (name_->text().trimmed().toStdString() == original_.name())
        name_->setText(QString::fromStdString(name));
    original_.display_name = name;
}
void SensorEditor::update_project(const application::Project &project) {
    for (const auto &satellite : project.satellites)
        if (satellite.orbit.object_id == original_.parent_satellite_id)
            parent_->set_name(QString::fromStdString(satellite.name()));
    target_->set_objects(project_object_choices(project, {ObjectKind::ground_station}));
    update_pointing();
}
} // namespace orbitforge::desktop
