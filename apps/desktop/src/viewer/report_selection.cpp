#include "report_selection.hpp"
#include "workbench_style.hpp"
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace orbitforge::desktop {
ReportSelectionDialog::ReportSelectionDialog(std::vector<ReportChoice> choices, QString selected,
                                             QWidget *parent)
    : QDialog(parent) {
    setObjectName("reportSelectionDialog");
    setWindowTitle(QStringLiteral("选择报告"));
    setWindowIcon(workbench_icon(WorkbenchIcon::properties));
    resize(780, 540);
    setMinimumSize(360, 440);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(14);
    auto *heading = new QLabel(QStringLiteral("选择报告"), this);
    heading->setObjectName("panelTitle");
    layout->addWidget(heading);
    search_ = new QLineEdit(this);
    search_->setObjectName("reportSelectionSearch");
    search_->setPlaceholderText(QStringLiteral("搜索报告名称、内容或对象…"));
    search_->setAccessibleName(QStringLiteral("搜索报告"));
    search_->setClearButtonEnabled(true);
    search_->addAction(workbench_icon(WorkbenchIcon::search), QLineEdit::LeadingPosition);
    layout->addWidget(search_);
    source_ = new QTabBar(this);
    source_->setObjectName("reportSelectionSource");
    source_->setAccessibleName(QStringLiteral("报告来源"));
    source_->addTab(QStringLiteral("全部"));
    source_->addTab(QStringLiteral("内置报告"));
    source_->addTab(QStringLiteral("项目模板"));
    source_->setExpanding(false);
    source_->setDrawBase(false);
    layout->addWidget(source_);
    split_ = new QSplitter(this);
    split_->setObjectName("reportSelectionSplit");
    split_->setChildrenCollapsible(false);
    list_ = new QListWidget(split_);
    list_->setObjectName("reportSelectionList");
    list_->setAccessibleName(QStringLiteral("报告列表"));
    list_->setTextElideMode(Qt::ElideRight);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMinimumSize(0, 100);
    auto *empty_layout = new QVBoxLayout(list_->viewport());
    empty_layout->addStretch();
    empty_ = new QLabel(QStringLiteral("没有匹配的报告\n试试其他关键词或来源"), list_->viewport());
    empty_->setObjectName("reportSelectionEmpty");
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setAttribute(Qt::WA_TransparentForMouseEvents);
    empty_layout->addWidget(empty_);
    empty_layout->addStretch();
    auto *scroll = new QScrollArea(split_);
    scroll->setObjectName("reportSelectionPreview");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumSize(0, 100);
    auto *details = new QWidget(scroll);
    scroll->setWidget(details);
    auto *preview_layout = new QVBoxLayout(details);
    preview_layout->setContentsMargins(18, 14, 18, 14);
    preview_layout->setSpacing(14);
    const auto label = [&](const char *name) {
        auto *value = new QLabel(details);
        value->setObjectName(name);
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        value->setMinimumWidth(0);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        preview_layout->addWidget(value);
        return value;
    };
    category_ = label("reportSelectionCategory");
    title_ = label("reportSelectionTitle");
    description_ = label("reportSelectionDescription");
    scope_ = label("reportSelectionScope");
    warning_ = label("reportSelectionWarning");
    preview_layout->addStretch();
    split_->setSizes({340, 360});
    layout->addWidget(split_, 1);
    count_ = new QLabel(this);
    count_->setObjectName("reportSelectionCount");
    layout->addWidget(count_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    confirm_ = buttons->button(QDialogButtonBox::Ok);
    confirm_->setText(QStringLiteral("使用此报告"));
    confirm_->setProperty("primary", true);
    confirm_->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ReportSelectionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(search_, &QLineEdit::textChanged, this, [this] { filter(); });
    connect(source_, &QTabBar::currentChanged, this, [this] { filter(); });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { preview(); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { accept(); });
    set_choices(std::move(choices));
    for (int row = 0; row < list_->count(); ++row)
        if (list_->item(row)->data(Qt::UserRole).toString() == selected)
            list_->setCurrentRow(row);
    search_->setFocus();
}

void ReportSelectionDialog::set_choices(std::vector<ReportChoice> choices) {
    const auto selected = selection();
    choices_ = std::move(choices);
    {
        const QSignalBlocker guard(list_);
        list_->clear();
        for (const auto &choice : choices_) {
            auto *item = new QListWidgetItem(
                workbench_icon(choice.saved ? WorkbenchIcon::save : WorkbenchIcon::properties),
                choice.name, list_);
            item->setData(Qt::UserRole, choice.key);
            item->setToolTip(choice.name + "\n" + choice.kind + "\n" + choice.scope);
            if (!choice.unavailable_reason.isEmpty())
                item->setForeground(QColor("#8b99a9"));
            if (choice.key == selected)
                list_->setCurrentItem(item);
        }
    }
    filter();
}
QString ReportSelectionDialog::selection() const {
    const auto *item = list_->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString{};
}
void ReportSelectionDialog::filter() {
    const auto query = search_->text().trimmed();
    int visible = 0;
    for (int row = 0; row < list_->count(); ++row) {
        const auto &choice = choices_.at(static_cast<std::size_t>(row));
        const auto searchable =
            choice.name + " " + choice.kind + " " + choice.description + " " + choice.scope;
        const bool matches =
            searchable.contains(query, Qt::CaseInsensitive) &&
            (source_->currentIndex() == 0 || choice.saved == (source_->currentIndex() == 2));
        list_->item(row)->setHidden(!matches);
        visible += matches ? 1 : 0;
    }
    // Filtering must never accept an invisible old selection on Enter.
    if (!list_->currentItem() || list_->currentItem()->isHidden())
        list_->setCurrentRow(-1);
    count_->setText(QStringLiteral("%1 项报告").arg(visible));
    empty_->setVisible(visible == 0);
    preview();
}
void ReportSelectionDialog::preview() {
    const auto found = std::ranges::find(choices_, selection(), &ReportChoice::key);
    const bool valid =
        found != choices_.end() && list_->currentItem() && !list_->currentItem()->isHidden();
    title_->setText(valid ? found->name : QStringLiteral("选择一项报告"));
    category_->setText(
        valid ? (found->saved ? QStringLiteral("项目模板 · ") : QStringLiteral("内置报告 · ")) +
                    found->kind
              : QString{});
    description_->setText(valid ? found->description
                                : QStringLiteral("在左侧查看报告内容与适用范围"));
    scope_->setText(valid ? QStringLiteral("对象范围\n%1").arg(found->scope) : QString{});
    warning_->setText(valid ? found->unavailable_reason : QString{});
    warning_->setVisible(valid && !found->unavailable_reason.isEmpty());
    confirm_->setEnabled(valid && found->unavailable_reason.isEmpty());
}
void ReportSelectionDialog::accept() {
    if (confirm_->isEnabled())
        QDialog::accept();
}
void ReportSelectionDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (!split_)
        return;
    const auto orientation = event->size().width() < 640 ? Qt::Vertical : Qt::Horizontal;
    if (split_->orientation() != orientation)
        split_->setOrientation(orientation);
    QTimer::singleShot(0, this, [this] {
        if (list_->currentItem())
            list_->scrollToItem(list_->currentItem());
    });
}
} // namespace orbitforge::desktop
