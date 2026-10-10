#include "insert_object_dialog.hpp"
#include "workbench_style.hpp"
#include <QButtonGroup>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace orbitforge::desktop {
std::optional<InsertObjectRequest> insert_object_dialog(QWidget *parent,
                                                        const QString &scenario_name) {
    QDialog dialog(parent);
    dialog.setObjectName("insertObjectsDialog");
    dialog.setWindowTitle(QStringLiteral("插入对象"));
    dialog.setWindowIcon(workbench_icon(WorkbenchIcon::satellite));
    dialog.setMinimumSize(540, 280);
    dialog.resize(560, 292);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("插入对象"));
    title->setObjectName("panelTitle");
    heading->addWidget(title);
    heading->addStretch();
    auto *destination = new QLabel;
    destination->setObjectName("muted");
    destination->setTextFormat(Qt::PlainText);
    destination->setText(destination->fontMetrics().elidedText(
        QStringLiteral("场景 / ") + scenario_name, Qt::ElideMiddle, 240));
    destination->setToolTip(scenario_name);
    heading->addWidget(destination);
    layout->addLayout(heading);

    auto *columns = new QHBoxLayout;
    columns->setSpacing(20);
    auto *left = new QVBoxLayout;
    left->setSpacing(6);
    auto *type_heading = new QLabel(QStringLiteral("对象类型"));
    type_heading->setObjectName("sectionTitle");
    left->addWidget(type_heading);
    auto *types = new QTreeWidget;
    types->setObjectName("insertObjectTypes");
    types->setAccessibleName(QStringLiteral("对象类型"));
    types->setHeaderHidden(true);
    types->setRootIsDecorated(false);
    types->setItemsExpandable(false);
    types->setIndentation(14);
    types->setIconSize({16, 16});
    types->setMinimumWidth(160);
    auto *group = new QTreeWidgetItem(types, {QStringLiteral("场景对象")});
    group->setFlags(Qt::ItemIsEnabled);
    group->setForeground(0, QColor("#8c9db1"));
    auto *satellite = new QTreeWidgetItem(group, {QStringLiteral("卫星")});
    satellite->setIcon(0, workbench_icon(WorkbenchIcon::satellite, QColor("#80b9e4")));
    satellite->setData(0, Qt::UserRole, static_cast<int>(InsertObjectType::satellite));
    group->setExpanded(true);
    types->setCurrentItem(satellite);
    left->addWidget(types, 1);
    columns->addLayout(left, 2);

    auto *right = new QVBoxLayout;
    right->setSpacing(0);
    auto *method_heading = new QLabel(QStringLiteral("插入方式"));
    method_heading->setObjectName("sectionTitle");
    right->addWidget(method_heading);
    right->addSpacing(10);
    auto *methods = new QButtonGroup(&dialog);
    const auto add_method = [&](const char *name, InsertObjectMethod method, const QString &text,
                                const QString &description) {
        auto *choice = new QRadioButton(text);
        choice->setObjectName(name);
        choice->setAccessibleDescription(description);
        methods->addButton(choice, static_cast<int>(method));
        right->addWidget(choice);
        auto *caption = new QLabel(description);
        caption->setObjectName("hint");
        caption->setContentsMargins(22, 4, 0, 0);
        caption->setWordWrap(true);
        right->addWidget(caption);
        return choice;
    };
    auto *properties =
        add_method("insertDefineProperties", InsertObjectMethod::define_properties,
                   QStringLiteral("定义属性"), QStringLiteral("设置轨道、参考系与传播器"));
    properties->setChecked(true);
    right->addSpacing(18);
    add_method("insertDefaults", InsertObjectMethod::defaults, QStringLiteral("使用默认参数"),
               QStringLiteral("二体传播 · 场景起始时刻 · 60 s 步长"));
    right->addStretch(1);
    columns->addLayout(right, 3);
    layout->addLayout(columns, 1);

    auto *divider = new QFrame;
    divider->setObjectName("sectionDivider");
    divider->setFixedHeight(1);
    layout->addWidget(divider);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    buttons->addStretch();
    auto *close = new QPushButton(QStringLiteral("关闭"));
    close->setObjectName("closeInsertObjects");
    close->setAutoDefault(false);
    auto *insert = new QPushButton(QStringLiteral("配置属性"));
    insert->setObjectName("insertObject");
    insert->setDefault(true);
    insert->setMinimumWidth(84);
    buttons->addWidget(close);
    buttons->addWidget(insert);
    layout->addLayout(buttons);
    QObject::connect(methods, &QButtonGroup::idToggled, &dialog, [insert](int id, bool checked) {
        if (checked)
            insert->setText(id == static_cast<int>(InsertObjectMethod::define_properties)
                                ? QStringLiteral("配置属性")
                                : QStringLiteral("直接插入"));
    });
    QObject::connect(types, &QTreeWidget::currentItemChanged, &dialog,
                     [insert](QTreeWidgetItem *item) {
                         insert->setEnabled(item && item->data(0, Qt::UserRole).isValid());
                     });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    std::optional<InsertObjectRequest> result;
    QObject::connect(insert, &QPushButton::clicked, &dialog, [&] {
        const auto *item = types->currentItem();
        if (!item || !item->data(0, Qt::UserRole).isValid() || methods->checkedId() < 0)
            return;
        result =
            InsertObjectRequest{static_cast<InsertObjectType>(item->data(0, Qt::UserRole).toInt()),
                                static_cast<InsertObjectMethod>(methods->checkedId())};
        dialog.accept();
    });
    types->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        result.reset();
    return result;
}
} // namespace orbitforge::desktop
