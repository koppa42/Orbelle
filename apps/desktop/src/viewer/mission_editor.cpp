#include "mission_editor.hpp"
#include "hpop_settings.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <cmath>

namespace orbitforge::desktop {
namespace {
class MissionEditor final : public QDialog {
  public:
    core::MissionPlan plan;
    core::HpopOptions hpop;
    MissionEditor(core::MissionPlan draft, core::HpopOptions options, core::Epoch initial,
                  core::Epoch scenario_first, core::Epoch scenario_last, QWidget *parent)
        : QDialog(parent), plan(std::move(draft)), hpop(options), initial_(initial),
          first_(scenario_first), last_(scenario_last) {
        setObjectName("missionEditorDialog");
        setWindowTitle(QStringLiteral("机动规划 · 任务序列"));
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(24, 22, 24, 20);
        outer->setSpacing(16);
        auto *title = new QLabel(QStringLiteral("机动规划"));
        title->setObjectName("panelTitle");
        outer->addWidget(title);
        auto *intro = new QLabel(QStringLiteral("从卫星初始状态出发，按顺序滑行与施加脉冲。"));
        intro->setObjectName("hint");
        intro->setWordWrap(true);
        outer->addWidget(intro);

        auto *model_row = new QHBoxLayout;
        model_row->setSpacing(12);
        model_row->addWidget(new QLabel(QStringLiteral("滑行模型")));
        model_ = new QComboBox;
        model_->setObjectName("missionCoastModel");
        model_->addItems({QStringLiteral("二体"), QStringLiteral("HPOP · 数值积分")});
        model_->setCurrentIndex(plan.coast_model == core::MissionCoastModel::hpop ? 1 : 0);
        model_row->addWidget(model_, 1);
        auto *settings = button(QStringLiteral("力模型与积分…"), "missionHpopSettings");
        settings->setEnabled(model_->currentIndex() == 1);
        model_row->addWidget(settings);
        outer->addLayout(model_row);
        connect(settings, &QPushButton::clicked, this, [this] { edit_hpop_settings(hpop, this); });
        connect(model_, &QComboBox::currentIndexChanged, this, [this, settings](int index) {
            plan.coast_model =
                index == 0 ? core::MissionCoastModel::two_body : core::MissionCoastModel::hpop;
            settings->setEnabled(index == 1);
        });

        auto *body = new QWidget;
        auto *columns = new QHBoxLayout(body);
        columns->setContentsMargins(0, 0, 0, 0);
        columns->setSpacing(20);
        outer->addWidget(body, 1);
        auto *sequence = new QWidget;
        auto *left = new QVBoxLayout(sequence);
        left->setContentsMargins(0, 0, 0, 0);
        left->setSpacing(10);
        auto *initial_label = new QLabel(QStringLiteral("起点  ·  卫星初始状态"));
        initial_label->setObjectName("sectionTitle");
        left->addWidget(initial_label);
        list_ = new QListWidget;
        list_->setObjectName("missionSegments");
        list_->setSpacing(5);
        list_->setMinimumWidth(0);
        list_->setWordWrap(true);
        list_->setResizeMode(QListView::Adjust);
        list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        left->addWidget(list_, 1);
        auto *add_row = new QHBoxLayout;
        auto *coast = button(QStringLiteral("＋ 滑行"), "missionAddCoast");
        auto *impulse = button(QStringLiteral("＋ 脉冲"), "missionAddImpulse");
        add_row->addWidget(coast);
        add_row->addWidget(impulse);
        left->addLayout(add_row);
        auto *tools = new QHBoxLayout;
        up_ = button(QStringLiteral("上移"), "missionMoveUp");
        down_ = button(QStringLiteral("下移"), "missionMoveDown");
        remove_ = button(QStringLiteral("删除"), "missionRemove");
        for (auto *b : {up_, down_, remove_})
            tools->addWidget(b);
        left->addLayout(tools);
        columns->addWidget(sequence, 2);

        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *details = new QWidget;
        auto *right = new QVBoxLayout(details);
        right->setContentsMargins(4, 0, 8, 8);
        right->setSpacing(16);
        heading_ = new QLabel;
        heading_->setObjectName("sectionTitle");
        right->addWidget(heading_);
        auto *form = new QFormLayout;
        form->setVerticalSpacing(14);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        name_ = new QLineEdit;
        name_->setObjectName("missionSegmentName");
        name_->setMaxLength(16384);
        name_->setPlaceholderText(QStringLiteral("可选名称"));
        form->addRow(QStringLiteral("名称"), name_);
        right->addLayout(form);
        pages_ = new QStackedWidget;
        auto *coast_page = new QWidget;
        auto *coast_form = new QFormLayout(coast_page);
        coast_form->setContentsMargins(0, 0, 0, 0);
        coast_form->setVerticalSpacing(14);
        coast_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        duration_ = new QuantityEditor(Unit::minute);
        duration_->setObjectName("missionCoastDuration");
        duration_->set_label(QStringLiteral("滑行时长"));
        duration_->set_range(1e-9, 366 * 86400.0, Unit::second);
        coast_form->addRow(QStringLiteral("时长"), duration_);
        coast_form->addRow(note(QStringLiteral("继承上一段的末状态，传播指定的时长。")));
        pages_->addWidget(coast_page);
        auto *impulse_page = new QWidget;
        auto *impulse_form = new QFormLayout(impulse_page);
        impulse_form->setContentsMargins(0, 0, 0, 0);
        impulse_form->setVerticalSpacing(14);
        impulse_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        frame_ = new QComboBox;
        frame_->setObjectName("missionManeuverFrame");
        frame_->addItems({"GCRF", "VNC", "LVLH"});
        impulse_form->addRow(QStringLiteral("方向参考系"), frame_);
        for (std::size_t i = 0; i < 3; ++i) {
            dv_[i] = new QuantityEditor(Unit::metres_per_second);
            dv_[i]->setObjectName(QStringLiteral("missionDeltaV%1").arg(i));
            dv_[i]->set_label(QStringLiteral("Δv %1").arg(i + 1));
            axes_[i] = new QLabel;
            impulse_form->addRow(axes_[i], dv_[i]);
        }
        impulse_form->addRow(
            note(QStringLiteral("脉冲瞬间改变速度，位置保持连续；方向取点火前的状态。")));
        pages_->addWidget(impulse_page);
        right->addWidget(pages_);
        right->addStretch();
        scroll->setWidget(details);
        columns->addWidget(scroll, 3);
        for (auto *f : {form, coast_form, impulse_form})
            f->setRowWrapPolicy(QFormLayout::WrapLongRows);
        new WidthObserver(body, [this, columns, sequence, mode = WidthMode{}](int width) mutable {
            const bool narrow = mode.narrow(width, 650);
            columns->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
            sequence->setMaximumHeight(narrow ? 250 : QWIDGETSIZE_MAX);
            sequence->setMinimumHeight(narrow ? 210 : 0);
            QTimer::singleShot(0, list_, [this] {
                if (list_->currentItem())
                    list_->scrollToItem(list_->currentItem());
            });
        });
        summary_ = new QLabel;
        summary_->setObjectName("missionSummary");
        summary_->setWordWrap(true);
        outer->addWidget(summary_);
        error_ = new QLabel;
        error_->setObjectName("errorText");
        error_->setWordWrap(true);
        error_->hide();
        outer->addWidget(error_);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存计划"));
        buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        outer->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (!store_selected())
                return;
            try {
                const auto end = core::validate_mission(initial_, plan);
                if (initial_.tai_nanoseconds() > first_.tai_nanoseconds())
                    throw std::invalid_argument("初始历元不能晚于场景起点。");
                if (end.tai_nanoseconds() < last_.tai_nanoseconds())
                    throw std::invalid_argument("计划结束早于场景结束，请延长滑行段。");
                if (plan.coast_model == core::MissionCoastModel::hpop)
                    core::validate_hpop_options(hpop);
                accept();
            } catch (const std::exception &e) {
                show_error(e.what());
            }
        });
        connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
            if (!store_selected()) {
                const QSignalBlocker block(list_);
                list_->setCurrentRow(selected_);
                return;
            }
            selected_ = row;
            load_selected();
        });
        connect(frame_, &QComboBox::currentIndexChanged, this, [this] { update_axes(); });
        connect(coast, &QPushButton::clicked, this,
                [this] { add(core::MissionSegment::coast(std::chrono::minutes{10})); });
        connect(impulse, &QPushButton::clicked, this,
                [this] { add(core::MissionSegment::impulse({0, 0, 0})); });
        connect(up_, &QPushButton::clicked, this, [this] { move(-1); });
        connect(down_, &QPushButton::clicked, this, [this] { move(1); });
        connect(remove_, &QPushButton::clicked, this, [this] {
            if (selected_ < 0)
                return;
            plan.segments.erase(plan.segments.begin() + selected_);
            rebuild(std::min(selected_, static_cast<int>(plan.segments.size()) - 1));
        });
        connect(name_, &QLineEdit::textEdited, this, [this] { store_selected(); });
        connect(duration_, &QuantityEditor::valueChanged, this, [this] {
            if (!loading_)
                store_selected();
        });
        for (auto *value : dv_)
            connect(value, &QuantityEditor::valueChanged, this, [this] {
                if (!loading_)
                    store_selected();
            });
        rebuild(plan.segments.empty() ? -1 : 0);
        const auto available = screen()->availableGeometry().size();
        resize(std::min(900, available.width() - 32), std::min(690, available.height() - 64));
    }

  private:
    core::Epoch initial_, first_, last_;
    int selected_ = -1;
    bool loading_ = false;
    QListWidget *list_;
    QComboBox *model_, *frame_;
    QLineEdit *name_;
    QStackedWidget *pages_;
    QuantityEditor *duration_;
    std::array<QuantityEditor *, 3> dv_{};
    std::array<QLabel *, 3> axes_{};
    QLabel *heading_, *summary_, *error_;
    QPushButton *up_, *down_, *remove_;
    static QPushButton *button(const QString &text, const char *id) {
        auto *b = new QPushButton(text);
        b->setObjectName(id);
        b->setAutoDefault(false);
        return b;
    }
    static QLabel *note(const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("hint");
        label->setWordWrap(true);
        return label;
    }
    void show_error(const char *message) {
        error_->setText(QString::fromUtf8(message));
        error_->show();
    }
    void update_axes() {
        const QStringList names =
            frame_->currentIndex() == 1
                ? QStringList{QStringLiteral("Δv · 速度 V"), QStringLiteral("Δv · 法向 N"),
                              QStringLiteral("Δv · 共法向 C")}
            : frame_->currentIndex() == 2
                ? QStringList{QStringLiteral("Δv · 径向 R"), QStringLiteral("Δv · 横向 T"),
                              QStringLiteral("Δv · 法向 N")}
                : QStringList{QStringLiteral("Δv · X"), QStringLiteral("Δv · Y"),
                              QStringLiteral("Δv · Z")};
        for (int i = 0; i < 3; ++i)
            axes_[static_cast<std::size_t>(i)]->setText(names[i]);
    }
    bool store_selected() {
        if (loading_ || selected_ < 0)
            return true;
        try {
            auto segment = plan.segments.at(static_cast<std::size_t>(selected_));
            segment.name = name_->text().trimmed().toStdString();
            if (segment.kind == core::MissionSegmentKind::coast) {
                const double seconds = duration_->value(Unit::second);
                // Opening and accepting must preserve an exact saved int64 duration,
                // even when its last nanoseconds cannot be represented by a double.
                if (seconds != static_cast<double>(segment.duration_nanoseconds) * 1e-9)
                    segment.duration_nanoseconds = std::llround(seconds * 1e9);
                if (segment.duration_nanoseconds <= 0)
                    throw std::invalid_argument("滑行时长至少为 1 ns。");
            } else {
                segment.frame = static_cast<core::ManeuverFrame>(frame_->currentIndex());
                for (std::size_t i = 0; i < 3; ++i)
                    segment.delta_v_mps[i] = dv_[i]->value(Unit::metres_per_second);
            }
            plan.segments[static_cast<std::size_t>(selected_)] = std::move(segment);
            refresh_summary();
            error_->hide();
            return true;
        } catch (const std::exception &e) {
            show_error(e.what());
            return false;
        }
    }
    void load_selected() {
        loading_ = true;
        const bool selected = selected_ >= 0;
        name_->setEnabled(selected);
        pages_->setEnabled(selected);
        up_->setEnabled(selected_ > 0);
        down_->setEnabled(selected && selected_ + 1 < static_cast<int>(plan.segments.size()));
        remove_->setEnabled(selected);
        heading_->setText(QStringLiteral("选择或添加一个段"));
        if (selected) {
            const auto &segment = plan.segments.at(static_cast<std::size_t>(selected_));
            const bool coast = segment.kind == core::MissionSegmentKind::coast;
            heading_->setText(coast ? QStringLiteral("滑行段") : QStringLiteral("脉冲机动"));
            name_->setText(QString::fromStdString(segment.name));
            pages_->setCurrentIndex(coast ? 0 : 1);
            if (coast)
                duration_->set_value(static_cast<double>(segment.duration_nanoseconds) * 1e-9,
                                     Unit::second);
            else {
                frame_->setCurrentIndex(static_cast<int>(segment.frame));
                for (std::size_t i = 0; i < 3; ++i)
                    dv_[i]->set_value(segment.delta_v_mps[i], Unit::metres_per_second);
            }
        }
        update_axes();
        loading_ = false;
    }
    void refresh_summary() {
        double seconds = 0, total = 0;
        for (std::size_t i = 0; i < plan.segments.size(); ++i) {
            const auto &s = plan.segments[i];
            const bool coast = s.kind == core::MissionSegmentKind::coast;
            const double dv = std::hypot(s.delta_v_mps[0], s.delta_v_mps[1], s.delta_v_mps[2]);
            if (coast)
                seconds += static_cast<double>(s.duration_nanoseconds) * 1e-9;
            else
                total += dv;
            const auto label = s.name.empty()
                                   ? (coast ? QStringLiteral("滑行") : QStringLiteral("脉冲机动"))
                                   : QString::fromStdString(s.name);
            list_->item(static_cast<int>(i))
                ->setText(
                    QStringLiteral("%1   %2\n       %3")
                        .arg(i + 1, 2, 10, QChar('0'))
                        .arg(label, coast ? QStringLiteral("%1 min").arg(
                                                static_cast<double>(s.duration_nanoseconds) / 6e10,
                                                0, 'g', 8)
                                          : QStringLiteral("Δv  %1 m/s").arg(dv, 0, 'g', 8)));
        }
        const auto remaining = last_.seconds_since(initial_) - seconds;
        summary_->setText(
            QStringLiteral("%1 段   ·   总时长 %2 min   ·   总 Δv %3 m/s%4")
                .arg(plan.segments.size())
                .arg(seconds / 60, 0, 'g', 8)
                .arg(total, 0, 'g', 8)
                .arg(remaining > 1e-8
                         ? QStringLiteral("\n距场景结束还差 %1 min").arg(remaining / 60, 0, 'g', 8)
                         : QString{}));
    }
    void rebuild(int selected) {
        const QSignalBlocker block(list_);
        list_->clear();
        for (const auto &segment : plan.segments) {
            auto *item = new QListWidgetItem(list_);
            item->setIcon(workbench_icon(segment.kind == core::MissionSegmentKind::coast
                                             ? WorkbenchIcon::orbit
                                             : WorkbenchIcon::satellite));
            item->setSizeHint(QSize(0, 64));
        }
        selected_ = selected;
        list_->setCurrentRow(selected);
        refresh_summary();
        load_selected();
    }
    void add(core::MissionSegment segment) {
        if (!store_selected())
            return;
        if (plan.segments.size() >= 256) {
            show_error("最多可添加 256 个段。");
            return;
        }
        const int next = selected_ + 1;
        plan.segments.insert(plan.segments.begin() + next, std::move(segment));
        rebuild(next);
    }
    void move(int direction) {
        if (!store_selected())
            return;
        const int next = selected_ + direction;
        if (selected_ < 0 || next < 0 || next >= static_cast<int>(plan.segments.size()))
            return;
        std::swap(plan.segments[static_cast<std::size_t>(selected_)],
                  plan.segments[static_cast<std::size_t>(next)]);
        rebuild(next);
    }
};
} // namespace
bool edit_mission(core::MissionPlan &plan, core::HpopOptions &hpop, core::Epoch initial,
                  core::Epoch scenario_first, core::Epoch scenario_last, QWidget *parent) {
    MissionEditor dialog(plan, hpop, initial, scenario_first, scenario_last, parent);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    plan = std::move(dialog.plan);
    hpop = dialog.hpop;
    return true;
}
} // namespace orbitforge::desktop
