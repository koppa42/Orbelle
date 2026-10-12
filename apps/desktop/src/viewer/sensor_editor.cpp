#include "sensor_editor.hpp"
#include "quantity_editor.hpp"
#include "workbench_style.hpp"
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace orbitforge::desktop {
SensorEditor::SensorEditor(QWidget *parent) : QDialog(parent) {
    setObjectName("sensorProperties");
    resize(440, 550);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(20);
    auto *title = new QLabel(QStringLiteral("圆锥传感器"));
    title->setObjectName("panelTitle");
    layout->addWidget(title);
    parent_ = new QLabel;
    parent_->setWordWrap(true);
    parent_->setTextFormat(Qt::PlainText);
    parent_->setObjectName("dialogDescription");
    layout->addWidget(parent_);
    auto *card = new QFrame;
    card->setObjectName("formSection");
    auto *form = new QFormLayout(card);
    form->setContentsMargins(16, 16, 16, 16);
    form->setVerticalSpacing(16);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    name_ = new QLineEdit;
    name_->setObjectName("sensorName");
    name_->setMaxLength(16384);
    form->addRow(QStringLiteral("名称"), name_);
    angle_ = new QuantityEditor(Unit::degree);
    angle_->setObjectName("sensorHalfAngle");
    angle_->set_label(QStringLiteral("视场半角"));
    angle_->set_range(.001, 90, Unit::degree);
    form->addRow(QStringLiteral("视场半角"), angle_);
    pointing_ = new QComboBox;
    pointing_->setObjectName("sensorPointing");
    pointing_->addItems({QStringLiteral("对地心（Nadir）"), QStringLiteral("跟踪地面站")});
    form->addRow(QStringLiteral("指向"), pointing_);
    target_ = new QComboBox;
    target_->setObjectName("sensorTarget");
    form->addRow(QStringLiteral("跟踪目标"), target_);
    connect(pointing_, &QComboBox::currentIndexChanged, this,
            [this, form](int i) { form->setRowVisible(target_, i == 1); });
    layout->addWidget(card);
    auto *hint = new QLabel(QStringLiteral(
        "传感器与卫星同位置，继承父对象的轨道。\n\n"
        "可见性同时满足地面站约束和圆锥视场。跟踪采用理想瞬时指向，暂不模拟姿态机动与转速限制。"));
    hint->setObjectName("hint");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch();
    scroll->setWidget(body);
    root->addWidget(scroll, 1);
    error_ = new QLabel;
    error_->setObjectName("inputError");
    error_->setTextFormat(Qt::PlainText);
    error_->setWordWrap(true);
    error_->setContentsMargins(24, 0, 24, 0);
    root->addWidget(error_);
    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(24, 8, 24, 20);
    auto *close = new QPushButton(QStringLiteral("关闭"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto *apply_button = new QPushButton(QStringLiteral("应用"));
    apply_button->setObjectName("applySensor");
    apply_button->setProperty("primary", true);
    connect(apply_button, &QPushButton::clicked, this, [this] {
        try {
            if (apply)
                apply(sensor());
            error_->clear();
        } catch (const std::exception &e) {
            error_->setText(QString::fromUtf8(e.what()));
        }
    });
    buttons->addStretch();
    buttons->addWidget(close);
    buttons->addWidget(apply_button);
    root->addLayout(buttons);
}
void SensorEditor::set_sensor(application::ProjectSensor sensor,
                              const application::Project &project, bool creating) {
    original_ = std::move(sensor);
    name_->setText(QString::fromStdString(original_.name()));
    QString parent_name = QString::fromStdString(original_.parent_satellite_id);
    for (const auto &sat : project.satellites)
        if (sat.orbit.object_id == original_.parent_satellite_id)
            parent_name = QString::fromStdString(sat.name());
    parent_->setText(QStringLiteral("挂载于 %1").arg(parent_name));
    angle_->set_value(original_.half_angle_rad, Unit::radian);
    target_->clear();
    for (const auto &station : project.ground_stations)
        target_->addItem(QString::fromStdString(station.name()),
                         QString::fromStdString(station.object_id));
    if (const int i = target_->findData(QString::fromStdString(original_.target_station_id));
        i >= 0)
        target_->setCurrentIndex(i);
    pointing_->setCurrentIndex(-1);
    pointing_->setCurrentIndex(static_cast<int>(original_.pointing));
    error_->clear();
    setWindowTitle(creating
                       ? QStringLiteral("插入传感器")
                       : QString::fromStdString(original_.name()) + QStringLiteral(" · 传感器"));
}
application::ProjectSensor SensorEditor::sensor() const {
    auto s = original_;
    s.display_name = name_->text().trimmed().toStdString();
    if (s.display_name.empty())
        throw std::invalid_argument("请输入传感器名称。");
    s.half_angle_rad = angle_->value(Unit::radian);
    s.pointing = static_cast<application::SensorPointing>(pointing_->currentIndex());
    s.target_station_id = s.pointing == application::SensorPointing::targeted
                              ? target_->currentData().toString().toStdString()
                              : "";
    if (s.pointing == application::SensorPointing::targeted && s.target_station_id.empty())
        throw std::invalid_argument("请先插入地面站，再选择跟踪目标。");
    return s;
}
} // namespace orbitforge::desktop
