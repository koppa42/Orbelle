#include "hpop_covariance_editor.hpp"
#include "form_kit.hpp"
#include <QComboBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QLocale>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <cmath>
#include <stdexcept>

namespace orbitforge::desktop {
namespace {
QTableWidgetItem *number(double value, bool editable = true) {
    auto *item = new QTableWidgetItem(QString::number(value, 'g', 17));
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (!editable)
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}
double read_number(const QString &text, const QString &label) {
    bool ok = false;
    const double value = QLocale::c().toDouble(text.trimmed(), &ok);
    if (!ok || !std::isfinite(value))
        throw std::invalid_argument((label + QStringLiteral("：请输入有限数值。")).toStdString());
    return value;
}
double read_cell(const QTableWidget *table, int row, int column) {
    return read_number(
        table->item(row, column)->text(),
        QStringLiteral("%1 [%2, %3]").arg(table->accessibleName()).arg(row + 1).arg(column + 1));
}
void setup_table(QTableWidget *table, const char *name, const QString &accessible) {
    table->setObjectName(name);
    table->setAccessibleName(accessible);
    table->setMinimumWidth(0);
    table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setMinimumSectionSize(68);
    table->horizontalHeader()->setDefaultSectionSize(92);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setDefaultSectionSize(32);
    table->setFixedHeight(table->horizontalHeader()->sizeHint().height() + table->rowCount() * 32 +
                          24);
    table->setAlternatingRowColors(true);
}
} // namespace
HpopCovarianceEditor::HpopCovarianceEditor(const application::HpopCovarianceSettings &s,
                                           bool supported, QWidget *parent)
    : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    enabled_ = new QCheckBox(QStringLiteral("计算协方差"));
    enabled_->setObjectName("hpopComputeCovariance");
    make_switch(enabled_);
    enabled_->setChecked(s.enabled);
    enabled_->setEnabled(supported);
    layout->addWidget(enabled_);
    if (!supported)
        layout->addWidget(caution_label(QStringLiteral("任务序列的跨机动协方差尚未接入。")));
    fields_ = new QWidget;
    auto *fields_layout = new QVBoxLayout(fields_);
    fields_layout->setContentsMargins(0, 0, 0, 0);
    fields_layout->setSpacing(12);
    layout->addWidget(fields_);
    auto *initial = new FormGroup(QStringLiteral("初始位置速度协方差"), "hpopInitialCovariance");
    fields_layout->addWidget(initial);
    initial->add_help(
        QStringLiteral("矩阵对应轨道初始历元。编辑下三角，上三角自动对称；位置用 m、速度用 m/s，"
                       "三个矩阵块分别为 m²、m²/s、m²/s²。对角线是方差，不是标准差。"));
    auto *form = initial->form();
    frame_ = new QComboBox;
    frame_->setObjectName("hpopCovarianceFrame");
    const auto add_frame = [&](const QString &name, core::HpopCovarianceFrame frame) {
        frame_->addItem(name, static_cast<int>(frame));
    };
    add_frame("GCRF", core::HpopCovarianceFrame::gcrf);
    add_frame("J2000 (IERS1996)", core::HpopCovarianceFrame::fk5_j2000);
    add_frame("TrueOfDate (IERS1996)", core::HpopCovarianceFrame::true_of_date);
    add_frame("LVLH (R, I, C)", core::HpopCovarianceFrame::lvlh);
    add_frame("Frenet (N, T, C)", core::HpopCovarianceFrame::frenet);
    frame_->setCurrentIndex(frame_->findData(static_cast<int>(s.coordinates.frame)));
    form->addRow(QStringLiteral("参考系"), frame_);
    form->addRow(QStringLiteral("表示"), new QLabel("Cartesian"));
    velocity_ = new QComboBox;
    velocity_->setObjectName("hpopCovarianceVelocity");
    velocity_->addItems(
        {QStringLiteral("惯性速度分量投影"), QStringLiteral("相对转动坐标系的速度")});
    velocity_->setCurrentIndex(static_cast<int>(s.coordinates.velocity));
    form->addRow(QStringLiteral("速度约定"), velocity_);
    initial->add_help(QStringLiteral("切换参考系重新解释输入矩阵，不自动换算已有数值。"
                                     "速度投影与转动速度具有不同的交叉项。"));
    state_ = new QTableWidget(6, 6);
    setup_table(state_, "hpopCovarianceMatrix", QStringLiteral("初始协方差"));
    state_->setHorizontalHeaderLabels({"x", "y", "z", "vx", "vy", "vz"});
    state_->setVerticalHeaderLabels({"x", "y", "z", "vx", "vy", "vz"});
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            state_->setItem(
                i, j,
                number(s.state[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)], j <= i));
    form->addRow(state_);
    connect(state_, &QTableWidget::cellChanged, this, [this](int row, int column) {
        if (column < row) {
            const QSignalBlocker block(state_);
            state_->item(column, row)->setText(state_->item(row, column)->text());
        }
    });
    auto *gravity = new FormGroup(QStringLiteral("协方差传播"), "hpopCovarianceGravitySection");
    fields_layout->addWidget(gravity);
    independent_ = new QCheckBox(QStringLiteral("使用独立重力阶次"));
    independent_->setObjectName("hpopCovarianceIndependentGravity");
    independent_->setChecked(s.independent_gravity);
    gravity->form()->addRow(independent_);
    degree_ = new QSpinBox;
    degree_->setObjectName("hpopCovarianceDegree");
    degree_->setRange(0, core::gravity_max_degree);
    degree_->setValue(s.gravity.degree);
    order_ = new QSpinBox;
    order_->setObjectName("hpopCovarianceOrder");
    order_->setRange(0, core::gravity_max_degree);
    order_->setValue(s.gravity.order);
    gravity->form()->addRow(QStringLiteral("最大阶数"), degree_);
    gravity->form()->addRow(QStringLiteral("最大次数"), order_);
    tolerance_ = new QLineEdit(QString::number(s.absolute_tolerance, 'g', 17));
    tolerance_->setObjectName("hpopCovarianceTolerance");
    gravity->form()->addRow(QStringLiteral("变分绝对容差"), tolerance_);
    gravity->add_help(QStringLiteral("独立阶次只影响协方差方程，名义轨道沿用力模型设置。"
                                     "支持 0×0 或至少 2 阶，次数不得超过阶数和重力文件范围。"));
    auto *parameters = new FormGroup(QStringLiteral("考虑参数"), "hpopConsiderSection");
    fields_layout->addWidget(parameters);
    consider_ = new QCheckBox(QStringLiteral("包含考虑参数分析"));
    consider_->setObjectName("hpopConsiderAnalysis");
    consider_->setChecked(s.consider_analysis);
    parameters->form()->addRow(consider_);
    parameters->add_help(QStringLiteral(
        "D = Cd × 阻力面积/质量，K = Cr × 光压面积/质量。Value 是参数方差，单位 m⁴/kg²；"
        "与位置、速度的交叉协方差分别为 m³/kg、m³/(kg·s)。整个联合矩阵必须正定。"));
    parameters_ = new QTableWidget(2, 9);
    setup_table(parameters_, "hpopConsiderTable", QStringLiteral("考虑参数"));
    parameters_->verticalHeader()->hide();
    parameters_->setHorizontalHeaderLabels(
        {QStringLiteral("使用"), QStringLiteral("参数"), "Value", "x", "y", "z", "vx", "vy", "vz"});
    const std::array<const core::HpopConsiderParameter *, 2> values{&s.drag, &s.srp};
    for (int row = 0; row < 2; ++row) {
        auto *use = new QTableWidgetItem;
        use->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        use->setCheckState((row == 0 ? s.drag_enabled : s.srp_enabled) ? Qt::Checked
                                                                       : Qt::Unchecked);
        parameters_->setItem(row, 0, use);
        auto *name = new QTableWidgetItem(row == 0 ? "Drag (D)" : "SRP (K)");
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        parameters_->setItem(row, 1, name);
        const auto &value = *values[static_cast<std::size_t>(row)];
        parameters_->setItem(row, 2, number(value.variance));
        for (int j = 0; j < 6; ++j)
            parameters_->setItem(row, j + 3,
                                 number(value.state_cross_covariance[static_cast<std::size_t>(j)]));
    }
    parameters->form()->addRow(parameters_);
    cross_enabled_ = new QCheckBox(QStringLiteral("包含考虑参数间交叉协方差"));
    cross_enabled_->setObjectName("hpopConsiderCrossCorrelation");
    cross_enabled_->setChecked(s.consider_cross_correlation);
    parameters->form()->addRow(cross_enabled_);
    cross_ = new QTableWidget(1, 3);
    setup_table(cross_, "hpopConsiderCrossTable", QStringLiteral("参数间交叉协方差"));
    cross_->verticalHeader()->hide();
    cross_->setHorizontalHeaderLabels({QStringLiteral("行"), QStringLiteral("列"), "Value"});
    auto *d = new QTableWidgetItem("Drag (D)");
    auto *k = new QTableWidgetItem("SRP (K)");
    for (auto *item : {d, k})
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    cross_->setItem(0, 0, d);
    cross_->setItem(0, 1, k);
    cross_->setItem(0, 2, number(s.drag_srp_covariance));
    parameters->form()->addRow(cross_);
    fields_layout->addStretch();
    connect(enabled_, &QCheckBox::toggled, this, [this] { refresh(); });
    connect(independent_, &QCheckBox::toggled, this, [this] { refresh(); });
    connect(consider_, &QCheckBox::toggled, this, [this] { refresh(); });
    connect(cross_enabled_, &QCheckBox::toggled, this, [this] { refresh(); });
    connect(parameters_, &QTableWidget::cellChanged, this, [this] { refresh(); });
    refresh();
}
void HpopCovarianceEditor::refresh() {
    fields_->setEnabled(enabled_->isChecked() && enabled_->isEnabled());
    degree_->setEnabled(independent_->isChecked());
    order_->setEnabled(independent_->isChecked());
    parameters_->setEnabled(consider_->isChecked());
    const bool both = consider_->isChecked() &&
                      parameters_->item(0, 0)->checkState() == Qt::Checked &&
                      parameters_->item(1, 0)->checkState() == Qt::Checked;
    cross_enabled_->setEnabled(both);
    cross_->setEnabled(both && cross_enabled_->isChecked());
}
QString HpopCovarianceEditor::summary() const {
    return enabled_->isChecked() ? frame_->currentText() : QStringLiteral("未启用");
}
application::HpopCovarianceSettings HpopCovarianceEditor::value() const {
    application::HpopCovarianceSettings s;
    s.enabled = enabled_->isChecked();
    s.coordinates = {static_cast<core::HpopCovarianceFrame>(frame_->currentData().toInt()),
                     static_cast<core::HpopCovarianceVelocity>(velocity_->currentIndex())};
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j <= i; ++j)
            s.state[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                s.state[static_cast<std::size_t>(j)][static_cast<std::size_t>(i)] =
                    read_cell(state_, i, j);
    s.independent_gravity = independent_->isChecked();
    s.gravity = {degree_->value(), order_->value()};
    s.absolute_tolerance = read_number(tolerance_->text(), QStringLiteral("变分绝对容差"));
    s.consider_analysis = consider_->isChecked();
    s.drag_enabled = parameters_->item(0, 0)->checkState() == Qt::Checked;
    s.srp_enabled = parameters_->item(1, 0)->checkState() == Qt::Checked;
    for (int row = 0; row < 2; ++row) {
        auto &p = row == 0 ? s.drag : s.srp;
        p.variance = read_cell(parameters_, row, 2);
        for (int j = 0; j < 6; ++j)
            p.state_cross_covariance[static_cast<std::size_t>(j)] =
                read_cell(parameters_, row, j + 3);
    }
    s.consider_cross_correlation = cross_enabled_->isChecked();
    s.drag_srp_covariance = read_cell(cross_, 0, 2);
    return s;
}
} // namespace orbitforge::desktop
