#include "hpop_covariance_editor.hpp"
#include "../ui_text.hpp"
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
        throw std::invalid_argument(
            (label + ui::text("hpop_covariance_editor.validation")).toStdString());
    return value;
}
double read_cell(const QTableWidget *table, int row, int column, bool nonnegative = false) {
    const auto value = read_number(
        table->item(row, column)->text(),
        QStringLiteral("%1 [%2, %3]").arg(table->accessibleName()).arg(row + 1).arg(column + 1));
    if (nonnegative && value < 0)
        throw std::invalid_argument(ui::text("hpop_covariance_editor.validation_2")
                                        .arg(table->accessibleName())
                                        .arg(row + 1)
                                        .arg(column + 1)
                                        .toStdString());
    return value;
}
template <class Read> double read_draft(double previous, bool active, const Read &read) {
    try {
        return read();
    } catch (const std::invalid_argument &) {
        if (active)
            throw;
        return previous;
    }
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
    : QWidget(parent), initial_(s) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    enabled_ = new QCheckBox(ui::text("hpop_covariance_editor.layout"));
    enabled_->setObjectName("hpopComputeCovariance");
    make_switch(enabled_);
    enabled_->setChecked(s.enabled);
    enabled_->setEnabled(supported);
    layout->addWidget(enabled_);
    if (!supported)
        layout->addWidget(caution_label(ui::text("hpop_covariance_editor.layout.add_widget")));
    fields_ = new QWidget;
    auto *fields_layout = new QVBoxLayout(fields_);
    fields_layout->setContentsMargins(0, 0, 0, 0);
    fields_layout->setSpacing(12);
    layout->addWidget(fields_);
    auto *initial =
        new FormGroup(ui::text("hpop_covariance_editor.initial"), "hpopInitialCovariance");
    fields_layout->addWidget(initial);
    initial->add_help(ui::text("hpop_covariance_editor.initial.add_help"));
    auto *form = initial->form();
    frame_ = new QComboBox;
    frame_->setObjectName("hpopCovarianceFrame");
    const auto add_frame = [&](const QString &name, core::HpopCovarianceFrame frame) {
        frame_->addItem(name, static_cast<int>(frame));
    };
    add_frame("GCRF", core::HpopCovarianceFrame::gcrf);
    add_frame(ui::utf8("covariance.frame.j2000"), core::HpopCovarianceFrame::fk5_j2000);
    add_frame(ui::utf8("covariance.frame.true_of_date"), core::HpopCovarianceFrame::true_of_date);
    add_frame(ui::utf8("covariance.frame.lvlh"), core::HpopCovarianceFrame::lvlh);
    add_frame(ui::utf8("covariance.frame.frenet"), core::HpopCovarianceFrame::frenet);
    frame_->setCurrentIndex(frame_->findData(static_cast<int>(s.coordinates.frame)));
    form->addRow(ui::text("hpop_covariance_editor.form.add_row"), frame_);
    form->addRow(ui::text("hpop_covariance_editor.form.add_row_2"),
                 new QLabel(ui::utf8("covariance.cartesian")));
    velocity_ = new QComboBox;
    velocity_->setObjectName("hpopCovarianceVelocity");
    velocity_->addItems(
        {ui::text("hpop_covariance_editor.label"), ui::text("hpop_covariance_editor.label_2")});
    velocity_->setCurrentIndex(static_cast<int>(s.coordinates.velocity));
    form->addRow(ui::text("hpop_covariance_editor.form.add_row_3"), velocity_);
    initial->add_help(ui::text("hpop_covariance_editor.initial.add_help_2"));
    state_ = new QTableWidget(6, 6);
    setup_table(state_, "hpopCovarianceMatrix", ui::text("hpop_covariance_editor.label_3"));
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
    auto *gravity =
        new FormGroup(ui::text("hpop_covariance_editor.gravity"), "hpopCovarianceGravitySection");
    fields_layout->addWidget(gravity);
    independent_ = new QCheckBox(ui::text("hpop_covariance_editor.label_4"));
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
    gravity->form()->addRow(ui::text("hpop_covariance_editor.label_5"), degree_);
    gravity->form()->addRow(ui::text("hpop_covariance_editor.label_6"), order_);
    tolerance_ = new QLineEdit(QString::number(s.absolute_tolerance, 'g', 17));
    tolerance_->setObjectName("hpopCovarianceTolerance");
    gravity->form()->addRow(ui::text("hpop_covariance_editor.label_7"), tolerance_);
    gravity->add_help(ui::text("hpop_covariance_editor.gravity.add_help"));
    auto *parameters =
        new FormGroup(ui::text("hpop_covariance_editor.parameters"), "hpopConsiderSection");
    fields_layout->addWidget(parameters);
    consider_ = new QCheckBox(ui::text("hpop_covariance_editor.label_8"));
    consider_->setObjectName("hpopConsiderAnalysis");
    consider_->setChecked(s.consider_analysis);
    parameters->form()->addRow(consider_);
    parameters->add_help(ui::text("hpop_covariance_editor.parameters.add_help"));
    parameters_ = new QTableWidget(2, 9);
    setup_table(parameters_, "hpopConsiderTable", ui::text("hpop_covariance_editor.parameters"));
    parameters_->verticalHeader()->hide();
    parameters_->setHorizontalHeaderLabels(
        {ui::text("hpop_covariance_editor.label_9"), ui::text("hpop_covariance_editor.label_10"),
         ui::utf8("covariance.value"), "x", "y", "z", "vx", "vy", "vz"});
    const std::array<const core::HpopConsiderParameter *, 2> values{&s.drag, &s.srp};
    for (int row = 0; row < 2; ++row) {
        auto *use = new QTableWidgetItem;
        use->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        use->setCheckState((row == 0 ? s.drag_enabled : s.srp_enabled) ? Qt::Checked
                                                                       : Qt::Unchecked);
        parameters_->setItem(row, 0, use);
        auto *name = new QTableWidgetItem(row == 0 ? ui::utf8("covariance.drag")
                                                   : ui::utf8("covariance.srp"));
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        parameters_->setItem(row, 1, name);
        const auto &value = *values[static_cast<std::size_t>(row)];
        parameters_->setItem(row, 2, number(value.variance));
        for (int j = 0; j < 6; ++j)
            parameters_->setItem(row, j + 3,
                                 number(value.state_cross_covariance[static_cast<std::size_t>(j)]));
    }
    parameters->form()->addRow(parameters_);
    cross_enabled_ = new QCheckBox(ui::text("hpop_covariance_editor.label_11"));
    cross_enabled_->setObjectName("hpopConsiderCrossCorrelation");
    cross_enabled_->setChecked(s.consider_cross_correlation);
    parameters->form()->addRow(cross_enabled_);
    cross_ = new QTableWidget(1, 3);
    setup_table(cross_, "hpopConsiderCrossTable", ui::text("hpop_covariance_editor.label_12"));
    cross_->verticalHeader()->hide();
    cross_->setHorizontalHeaderLabels({ui::text("hpop_covariance_editor.label_13"),
                                       ui::text("hpop_covariance_editor.label_14"),
                                       ui::utf8("covariance.value")});
    auto *d = new QTableWidgetItem(ui::utf8("covariance.drag"));
    auto *k = new QTableWidgetItem(ui::utf8("covariance.srp"));
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
    const QSignalBlocker block(parameters_);
    for (int row = 0; row < 2; ++row) {
        const bool used = parameters_->item(row, 0)->checkState() == Qt::Checked;
        for (int column = 2; column < parameters_->columnCount(); ++column)
            parameters_->item(row, column)
                ->setFlags(used ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable
                                : Qt::ItemIsSelectable);
    }
    const bool both = consider_->isChecked() &&
                      parameters_->item(0, 0)->checkState() == Qt::Checked &&
                      parameters_->item(1, 0)->checkState() == Qt::Checked;
    cross_enabled_->setEnabled(both);
    cross_->setEnabled(both && cross_enabled_->isChecked());
}
QString HpopCovarianceEditor::summary() const {
    return enabled_->isChecked() ? frame_->currentText() : ui::text("common.disabled");
}
application::HpopCovarianceSettings HpopCovarianceEditor::value() const {
    auto s = initial_;
    s.enabled = enabled_->isChecked();
    s.coordinates = {static_cast<core::HpopCovarianceFrame>(frame_->currentData().toInt()),
                     static_cast<core::HpopCovarianceVelocity>(velocity_->currentIndex())};
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j <= i; ++j)
            s.state[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                s.state[static_cast<std::size_t>(j)][static_cast<std::size_t>(i)] =
                    read_draft(s.state[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)],
                               s.enabled, [&] { return read_cell(state_, i, j, i == j); });
    s.independent_gravity = independent_->isChecked();
    const core::HpopGravityTruncation gravity{degree_->value(), order_->value()};
    if ((gravity.degree == 0 || gravity.degree >= 2) && gravity.order <= gravity.degree)
        s.gravity = gravity;
    else if (s.enabled && s.independent_gravity)
        throw std::invalid_argument(ui::utf8("hpop_covariance_editor.validation_3"));
    s.absolute_tolerance = read_draft(s.absolute_tolerance, s.enabled, [&] {
        const double value =
            read_number(tolerance_->text(), ui::text("hpop_covariance_editor.label_7"));
        if (value < 1e-16 || value > 1)
            throw std::invalid_argument(ui::utf8("hpop_covariance_editor.validation_4"));
        return value;
    });
    s.consider_analysis = consider_->isChecked();
    s.drag_enabled = parameters_->item(0, 0)->checkState() == Qt::Checked;
    s.srp_enabled = parameters_->item(1, 0)->checkState() == Qt::Checked;
    for (int row = 0; row < 2; ++row) {
        auto &p = row == 0 ? s.drag : s.srp;
        const bool active =
            s.enabled && s.consider_analysis && (row == 0 ? s.drag_enabled : s.srp_enabled);
        p.variance =
            read_draft(p.variance, active, [&] { return read_cell(parameters_, row, 2, true); });
        for (int j = 0; j < 6; ++j)
            p.state_cross_covariance[static_cast<std::size_t>(j)] =
                read_draft(p.state_cross_covariance[static_cast<std::size_t>(j)], active,
                           [&] { return read_cell(parameters_, row, j + 3); });
    }
    s.consider_cross_correlation = cross_enabled_->isChecked();
    s.drag_srp_covariance = read_draft(s.drag_srp_covariance,
                                       s.enabled && s.consider_analysis && s.drag_enabled &&
                                           s.srp_enabled && s.consider_cross_correlation,
                                       [&] { return read_cell(cross_, 0, 2); });
    return s;
}
} // namespace orbitforge::desktop
