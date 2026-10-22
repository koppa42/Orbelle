#include "insert_object_dialog.hpp"
#include "../ui_text.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <array>

namespace orbelle::desktop {
namespace {
struct ObjectSpec {
    InsertObjectType type;
    const char *key;
    const char *label;
    const char *properties;
    const char *defaults;
    WorkbenchIcon icon;
};
constexpr std::array objects{
    ObjectSpec{InsertObjectType::satellite, "satellite", "insert_object_dialog.satellite",
               "insert_object_dialog.label_2", "insert_object_dialog.label_4",
               WorkbenchIcon::satellite},
    ObjectSpec{InsertObjectType::ground_station, "ground-station", "insert_object_dialog.station",
               "insert_object_dialog.label_6", "insert_object_dialog.caption",
               WorkbenchIcon::ground_station},
    ObjectSpec{InsertObjectType::sensor, "sensor", "insert_object_dialog.sensor",
               "insert_object_dialog.sensor_properties", "insert_object_dialog.sensor_defaults",
               WorkbenchIcon::focus}};
struct MethodChoice {
    InsertObjectMethod method;
    const char *key;
    const char *label;
    WorkbenchIcon icon;
    bool satellite_only = false;
};
constexpr std::array methods{
    MethodChoice{InsertObjectMethod::define_properties, "properties",
                 "insert_object_dialog.properties", WorkbenchIcon::properties},
    MethodChoice{InsertObjectMethod::defaults, "defaults", "insert_object_dialog.label_3",
                 WorkbenchIcon::add},
    MethodChoice{InsertObjectMethod::orbit_wizard, "orbit-wizard", "insert_object_dialog.wizard",
                 WorkbenchIcon::orbit, true},
    MethodChoice{InsertObjectMethod::tle_file, "tle-file", "insert_object_dialog.tle",
                 WorkbenchIcon::folder, true}};
QString preference_key(InsertObjectType type) {
    for (const auto &object : objects)
        if (object.type == type)
            return QStringLiteral("insertion/default-method/") + object.key;
    return {};
}
} // namespace

InsertObjectDialog::InsertObjectDialog(const QString &preferences_file, QWidget *parent)
    : QDialog(parent), settings_(preferences_file, QSettings::IniFormat) {
    setObjectName("insertObjectsDialog");
    setWindowTitle(ui::text("insert_object_dialog.label"));
    setWindowIcon(workbench_icon(WorkbenchIcon::satellite));
    setMinimumSize(360, 300);
    const auto available = screen()->availableGeometry().size() - QSize(32, 64);
    resize(QSize(780, 500).boundedTo(available).expandedTo(minimumSize()));
    setSizeGripEnabled(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(18);
    destination_ = new QLabel;
    destination_->setObjectName("insertDestination");
    destination_->setTextFormat(Qt::PlainText);
    destination_->setWordWrap(true);
    destination_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(destination_);

    auto *scroll = new QScrollArea;
    scroll->setObjectName("insertOptionsScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget;
    auto *columns = new QHBoxLayout(body);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(24);
    auto *left = new QVBoxLayout;
    left->setSpacing(10);
    auto *type_heading = new QLabel(ui::text("insert_object_dialog.type_heading"));
    type_heading->setObjectName("insertSectionHeading");
    type_heading->setMinimumHeight(32);
    left->addWidget(type_heading);
    types_ = new QTreeWidget;
    types_->setObjectName("insertObjectTypes");
    types_->setAccessibleName(ui::text("insert_object_dialog.type_heading"));
    types_->setHeaderHidden(true);
    types_->setRootIsDecorated(false);
    types_->setItemsExpandable(false);
    types_->setIndentation(12);
    types_->setMinimumSize(160, 204);
    types_->setIconSize(QSize(18, 18));
    auto *group = new QTreeWidgetItem(types_, {ui::text("insert_object_dialog.group")});
    group->setFlags(Qt::ItemIsEnabled);
    group->setForeground(0, theme::color(theme::text_muted));
    auto *attached = new QTreeWidgetItem(types_, {ui::text("insert_object_dialog.attached")});
    attached->setFlags(Qt::ItemIsEnabled);
    attached->setForeground(0, theme::color(theme::text_muted));
    for (const auto &object : objects) {
        auto *item = new QTreeWidgetItem(object.type == InsertObjectType::sensor ? attached : group,
                                         {ui::text(object.label)});
        item->setData(0, Qt::UserRole, static_cast<int>(object.type));
        item->setIcon(0, workbench_icon(object.icon));
    }
    group->setExpanded(true);
    attached->setExpanded(true);
    left->addWidget(types_, 1);
    columns->addLayout(left, 1);

    auto *right = new QVBoxLayout;
    right->setSpacing(10);
    auto *method_heading = new QHBoxLayout;
    auto *method_label = new QLabel(ui::text("insert_object_dialog.method_heading"));
    method_label->setObjectName("insertSectionHeading");
    method_label->setMinimumHeight(32);
    method_heading->addWidget(method_label);
    method_heading->addStretch();
    save_default_ = new QPushButton(ui::text("insert_object_dialog.save_default"));
    save_default_->setObjectName("saveDefaultInsertMethod");
    save_default_->setToolTip(ui::text("insert_object_dialog.default_hint"));
    save_default_->setAutoDefault(false);
    method_heading->addWidget(save_default_);
    right->addLayout(method_heading);
    methods_ = new QTreeWidget;
    methods_->setObjectName("insertObjectMethods");
    methods_->setAccessibleName(ui::text("insert_object_dialog.method_heading"));
    methods_->setColumnCount(2);
    methods_->setHeaderHidden(true);
    methods_->setRootIsDecorated(false);
    methods_->setMinimumSize(210, 94);
    methods_->setIconSize(QSize(18, 18));
    methods_->header()->setStretchLastSection(false);
    methods_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    methods_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    for (const auto &method : methods) {
        auto *item = new QTreeWidgetItem(methods_, {ui::text(method.label)});
        item->setData(0, Qt::UserRole, static_cast<int>(method.method));
        item->setIcon(0, workbench_icon(method.icon));
    }
    right->addWidget(methods_);
    description_ = new QLabel;
    description_->setObjectName("insertMethodDescription");
    description_->setWordWrap(true);
    description_->setTextFormat(Qt::PlainText);
    description_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    description_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    right->addWidget(description_);
    parent_label_ = new QLabel(ui::text("insert_object_dialog.parent"));
    right->addWidget(parent_label_);
    parent_ = new ObjectSelectionField;
    parent_->setObjectName("insertSensorParent");
    parent_->set_options({ui::text("insert_object_dialog.parent"), false, {}});
    right->addWidget(parent_);
    right->addStretch();
    columns->addLayout(right, 2);
    responsive_columns(body, columns, 560);
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);

    result_ = new QLabel;
    result_->setObjectName("insertObjectResult");
    result_->setTextFormat(Qt::PlainText);
    result_->setWordWrap(true);
    result_->hide();
    layout->addWidget(result_);
    auto *divider = new QFrame;
    divider->setObjectName("sectionDivider");
    divider->setFixedHeight(1);
    layout->addWidget(divider);
    auto *footer = new QWidget;
    auto *footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(0, 0, 0, 0);
    footer_layout->setSpacing(12);
    keep_open_ = new QCheckBox(ui::text("insert_object_dialog.keep_open"));
    keep_open_->setObjectName("keepInsertObjectsOpen");
    keep_open_->setToolTip(ui::text("insert_object_dialog.keep_open_hint"));
    keep_open_->setChecked(settings_.value("insertion/keep-open", false).toBool());
    footer_layout->addWidget(keep_open_);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *close = new QPushButton(ui::text("common.close"));
    close->setObjectName("closeInsertObjects");
    close->setAutoDefault(false);
    close->setMinimumSize(88, 32);
    insert_ = new QPushButton;
    insert_->setObjectName("insertObject");
    insert_->setDefault(true);
    insert_->setMinimumSize(108, 32);
    buttons->setSpacing(12);
    buttons->addWidget(close);
    buttons->addWidget(insert_);
    footer_layout->addLayout(buttons, 1);
    responsive_columns(footer, footer_layout, 500);
    layout->addWidget(footer);

    connect(types_, &QTreeWidget::currentItemChanged, this, [this] { refresh_methods(); });
    connect(methods_, &QTreeWidget::currentItemChanged, this, [this] { refresh_details(); });
    connect(parent_, &ObjectSelectionField::selectionChanged, this, [this] { refresh_details(); });
    connect(methods_, &QTreeWidget::itemActivated, this, [this] { submit(); });
    connect(types_, &QTreeWidget::itemActivated, this, [this] { submit(); });
    connect(insert_, &QPushButton::clicked, this, [this] { submit(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(save_default_, &QPushButton::clicked, this, [this] {
        const auto request = selection();
        if (!request || busy_)
            return;
        const auto key = preference_key(request->type);
        const auto previous = settings_.value(key);
        for (const auto &method : methods)
            if (method.method == request->method)
                settings_.setValue(key, method.key);
        if (save_preferences()) {
            refresh_details();
            show_result(ui::text("insert_object_dialog.default_saved"));
        } else {
            settings_.setValue(key, previous);
        }
    });
    connect(keep_open_, &QCheckBox::toggled, this, [this](bool checked) {
        settings_.setValue("insertion/keep-open", checked);
        save_preferences();
    });
    select_type(InsertObjectType::satellite);
}

void InsertObjectDialog::set_scenario(const QString &name) {
    destination_->setText(ui::text("insert_object_dialog.destination.set_text") + name);
    destination_->setToolTip(name);
}
InsertObjectMethod InsertObjectDialog::default_method(InsertObjectType type) const {
    const auto value = settings_.value(preference_key(type)).toString();
    for (const auto &method : methods)
        if (value == method.key && (!method.satellite_only || type == InsertObjectType::satellite))
            return method.method;
    return InsertObjectMethod::define_properties;
}
void InsertObjectDialog::select_type(InsertObjectType type) {
    for (int group_index = 0; group_index < types_->topLevelItemCount(); ++group_index) {
        auto *group = types_->topLevelItem(group_index);
        for (int i = 0; i < group->childCount(); ++i)
            if (group->child(i)->data(0, Qt::UserRole).toInt() == static_cast<int>(type)) {
                types_->setCurrentItem(group->child(i));
                refresh_methods();
                return;
            }
    }
}
void InsertObjectDialog::set_parents(std::vector<ObjectChoice> parents,
                                     std::optional<std::string> selected) {
    parent_->set_objects(std::move(parents));
    if (selected)
        parent_->set_id(*selected);
    refresh_details();
}
std::optional<InsertObjectRequest> InsertObjectDialog::selection() const {
    const auto *object = types_->currentItem();
    const auto *method = methods_->currentItem();
    if (!object || !object->data(0, Qt::UserRole).isValid() || !method || method->isHidden())
        return std::nullopt;
    return InsertObjectRequest{
        static_cast<InsertObjectType>(object->data(0, Qt::UserRole).toInt()),
        static_cast<InsertObjectMethod>(method->data(0, Qt::UserRole).toInt()), parent_->id()};
}
void InsertObjectDialog::refresh_methods() {
    show_result({});
    const auto *object = types_->currentItem();
    if (!object || !object->data(0, Qt::UserRole).isValid()) {
        methods_->setCurrentItem(nullptr);
        refresh_details();
        return;
    }
    const auto preferred =
        default_method(static_cast<InsertObjectType>(object->data(0, Qt::UserRole).toInt()));
    int visible_methods = 0;
    for (int i = 0; i < methods_->topLevelItemCount(); ++i) {
        methods_->topLevelItem(i)->setHidden(methods[static_cast<std::size_t>(i)].satellite_only &&
                                             object->data(0, Qt::UserRole).toInt() !=
                                                 static_cast<int>(InsertObjectType::satellite));
        if (methods_->topLevelItem(i)->data(0, Qt::UserRole).toInt() == static_cast<int>(preferred))
            methods_->setCurrentItem(methods_->topLevelItem(i));
        visible_methods += !methods_->topLevelItem(i)->isHidden();
    }
    methods_->setFixedHeight(visible_methods * 40 + 14);
    refresh_details();
}
void InsertObjectDialog::refresh_details() {
    const auto request = selection();
    const bool sensor = request && request->type == InsertObjectType::sensor;
    parent_label_->setVisible(sensor);
    parent_->setVisible(sensor);
    parent_->setEnabled(!busy_);
    insert_->setEnabled(!busy_ && request && (!sensor || parent_->contains(parent_->id())));
    save_default_->setEnabled(!busy_ && request &&
                              default_method(request->type) != request->method);
    methods_->setEnabled(!busy_ && request.has_value());
    keep_open_->setEnabled(!busy_ && request && request->method == InsertObjectMethod::defaults);
    insert_->setText(ui::text(request && request->method == InsertObjectMethod::defaults
                                  ? "insert_object_dialog.label_5"
                              : request && request->method == InsertObjectMethod::tle_file
                                  ? "insert_object_dialog.choose_file"
                              : request && request->method == InsertObjectMethod::orbit_wizard
                                  ? "insert_object_dialog.open_wizard"
                                  : "insert_object_dialog.insert"));
    description_->clear();
    for (int i = 0; i < methods_->topLevelItemCount(); ++i) {
        auto *item = methods_->topLevelItem(i);
        const bool preferred = request && item->data(0, Qt::UserRole).toInt() ==
                                              static_cast<int>(default_method(request->type));
        item->setText(1, preferred ? ui::text("insert_object_dialog.default_badge") : QString{});
    }
    if (request)
        for (const auto &object : objects)
            if (object.type == request->type)
                description_->setText(ui::text(request->method == InsertObjectMethod::defaults
                                                   ? object.defaults
                                                   : object.properties));
    if (request && request->method == InsertObjectMethod::tle_file)
        description_->setText(ui::text("insert_object_dialog.tle_description"));
    if (request && request->method == InsertObjectMethod::orbit_wizard)
        description_->setText(ui::text("insert_object_dialog.wizard_description"));
}
void InsertObjectDialog::set_busy(bool busy) {
    busy_ = busy;
    types_->setEnabled(!busy);
    refresh_details();
}
void InsertObjectDialog::show_result(const QString &message) {
    result_->setText(message);
    result_->setVisible(!message.isEmpty());
}
bool InsertObjectDialog::save_preferences() {
    settings_.sync();
    if (settings_.status() == QSettings::NoError)
        return true;
    show_result(ui::text("insert_object_dialog.preferences_failed"));
    return false;
}
void InsertObjectDialog::submit() {
    const auto request = selection();
    if (!isVisible() || !request || busy_ || !insert || !insert_->isEnabled())
        return;
    show_result({});
    if (request->method != InsertObjectMethod::defaults || !keep_open_->isChecked())
        hide();
    insert(*request);
}
} // namespace orbelle::desktop
