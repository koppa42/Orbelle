#include "report_creation.hpp"
#include "../ui_text.hpp"
#include <QAction>
#include <QCursor>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHelpEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace orbelle::desktop {
using namespace application;
QString report_kind_name(ReportKind kind) {
    switch (kind) {
    case ReportKind::scenario_summary:
        return ui::text("report_dialog.label");
    case ReportKind::object_summary:
        return ui::text("report_dialog.label_2");
    case ReportKind::cartesian_state:
        return ui::text("report_dialog.label_3");
    case ReportKind::ground_stations:
        return ui::text("report_dialog.label_4");
    case ReportKind::classical_elements:
        return ui::text("report_dialog.label_5");
    case ReportKind::geodetic_position:
        return ui::text("report_dialog.label_6");
    case ReportKind::orbital_motion:
        return ui::text("report_dialog.label_7");
    case ReportKind::access_intervals:
        return ui::text("report_dialog.label_8");
    case ReportKind::aer:
        return ui::text("report_dialog.label_9");
    case ReportKind::lighting_intervals:
        return ui::text("report_dialog.label_10");
    case ReportKind::lighting_state:
        return ui::text("report_dialog.label_11");
    case ReportKind::orbital_events:
        return ui::text("report_dialog.label_12");
    case ReportKind::relative_motion:
        return ui::text("report_dialog.label_13");
    case ReportKind::intersatellite_access:
        return ui::text("report_dialog.label_14");
    case ReportKind::coverage_grid:
        return ui::text("report_dialog.label_15");
    case ReportKind::coverage_time:
        return ui::text("report_dialog.label_16");
    case ReportKind::state_covariance:
        return ui::text("report_dialog.label_17");
    }
    return {};
}
ReportChoice report_kind_choice(ReportKind kind) {
    const auto description = [](ReportKind kind) -> QString {
        switch (kind) {
        case ReportKind::scenario_summary:
            return ui::text("report_dialog.label_121");
        case ReportKind::object_summary:
            return ui::text("report_dialog.label_122");
        case ReportKind::cartesian_state:
            return ui::text("report_dialog.label_123");
        case ReportKind::ground_stations:
            return ui::text("report_dialog.label_124");
        case ReportKind::classical_elements:
            return ui::text("report_dialog.label_125");
        case ReportKind::geodetic_position:
            return ui::text("report_dialog.label_126");
        case ReportKind::orbital_motion:
            return ui::text("report_dialog.label_127");
        case ReportKind::access_intervals:
            return ui::text("report_dialog.label_128");
        case ReportKind::aer:
            return ui::text("report_dialog.label_129");
        case ReportKind::lighting_intervals:
            return ui::text("report_dialog.label_130");
        case ReportKind::lighting_state:
            return ui::text("report_dialog.label_131");
        case ReportKind::orbital_events:
            return ui::text("report_dialog.label_132");
        case ReportKind::relative_motion:
            return ui::text("report_dialog.label_133");
        case ReportKind::intersatellite_access:
            return ui::text("report_dialog.label_134");
        case ReportKind::coverage_grid:
            return ui::text("report_dialog.label_135");
        case ReportKind::coverage_time:
            return ui::text("report_dialog.label_136");
        case ReportKind::state_covariance:
            return ui::text("report_dialog.label_137");
        }
        return {};
    };
    const auto output = [](ReportKind kind) -> QString {
        switch (kind) {
        case ReportKind::coverage_grid:
            return ui::text("report_dialog.output");
        case ReportKind::coverage_time:
            return ui::text("report_dialog.label_138");
        case ReportKind::scenario_summary:
            return ui::text("report_dialog.label_139");
        case ReportKind::object_summary:
            return ui::text("report_dialog.label_140");
        case ReportKind::ground_stations:
            return ui::text("report_dialog.label_141");
        case ReportKind::access_intervals:
        case ReportKind::intersatellite_access:
            return ui::text("report_dialog.label_142");
        case ReportKind::lighting_intervals:
            return ui::text("report_dialog.label_143");
        case ReportKind::orbital_events:
            return ui::text("report_dialog.label_144");
        default:
            return ui::text("report_dialog.label_145");
        }
    };
    const auto icon = [](ReportKind kind) {
        switch (kind) {
        case ReportKind::scenario_summary:
            return WorkbenchIcon::earth;
        case ReportKind::object_summary:
            return WorkbenchIcon::satellite;
        case ReportKind::ground_stations:
            return WorkbenchIcon::ground_station;
        case ReportKind::coverage_grid:
        case ReportKind::coverage_time:
        case ReportKind::geodetic_position:
            return WorkbenchIcon::map;
        case ReportKind::access_intervals:
            return WorkbenchIcon::clock;
        case ReportKind::aer:
            return WorkbenchIcon::focus;
        default:
            return WorkbenchIcon::orbit;
        }
    };
    return {QStringLiteral("builtin:%1").arg(static_cast<int>(kind)),
            report_kind_name(kind),
            report_kind_name(kind),
            description(kind),
            {},
            false,
            {},
            output(kind),
            {},
            icon(kind)};
}
std::vector<ObjectChoice> report_object_choices(const Project &project, ReportKind kind) {
    auto choices =
        project_object_choices(project, {ObjectKind::satellite, ObjectKind::ground_station});
    if (kind == ReportKind::scenario_summary)
        choices.clear();
    else if (report_is_coverage(kind))
        choices = project_object_choices(project, {ObjectKind::satellite, ObjectKind::sensor});
    else if (kind == ReportKind::ground_stations)
        choices = project_object_choices(project, {ObjectKind::ground_station});
    else if (report_satellites_only(kind))
        choices = project_object_choices(project, {ObjectKind::satellite});
    if (kind == ReportKind::state_covariance)
        std::erase_if(choices, [&](const auto &object) {
            return std::ranges::none_of(project.satellites, [&](const auto &satellite) {
                return satellite.orbit.object_id == object.id &&
                       satellite.orbit.computes_covariance();
            });
        });
    return choices;
}
namespace {
class SelectedObjectDelegate final : public QStyledItemDelegate {
  public:
    SelectedObjectDelegate(std::function<void(QString)> remove, QObject *parent)
        : QStyledItemDelegate(parent), remove_(std::move(remove)) {}
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return {120, std::max(36, option.fontMetrics.height() + 16)};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const auto row = option.rect;
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hover = option.state.testFlag(QStyle::State_MouseOver);
        if (selected || hover)
            painter->fillRect(row,
                              theme::color(selected ? theme::accent_hover : theme::field_muted));
        painter->setPen(theme::color(theme::line, 140));
        painter->drawLine(row.bottomLeft() + QPoint(12, 0), row.bottomRight() - QPoint(12, 0));
        qvariant_cast<QIcon>(index.data(Qt::DecorationRole))
            .paint(painter, QRect(row.left() + 12, row.center().y() - 8, 16, 16));
        const auto remove = remove_rect(row);
        const QRect type(remove.left() - 56, row.top(), 48, row.height());
        const QRect name(row.left() + 38, row.top(), std::max(0, type.left() - row.left() - 48),
                         row.height());
        const auto foreground = index.data(Qt::ForegroundRole);
        painter->setPen(foreground.isValid() ? qvariant_cast<QBrush>(foreground).color()
                                             : theme::color(theme::text));
        painter->setFont(option.font);
        painter->drawText(
            name, Qt::AlignVCenter | Qt::AlignLeft,
            option.fontMetrics.elidedText(index.data().toString(), Qt::ElideMiddle, name.width()));
        auto small = option.font;
        small.setPixelSize(11);
        painter->setFont(small);
        painter->setPen(theme::color(theme::text_muted));
        painter->drawText(type, Qt::AlignVCenter | Qt::AlignRight,
                          index.data(Qt::UserRole + 1).toString());
        auto *list = static_cast<QListWidget *>(parent());
        const bool remove_hover =
            hover && remove.contains(list->viewport()->mapFromGlobal(QCursor::pos()));
        if (remove_hover) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme::color(theme::overlay));
            painter->drawRoundedRect(remove.adjusted(2, 2, -2, -2), 4, 4);
        }
        workbench_icon(WorkbenchIcon::close,
                       theme::color(remove_hover ? theme::text : theme::text_muted))
            .paint(painter, remove.adjusted(8, 8, -8, -8));
        painter->restore();
    }
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override {
        if (remove_rect(option.rect).contains(event->pos())) {
            QToolTip::showText(
                event->globalPos(),
                ui::text("report_creation.remove_named").arg(index.data().toString()), view);
            return true;
        }
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override {
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton &&
                remove_rect(option.rect).contains(mouse->position().toPoint())) {
                remove_(index.data(Qt::UserRole).toString());
                return true;
            }
        }
        return false;
    }

  private:
    static QRect remove_rect(const QRect &row) {
        return {row.right() - 29, row.center().y() - 14, 28, 28};
    }
    std::function<void(QString)> remove_;
};

bool has_object(const std::vector<ObjectChoice> &objects, const std::string &id) {
    return std::ranges::any_of(
        objects, [&](const auto &object) { return object.selectable && object.id == id; });
}
bool pair_report(ReportKind kind) {
    return report_is_access(kind) || report_is_intersatellite(kind);
}
std::vector<ObjectChoice> role_objects(const Project &project, ReportKind kind, int role) {
    if (report_is_coverage(kind))
        return project_object_choices(
            project, role == 0 ? std::initializer_list{ObjectKind::region}
                               : std::initializer_list{ObjectKind::satellite, ObjectKind::sensor});
    if (report_is_intersatellite(kind))
        return project_object_choices(project, {ObjectKind::satellite});
    if (report_is_access(kind))
        return project_object_choices(
            project, role == 0 ? std::initializer_list{ObjectKind::ground_station}
                               : std::initializer_list{ObjectKind::satellite, ObjectKind::sensor});
    return report_object_choices(project, kind);
}
QString role_name(ReportKind kind, int role) {
    if (report_is_coverage(kind))
        return ui::text(role == 0 ? "region.title" : "region.coverage_assets");
    if (report_is_intersatellite(kind))
        return ui::text(role == 0 ? "report_dialog.label_157" : "report_dialog.label_158");
    if (report_is_access(kind))
        return ui::text(role == 0 ? "report_dialog.label_52" : "report_dialog.pair_form.add_row");
    return ui::text("report_creation.objects");
}
} // namespace

struct ReportCreationDialog::Impl {
    ReportCreationDialog *owner;
    Project project;
    std::string context;
    ReportSelectionPanel *types;
    QStackedWidget *pages;
    QLabel *heading, *summary, *progress, *status;
    QPushButton *back, *next;
    QString prepared_key;
    std::vector<ReportChoice> available_choices;
    ReportDefinition draft;
    struct Binding {
        int role;
        ObjectSelectionField *field;
        QLabel *badge;
        QLabel *detail;
        QListWidget *selected_list = nullptr;
        QPushButton *manage = nullptr;
    };
    std::vector<Binding> bindings;
    QSize type_size{840, 620};
    QSize objects_size{560, 400};
    bool rebuilding = false;
    bool template_changed = false;

    Impl(ReportCreationDialog *parent, Project input, std::string object,
         std::optional<ReportKind> preset)
        : owner(parent), project(std::move(input)), context(std::move(object)) {
        owner->setObjectName("reportCreationDialog");
        owner->setWindowTitle(ui::text("report_creation.title"));
        owner->setWindowIcon(workbench_icon(WorkbenchIcon::properties));
        owner->resize(840, 620);
        owner->setMinimumSize(360, 400);
        auto *layout = new QVBoxLayout(owner);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(10);
        progress = new QLabel(owner);
        progress->setObjectName("reportCreationProgress");
        layout->addWidget(progress);
        heading = new QLabel(owner);
        heading->setObjectName("panelTitle");
        heading->setTextFormat(Qt::PlainText);
        heading->setWordWrap(true);
        layout->addWidget(heading);
        summary = new QLabel(owner);
        summary->setObjectName("reportCreationSummary");
        summary->setTextFormat(Qt::PlainText);
        summary->setWordWrap(true);
        summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(summary);
        pages = new QStackedWidget(owner);
        pages->setObjectName("reportCreationPages");
        // Both pages scroll internally; a hidden type list must not impose its height on the form.
        pages->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
        available_choices = choices();
        types = new ReportSelectionPanel(
            available_choices,
            preset ? QStringLiteral("builtin:%1").arg(static_cast<int>(*preset)) : QString{},
            pages);
        pages->addWidget(types);
        layout->addWidget(pages, 1);
        status = new QLabel(owner);
        status->setObjectName("reportCreationStatus");
        status->setWordWrap(true);
        status->setTextFormat(Qt::PlainText);
        status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(status);
        auto *footer = new QHBoxLayout;
        back = new QPushButton(ui::text("report_creation.back"), owner);
        back->setObjectName("reportCreationBack");
        back->setAutoDefault(false);
        back->setIcon(workbench_icon(WorkbenchIcon::arrow_left));
        back->setIconSize({14, 14});
        footer->addWidget(back);
        footer->addStretch();
        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, owner);
        next = buttons->button(QDialogButtonBox::Ok);
        next->setObjectName("reportCreationNext");
        next->setProperty("primary", true);
        next->setDefault(true);
        next->style()->unpolish(next);
        next->style()->polish(next);
        buttons->button(QDialogButtonBox::Cancel)->setText(ui::text("common.cancel"));
        buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
        footer->addWidget(buttons);
        layout->addLayout(footer);
        QObject::connect(buttons, &QDialogButtonBox::accepted, owner,
                         &ReportCreationDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, owner, &QDialog::reject);
        QObject::connect(back, &QPushButton::clicked, owner, [this] { show_page(0); });
        QObject::connect(types, &ReportSelectionPanel::selectionChanged, owner,
                         [this] { refresh(); });
        QObject::connect(types, &ReportSelectionPanel::activated, owner,
                         &ReportCreationDialog::accept);
        refresh();
        focus_search();
    }
    QString object_name(const std::string &id) const {
        const auto objects = project_object_choices(project);
        const auto found = std::ranges::find(objects, id, &ObjectChoice::id);
        return found == objects.end() ? QString::fromStdString(id) + ui::text("report_dialog.found")
                                      : found->path;
    }
    int context_role(ReportKind kind) const {
        if (context.empty())
            return -1;
        if (has_object(role_objects(project, kind, 0), context))
            return 0;
        return (pair_report(kind) || report_is_coverage(kind)) &&
                       has_object(role_objects(project, kind, 1), context)
                   ? 1
                   : -1;
    }
    QString requirements(ReportKind kind) const {
        if (kind == ReportKind::scenario_summary)
            return ui::text("report_creation.scenario_scope");
        if (report_is_intersatellite(kind))
            return ui::text("report_dialog.label_146");
        if (report_is_access(kind))
            return ui::text("report_dialog.label_147");
        if (report_is_coverage(kind))
            return ui::text("report_creation.coverage_scope");
        if (kind == ReportKind::state_covariance)
            return ui::text("report_creation.covariance_scope");
        if (kind == ReportKind::ground_stations)
            return ui::text("report_creation.station_scope");
        if (report_satellites_only(kind))
            return ui::text("report_creation.satellite_scope");
        return ui::text("report_creation.state_scope");
    }
    std::vector<ReportChoice> choices() const {
        std::vector<ReportChoice> result;
        for (const auto kind : report_kinds) {
            if (!context.empty() && context_role(kind) < 0)
                continue;
            auto choice = report_kind_choice(kind);
            choice.scope = requirements(kind);
            if (!context.empty())
                choice.note = ui::text("report_creation.bound_context").arg(object_name(context));
            if (kind != ReportKind::scenario_summary) {
                const auto first = role_objects(project, kind, 0);
                const bool available = std::ranges::any_of(first, &ObjectChoice::selectable) &&
                                       (!(report_is_access(kind) || report_is_coverage(kind)) ||
                                        std::ranges::any_of(role_objects(project, kind, 1),
                                                            &ObjectChoice::selectable)) &&
                                       (!report_is_intersatellite(kind) || first.size() >= 2);
                if (!available)
                    choice.unavailable_reason =
                        ui::text("report_creation.missing_objects").arg(requirements(kind));
            }
            result.push_back(std::move(choice));
        }
        const auto builtins = result;
        for (const auto &definition : project.reports) {
            const auto key =
                QStringLiteral("builtin:%1").arg(static_cast<int>(definition.query.kind));
            const auto base = std::ranges::find(builtins, key, &ReportChoice::key);
            if (base == builtins.end())
                continue;
            if (!context.empty() && context != definition.query.region_id &&
                definition.query.scope != ReportScope::all_compatible &&
                std::ranges::find(definition.query.object_ids, context) ==
                    definition.query.object_ids.end())
                continue;
            auto choice = *base;
            choice.key = "template:" + QString::fromStdString(definition.id);
            choice.name = QString::fromStdString(definition.name);
            choice.saved = true;
            QStringList names;
            if (!definition.query.region_id.empty())
                names << object_name(definition.query.region_id);
            for (const auto &id : definition.query.object_ids)
                names << object_name(id);
            choice.scope = definition.query.scope == ReportScope::all_compatible
                               ? ui::text("report_dialog.label_120")
                               : names.join(QStringLiteral("、"));
            choice.note = context.empty() ? ui::text("report_creation.template_hint") : choice.note;
            result.push_back(std::move(choice));
        }
        return result;
    }
    bool choice_exists() const {
        const auto found =
            std::ranges::find(available_choices, types->selection(), &ReportChoice::key);
        return found != available_choices.end() && found->unavailable_reason.isEmpty();
    }
    bool direct(ReportKind kind) const { return kind == ReportKind::scenario_summary; }
    ReportKind selected_kind() const {
        const auto key = types->selection();
        if (key.startsWith("builtin:"))
            return static_cast<ReportKind>(key.mid(8).toInt());
        for (const auto &definition : project.reports)
            if (key == "template:" + QString::fromStdString(definition.id))
                return definition.query.kind;
        return ReportKind::scenario_summary;
    }
    void focus_search() {
        if (auto *search = pages->currentWidget()->findChild<QLineEdit *>())
            search->setFocus();
        else {
            const auto first = std::ranges::find_if(
                bindings, [this](const auto &binding) { return !valid(binding); });
            if (first != bindings.end())
                (first->field->isHidden() ? static_cast<QWidget *>(first->manage) : first->field)
                    ->setFocus();
            else
                next->setFocus();
        }
    }
    void show_page(int index) {
        const bool resize_window = !owner->isMaximized() && !owner->isFullScreen();
        if (resize_window)
            (pages->currentIndex() == 0 ? type_size : objects_size) = owner->size();
        const auto center = owner->frameGeometry().center();
        pages->setCurrentIndex(index);
        refresh();
        if (resize_window) {
            owner->resize(index == 0 ? type_size : objects_size);
            if (owner->isVisible()) {
                auto frame = owner->frameGeometry();
                frame.moveCenter(center);
                const auto bounds = owner->screen()->availableGeometry();
                frame.moveLeft(
                    std::clamp(frame.left(), bounds.left(),
                               std::max(bounds.left(), bounds.right() - frame.width() + 1)));
                frame.moveTop(
                    std::clamp(frame.top(), bounds.top(),
                               std::max(bounds.top(), bounds.bottom() - frame.height() + 1)));
                owner->move(frame.topLeft());
            }
        }
        focus_search();
    }
    ObjectSelection selection(const Binding &binding) const { return binding.field->selection(); }
    std::vector<ObjectChoice> candidates(const Binding &binding) const {
        auto result = role_objects(project, draft.query.kind, binding.role);
        if (report_is_intersatellite(draft.query.kind)) {
            for (const auto &other : bindings)
                if (other.role != binding.role) {
                    const auto selected = selection(other);
                    std::erase_if(result, [&](const auto &object) {
                        return std::ranges::find(selected.ids, object.id) != selected.ids.end();
                    });
                }
        }
        return result;
    }
    bool valid(const Binding &binding) const {
        const auto selected = selection(binding);
        const auto objects = candidates(binding);
        if (selected.all)
            return !pair_report(draft.query.kind) && !report_is_coverage(draft.query.kind) &&
                   std::ranges::any_of(objects, &ObjectChoice::selectable);
        std::unordered_set<std::string> available;
        for (const auto &object : objects)
            if (object.selectable)
                available.insert(object.id);
        return !selected.ids.empty() &&
               (!(pair_report(draft.query.kind) ||
                  (report_is_coverage(draft.query.kind) && binding.role == 0)) ||
                selected.ids.size() == 1) &&
               std::ranges::all_of(selected.ids,
                                   [&](const auto &id) { return available.contains(id); });
    }
    void refresh() {
        if (rebuilding)
            return;
        const int page = pages->currentIndex();
        const bool valid_choice =
            page == 0 ? types->is_valid() && choice_exists() : !template_changed;
        progress->setText(
            ui::text(page == 0 ? "report_creation.step_type" : "report_creation.step_objects"));
        back->setVisible(page > 0);
        status->setVisible(page > 0);
        if (page == 0) {
            heading->setText(ui::text("report_creation.choose_type"));
            summary->setText(
                context.empty()
                    ? ui::text("report_creation.general_hint")
                    : ui::text("report_creation.context_hint").arg(object_name(context)));
            next->setText(ui::text(valid_choice && direct(selected_kind())
                                       ? "report_creation.open"
                                       : "report_creation.next_objects"));
            next->setEnabled(valid_choice);
            return;
        }
        heading->setText(QString::fromStdString(draft.name));
        summary->setText(report_kind_choice(draft.query.kind).description);
        next->setText(ui::text("report_creation.open"));
        int remaining = 0;
        for (const auto &binding : bindings) {
            const auto selected = selection(binding);
            const bool ready = valid(binding);
            remaining += !ready;
            const bool missing = !ready && (selected.all || !selected.ids.empty());
            const bool original = !context.empty() && !selected.all &&
                                  selected.ids == std::vector<std::string>{context};
            binding.field->set_action_text(ui::text(selected.ids.empty() && !selected.all
                                                        ? "report_creation.select_action"
                                                        : "report_creation.change_action"));
            binding.badge->setText(ui::text(missing    ? "report_creation.reselect"
                                            : original ? "report_creation.current"
                                            : ready    ? "report_creation.selected"
                                                       : "report_creation.pending"));
            binding.badge->setProperty("warning", missing);
            binding.badge->setProperty("ready", ready);
            binding.badge->style()->unpolish(binding.badge);
            binding.badge->style()->polish(binding.badge);
            binding.badge->setVisible(missing || original);
            if (binding.selected_list) {
                const bool list_visible = !selected.all && selected.ids.size() > 1;
                const bool restore_focus =
                    binding.field->hasFocus() || binding.selected_list->hasFocus();
                binding.field->setVisible(!list_visible);
                binding.selected_list->setVisible(list_visible);
                binding.manage->setVisible(list_visible);
                if (restore_focus)
                    (list_visible ? static_cast<QWidget *>(binding.selected_list) : binding.field)
                        ->setFocus();
                if (list_visible) {
                    binding.badge->setVisible(true);
                    if (ready)
                        binding.badge->setText(
                            ui::text("report_creation.selected_count").arg(selected.ids.size()));
                    const auto current = binding.selected_list->currentItem();
                    const auto current_id =
                        current ? current->data(Qt::UserRole).toString() : QString{};
                    const int scroll = binding.selected_list->verticalScrollBar()->value();
                    const int current_row = binding.selected_list->currentRow();
                    binding.selected_list->clear();
                    std::unordered_map<std::string, const ObjectChoice *> by_id;
                    for (const auto &object : binding.field->objects())
                        by_id.emplace(object.id, &object);
                    for (const auto &id : selected.ids) {
                        const auto found = by_id.find(id);
                        const auto *object = found == by_id.end() ? nullptr : found->second;
                        const auto name =
                            object ? (object->path.isEmpty() ? object->name : object->path)
                                   : QString::fromStdString(id) + ui::text("report_dialog.found");
                        auto icon = WorkbenchIcon::satellite;
                        if (object && object->kind == ObjectKind::ground_station)
                            icon = WorkbenchIcon::ground_station;
                        else if (object && object->kind == ObjectKind::sensor)
                            icon = WorkbenchIcon::focus;
                        auto *item =
                            new QListWidgetItem(workbench_icon(icon), name, binding.selected_list);
                        item->setData(Qt::UserRole, QString::fromStdString(id));
                        item->setData(Qt::UserRole + 1,
                                      ui::text(object && object->kind == ObjectKind::ground_station
                                                   ? "object_selection.label_2"
                                               : object && object->kind == ObjectKind::sensor
                                                   ? "object_selection.label_3"
                                                   : "object_selection.label"));
                        item->setToolTip(
                            object_tooltip(name, item->data(Qt::UserRole + 1).toString()));
                        if (!object || !object->selectable)
                            item->setForeground(theme::color(theme::warning));
                        if (item->data(Qt::UserRole).toString() == current_id)
                            binding.selected_list->setCurrentItem(item);
                    }
                    if (!binding.selected_list->currentItem() && current_row >= 0)
                        binding.selected_list->setCurrentRow(
                            std::min(current_row, binding.selected_list->count() - 1));
                    const int row_height =
                        std::max(36, binding.selected_list->fontMetrics().height() + 16);
                    binding.selected_list->setFixedHeight(
                        static_cast<int>(std::min<std::size_t>(4, selected.ids.size())) *
                            row_height +
                        2);
                    binding.selected_list->verticalScrollBar()->setValue(scroll);
                }
            }
            QString detail;
            if (missing)
                detail = ui::text("report_creation.invalid_binding");
            else if (selected.all)
                detail = ui::text("report_creation.all_hint");
            binding.detail->setText(detail);
            binding.detail->setVisible(!detail.isEmpty());
        }
        next->setEnabled(valid_choice && remaining == 0);
        status->setText(!valid_choice ? ui::text("report_creation.changed")
                        : remaining   ? ui::text("report_creation.remaining").arg(remaining)
                                      : QString{});
        status->setVisible(!status->text().isEmpty());
    }
    void prepare_bindings() {
        if (prepared_key == types->selection() && !template_changed)
            return;
        rebuilding = true;
        while (pages->count() > 1) {
            auto *page = pages->widget(1);
            pages->removeWidget(page);
            delete page;
        }
        bindings.clear();
        prepared_key = types->selection();
        template_changed = false;
        draft = {};
        draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        draft.query.kind = selected_kind();
        draft.name = report_kind_name(draft.query.kind).toStdString();
        draft.query.first = project.first;
        draft.query.last = project.last;
        if (draft.query.kind == ReportKind::aer)
            draft.query.step_nanoseconds = 1'000'000'000;
        for (const auto &definition : project.reports)
            if (prepared_key == "template:" + QString::fromStdString(definition.id))
                draft = definition;
        int initial_context_role = context_role(draft.query.kind);
        if (report_is_intersatellite(draft.query.kind) && draft.query.object_ids.size() == 2 &&
            draft.query.object_ids[1] == context)
            initial_context_role = 1;
        if (!direct(draft.query.kind)) {
            const bool pair = pair_report(draft.query.kind);
            const bool coverage = report_is_coverage(draft.query.kind);
            auto *scroll = new QScrollArea(pages);
            scroll->setObjectName("reportCreationBindings");
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            auto *content = new QWidget(scroll);
            auto *layout = new QVBoxLayout(content);
            layout->setContentsMargins(0, 0, 4, 0);
            layout->setSpacing(20);
            for (int role = 0; role < (pair || coverage ? 2 : 1); ++role) {
                const bool multiple = !pair && (!coverage || role == 1);
                ObjectSelection selection;
                if (coverage) {
                    if (role == 0 && !draft.query.region_id.empty())
                        selection.ids = {draft.query.region_id};
                    else if (role == 1)
                        selection.ids = draft.query.object_ids;
                } else if (pair) {
                    if (draft.query.object_ids.size() == 2)
                        selection.ids = {draft.query.object_ids[role]};
                } else {
                    selection.ids = draft.query.object_ids;
                    selection.all = draft.query.scope == ReportScope::all_compatible;
                }
                if (role == initial_context_role)
                    selection = {{context}, false};
                auto *card = new QFrame(content);
                card->setObjectName("reportBindingCard");
                auto *card_layout = new QVBoxLayout(card);
                card_layout->setContentsMargins(0, 0, 0, 0);
                card_layout->setSpacing(8);
                auto *header = new QHBoxLayout;
                auto *label = new QLabel(pair || coverage ? role_name(draft.query.kind, role)
                                                          : ui::text("report_creation.objects"),
                                         card);
                label->setObjectName("reportBindingRole");
                label->setWordWrap(true);
                header->addWidget(label);
                Binding binding{role};
                binding.badge = new QLabel(card);
                binding.badge->setObjectName("reportBindingBadge");
                binding.badge->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
                header->addWidget(binding.badge);
                header->addStretch();
                card_layout->addLayout(header);
                if (multiple) {
                    auto *hint = new QLabel(requirements(draft.query.kind), card);
                    hint->setObjectName("reportBindingDetail");
                    hint->setWordWrap(true);
                    card_layout->addWidget(hint);
                }
                binding.field = new ObjectSelectionField(card);
                binding.field->setObjectName(QStringLiteral("reportCreationObject%1").arg(role));
                binding.field->setAccessibleName(role_name(draft.query.kind, role));
                binding.field->setMinimumHeight(40);
                binding.field->setProperty("reportObjectField", true);
                binding.field->set_options({pair || coverage ? role_name(draft.query.kind, role)
                                                             : requirements(draft.query.kind),
                                            multiple,
                                            pair || report_is_coverage(draft.query.kind)
                                                ? QString{}
                                                : ui::text("report_dialog.label_156")});
                binding.field->set_selection(selection);
                label->setBuddy(binding.field);
                card_layout->addWidget(binding.field);
                QObject::connect(binding.field, &ObjectSelectionField::selectionChanged, owner,
                                 [this] { update_candidates(); });
                if (multiple) {
                    binding.selected_list = new QListWidget(card);
                    binding.selected_list->setObjectName("reportCreationSelectedObjects");
                    binding.selected_list->setAccessibleName(
                        ui::text("report_creation.selected_objects"));
                    binding.selected_list->setAccessibleDescription(
                        ui::text("report_creation.list_help"));
                    binding.selected_list->setUniformItemSizes(true);
                    binding.selected_list->setMouseTracking(true);
                    binding.selected_list->setIconSize({16, 16});
                    binding.selected_list->setTextElideMode(Qt::ElideMiddle);
                    binding.selected_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                    binding.selected_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
                    const auto remove = [field = binding.field](const QString &id) {
                        auto value = field->selection();
                        std::erase(value.ids, id.toStdString());
                        field->set_selection(std::move(value));
                    };
                    binding.selected_list->setItemDelegate(
                        new SelectedObjectDelegate(remove, binding.selected_list));
                    auto *remove_action = new QAction(ui::text("report_creation.remove_selected"),
                                                      binding.selected_list);
                    remove_action->setObjectName("reportCreationRemoveObject");
                    remove_action->setShortcut(QKeySequence(Qt::Key_Delete));
                    remove_action->setShortcutContext(Qt::WidgetShortcut);
                    binding.selected_list->addAction(remove_action);
                    binding.selected_list->setContextMenuPolicy(Qt::ActionsContextMenu);
                    QObject::connect(remove_action, &QAction::triggered, owner,
                                     [list = binding.selected_list, remove] {
                                         if (auto *item = list->currentItem())
                                             remove(item->data(Qt::UserRole).toString());
                                     });
                    QObject::connect(binding.selected_list, &QListWidget::itemDoubleClicked,
                                     binding.field, &ObjectSelectionField::choose);
                    card_layout->addWidget(binding.selected_list);
                    binding.manage = new QPushButton(ui::text("report_creation.manage"), card);
                    binding.manage->setObjectName("reportCreationManageObjects");
                    binding.manage->setIcon(workbench_icon(WorkbenchIcon::add));
                    binding.manage->setIconSize({14, 14});
                    binding.manage->setAutoDefault(false);
                    QObject::connect(binding.manage, &QPushButton::clicked, binding.field,
                                     &ObjectSelectionField::choose);
                    header->addWidget(binding.manage);
                }
                binding.detail = new QLabel(card);
                binding.detail->setObjectName("reportBindingDetail");
                binding.detail->setTextFormat(Qt::PlainText);
                binding.detail->setWordWrap(true);
                binding.detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                card_layout->addWidget(binding.detail);
                layout->addWidget(card);
                bindings.push_back(binding);
            }
            layout->addStretch();
            scroll->setWidget(content);
            pages->addWidget(scroll);
        }
        rebuilding = false;
        update_candidates();
    }
    void update_candidates() {
        if (rebuilding)
            return;
        rebuilding = true;
        for (auto &binding : bindings)
            binding.field->set_objects(candidates(binding));
        rebuilding = false;
        refresh();
    }
    void advance() {
        refresh();
        if (!next->isEnabled())
            return;
        if (pages->currentIndex() == 0)
            prepare_bindings();
        if (pages->currentIndex() == 0 && pages->count() > 1) {
            update_candidates();
            show_page(1);
            return;
        }
        draft.query.scope = ReportScope::objects;
        draft.query.object_ids.clear();
        if (draft.query.kind == ReportKind::scenario_summary)
            draft.query.scope = ReportScope::all_compatible;
        else if (report_is_coverage(draft.query.kind)) {
            draft.query.region_id = selection(bindings[0]).ids.front();
            draft.query.object_ids = selection(bindings[1]).ids;
        } else if (pair_report(draft.query.kind)) {
            draft.query.object_ids.resize(2);
            for (const auto &binding : bindings)
                draft.query.object_ids[binding.role] = selection(binding).ids.front();
        } else {
            const auto selected = selection(bindings.front());
            draft.query.scope = selected.all ? ReportScope::all_compatible : ReportScope::objects;
            draft.query.object_ids = selected.ids;
        }
        owner->QDialog::accept();
    }
};
ReportCreationDialog::ReportCreationDialog(Project project, std::string context,
                                           std::optional<ReportKind> preset, QWidget *parent)
    : QDialog(parent),
      impl_(std::make_unique<Impl>(this, std::move(project), std::move(context), preset)) {}
ReportCreationDialog::~ReportCreationDialog() = default;
void ReportCreationDialog::accept() { impl_->advance(); }
ReportDefinition ReportCreationDialog::definition() const { return impl_->draft; }
void ReportCreationDialog::update_project(Project project) {
    if (impl_->prepared_key.startsWith("template:")) {
        const auto id = impl_->prepared_key.mid(9).toStdString();
        const auto old = std::ranges::find(impl_->project.reports, id, &ReportDefinition::id);
        const auto latest = std::ranges::find(project.reports, id, &ReportDefinition::id);
        if (old == impl_->project.reports.end() || latest == project.reports.end() ||
            old->name != latest->name || old->query != latest->query ||
            old->format != latest->format)
            impl_->template_changed = true;
    }
    impl_->project = std::move(project);
    impl_->available_choices = impl_->choices();
    impl_->types->set_choices(impl_->available_choices);
    impl_->update_candidates();
}
} // namespace orbelle::desktop
