#include "report_selection.hpp"
#include "workbench_theme.hpp"
#include <QDialogButtonBox>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTextLayout>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtMath>
#include <algorithm>

namespace orbitforge::desktop {
namespace {
constexpr int subtitle_role = Qt::UserRole + 1;
constexpr int unavailable_role = Qt::UserRole + 2;
class PreviewTitle final : public QLabel {
  public:
    using QLabel::QLabel;
    QSize sizeHint() const override { return {220, fontMetrics().height()}; }
    QSize minimumSizeHint() const override { return {0, fontMetrics().height()}; }
    int heightForWidth(int width) const override {
        QTextLayout text_layout(text().simplified(), font());
        arrange(text_layout, width);
        return qCeil(text_layout.boundingRect().height());
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        const auto display = text().simplified();
        QTextLayout text_layout(display, font());
        arrange(text_layout, contentsRect().width());
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        for (int row = 0; row < text_layout.lineCount(); ++row) {
            const auto line = text_layout.lineAt(row);
            if (row == 1 && line.textStart() + line.textLength() < display.size())
                painter.drawText(
                    QPointF(contentsRect().left(), contentsRect().top() + line.y() + line.ascent()),
                    fontMetrics().elidedText(display.mid(line.textStart()), Qt::ElideRight,
                                             contentsRect().width()));
            else
                line.draw(&painter, contentsRect().topLeft());
        }
    }

  private:
    static void arrange(QTextLayout &text, int width) {
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        text.setTextOption(option);
        text.beginLayout();
        qreal y = 0;
        for (int row = 0; row < 2; ++row) {
            auto line = text.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(std::max(1, width));
            line.setPosition({0, y});
            y += line.height();
        }
        text.endLayout();
    }
};
class ReportChoiceDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return {120, option.fontMetrics.height() * 2 + 16};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect row = option.rect.adjusted(0, 0, 0, -1);
        const bool selected = option.state & QStyle::State_Selected;
        const bool unavailable = index.data(unavailable_role).toBool();
        paint_list_row(painter, option, QRectF(row), theme::color(theme::accent));
        const auto icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        icon.paint(painter, QRect(row.left() + 12, row.center().y() - 10, 20, 20), Qt::AlignCenter,
                   unavailable ? QIcon::Disabled : QIcon::Normal);
        QFont name_font = option.font;
        painter->setFont(name_font);
        painter->setPen(theme::color(unavailable ? theme::text_faint
                                     : selected  ? theme::accent_bright
                                                 : theme::text));
        const int x = row.left() + 45, available = std::max(0, row.width() - 64);
        const QFontMetrics name_metrics(name_font);
        painter->drawText(
            QRect(x, row.top() + 5, available, name_metrics.height()), Qt::AlignVCenter,
            name_metrics.elidedText(index.data().toString(), Qt::ElideRight, available));
        QFont subtitle_font = option.font;
        subtitle_font.setPixelSize(std::max(11, option.font.pixelSize() - 1));
        painter->setFont(subtitle_font);
        painter->setPen(theme::color(theme::text_muted));
        const QFontMetrics subtitle_metrics(subtitle_font);
        painter->drawText(
            QRect(x, row.top() + 7 + name_metrics.height(), available, subtitle_metrics.height()),
            Qt::AlignVCenter,
            subtitle_metrics.elidedText(index.data(subtitle_role).toString(), Qt::ElideRight,
                                        available));
        painter->restore();
    }
};
} // namespace
ReportCurveSelectionDialog::ReportCurveSelectionDialog(const std::vector<ReportCurveField> &fields,
                                                       const QStringList &selected, QWidget *parent)
    : QDialog(parent) {
    setObjectName("reportCurveSelectionDialog");
    setWindowTitle(QStringLiteral("选择曲线"));
    setWindowIcon(workbench_icon(WorkbenchIcon::properties));
    resize(560, 480);
    setMinimumSize(360, 380);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    auto *hint = new QLabel(
        QStringLiteral("勾选要比较的字段。同单位叠加，不同单位分区，共用时间轴。"), this);
    hint->setObjectName("hint");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    search_ = new QLineEdit(this);
    search_->setObjectName("reportCurveSearch");
    search_->setPlaceholderText(QStringLiteral("搜索字段或单位…"));
    search_->setAccessibleName(QStringLiteral("搜索曲线字段"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);
    list_ = new QTreeWidget(this);
    list_->setObjectName("reportCurveList");
    list_->setAccessibleName(QStringLiteral("曲线字段，多选"));
    list_->setColumnCount(2);
    list_->setHeaderLabels({QStringLiteral("曲线字段"), QStringLiteral("单位")});
    list_->setRootIsDecorated(false);
    list_->setAlternatingRowColors(true);
    list_->setUniformRowHeights(true);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    for (const auto &field : fields) {
        auto label = field.label;
        const auto suffix = " (" + field.unit + ")";
        if (!field.unit.isEmpty() && label.endsWith(suffix))
            label.chop(suffix.size());
        auto *item = new QTreeWidgetItem(list_, {label, field.unit});
        item->setData(0, Qt::UserRole, field.id);
        item->setToolTip(0, field.label + "\n" + field.id);
        item->setCheckState(0, selected.contains(field.id) ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list_, 1);
    auto *footer = new QHBoxLayout;
    count_ = new QLabel(this);
    count_->setObjectName("reportCurveCount");
    count_->setWordWrap(true);
    footer->addWidget(count_, 1);
    auto *clear = new QPushButton(QStringLiteral("清空选择"), this);
    clear->setObjectName("reportCurveClear");
    footer->addWidget(clear);
    layout->addLayout(footer);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    confirm_ = buttons->button(QDialogButtonBox::Ok);
    confirm_->setText(QStringLiteral("应用"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ReportCurveSelectionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(search_, &QLineEdit::textChanged, this, &ReportCurveSelectionDialog::filter);
    connect(list_, &QTreeWidget::itemChanged, this, &ReportCurveSelectionDialog::update_selection);
    connect(clear, &QPushButton::clicked, this, [this] {
        const QSignalBlocker guard(list_);
        for (int i = 0; i < list_->topLevelItemCount(); ++i)
            list_->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
        update_selection();
    });
    update_selection();
}
QStringList ReportCurveSelectionDialog::selection() const {
    QStringList selected;
    for (int i = 0; i < list_->topLevelItemCount(); ++i)
        if (const auto *item = list_->topLevelItem(i); item->checkState(0) == Qt::Checked)
            selected << item->data(0, Qt::UserRole).toString();
    return selected;
}
void ReportCurveSelectionDialog::update_selection() {
    const auto count = selection().size();
    QStringList units;
    for (int i = 0; i < list_->topLevelItemCount(); ++i)
        if (const auto *item = list_->topLevelItem(i);
            item->checkState(0) == Qt::Checked && !units.contains(item->text(1)))
            units << item->text(1);
    count_->setText(
        count > 6
            ? QStringLiteral("已选 %1 项，请保留最多 6 项以便比较").arg(count)
            : QStringLiteral("已选 %1 项 · %2 个绘图区（最多 6 项）").arg(count).arg(units.size()));
    confirm_->setEnabled(count > 0 && count <= 6);
}
void ReportCurveSelectionDialog::filter() {
    const auto query = search_->text().trimmed();
    int visible = 0;
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        auto *item = list_->topLevelItem(i);
        const auto value =
            item->text(0) + " " + item->text(1) + " " + item->data(0, Qt::UserRole).toString();
        item->setHidden(!value.contains(query, Qt::CaseInsensitive));
        visible += !item->isHidden();
    }
    update_selection();
    if (!visible)
        count_->setText(QStringLiteral("没有匹配字段；已选 %1 项保留").arg(selection().size()));
}
void ReportCurveSelectionDialog::accept() {
    if (confirm_->isEnabled())
        QDialog::accept();
}
ReportColumnSelectionDialog::ReportColumnSelectionDialog(std::vector<ReportColumnChoice> fields,
                                                         QStringList selected, QStringList defaults,
                                                         QWidget *parent)
    : QDialog(parent), fields_(std::move(fields)), defaults_(std::move(defaults)) {
    setObjectName("reportColumnSelectionDialog");
    setWindowTitle(QStringLiteral("显示列"));
    setWindowIcon(workbench_icon(WorkbenchIcon::properties));
    resize(560, 500);
    setMinimumSize(340, 380);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    auto *hint = new QLabel(
        QStringLiteral("选择表格、复制和 CSV 使用的列。上移或下移调整顺序；曲线保留完整结果。"),
        this);
    hint->setObjectName("hint");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    search_ = new QLineEdit(this);
    search_->setObjectName("reportColumnSearch");
    search_->setAccessibleName(QStringLiteral("搜索报告列"));
    search_->setPlaceholderText(QStringLiteral("搜索名称、字段或单位…"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);
    list_ = new QTreeWidget(this);
    list_->setObjectName("reportColumnList");
    list_->setAccessibleName(QStringLiteral("报告列，勾选并调整顺序"));
    list_->setColumnCount(2);
    list_->setHeaderLabels({QStringLiteral("列"), QStringLiteral("单位")});
    list_->setRootIsDecorated(false);
    list_->setAlternatingRowColors(true);
    list_->setUniformRowHeights(true);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(list_, 1);
    auto *ordering = new QHBoxLayout;
    up_ = new QPushButton(QStringLiteral("上移"), this);
    down_ = new QPushButton(QStringLiteral("下移"), this);
    up_->setObjectName("reportColumnUp");
    down_->setObjectName("reportColumnDown");
    up_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    down_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Down));
    for (auto *button : {up_, down_}) {
        button->setAutoDefault(false);
        button->setToolTip(QStringLiteral("清空搜索后调整列顺序（Alt+↑ / Alt+↓）"));
        ordering->addWidget(button);
    }
    ordering->addStretch();
    auto *all = new QPushButton(QStringLiteral("全选"), this);
    all->setObjectName("reportColumnAll");
    all->setAutoDefault(false);
    auto *clear = new QPushButton(QStringLiteral("清空"), this);
    clear->setObjectName("reportColumnClear");
    clear->setAutoDefault(false);
    auto *reset = new QPushButton(QStringLiteral("默认列"), this);
    reset->setObjectName("reportColumnDefaults");
    reset->setAutoDefault(false);
    ordering->addWidget(all);
    ordering->addWidget(clear);
    ordering->addWidget(reset);
    layout->addLayout(ordering);
    count_ = new QLabel(this);
    count_->setObjectName("reportColumnCount");
    count_->setWordWrap(true);
    layout->addWidget(count_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    confirm_ = buttons->button(QDialogButtonBox::Ok);
    confirm_->setObjectName("reportColumnConfirm");
    confirm_->setText(QStringLiteral("应用"));
    confirm_->setProperty("primary", true);
    confirm_->setDefault(true);
    confirm_->style()->unpolish(confirm_);
    confirm_->style()->polish(confirm_);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ReportColumnSelectionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(search_, &QLineEdit::textChanged, this, &ReportColumnSelectionDialog::filter);
    connect(list_, &QTreeWidget::itemChanged, this, &ReportColumnSelectionDialog::update_controls);
    connect(list_, &QTreeWidget::currentItemChanged, this,
            &ReportColumnSelectionDialog::update_controls);
    connect(up_, &QPushButton::clicked, this, [this] { move_current(-1); });
    connect(down_, &QPushButton::clicked, this, [this] { move_current(1); });
    connect(all, &QPushButton::clicked, this, [this] {
        const QSignalBlocker guard(list_);
        for (int i = 0; i < list_->topLevelItemCount(); ++i)
            list_->topLevelItem(i)->setCheckState(0, Qt::Checked);
        update_controls();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        const QSignalBlocker guard(list_);
        for (int i = 0; i < list_->topLevelItemCount(); ++i)
            list_->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
        update_controls();
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        search_->clear();
        populate(defaults_);
    });
    populate(selected);
    search_->setFocus();
}
void ReportColumnSelectionDialog::populate(const QStringList &selected) {
    const QSignalBlocker guard(list_);
    list_->clear();
    auto ordered = selected;
    for (const auto &field : fields_)
        if (!ordered.contains(field.id))
            ordered << field.id;
    for (const auto &id : ordered) {
        const auto found = std::ranges::find(fields_, id, &ReportColumnChoice::id);
        if (found == fields_.end())
            continue;
        auto *item = new QTreeWidgetItem(list_, {found->title, found->unit});
        item->setData(0, Qt::UserRole, id);
        item->setToolTip(0, found->title + "\n" + id);
        item->setCheckState(0, selected.contains(id) ? Qt::Checked : Qt::Unchecked);
    }
    list_->setCurrentItem(list_->topLevelItem(0));
    filter();
}
QStringList ReportColumnSelectionDialog::selection() const {
    QStringList selected;
    for (int i = 0; i < list_->topLevelItemCount(); ++i)
        if (const auto *item = list_->topLevelItem(i); item->checkState(0) == Qt::Checked)
            selected << item->data(0, Qt::UserRole).toString();
    return selected;
}
void ReportColumnSelectionDialog::filter() {
    const auto query = search_->text().trimmed();
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        auto *item = list_->topLevelItem(i);
        const auto text =
            item->text(0) + " " + item->text(1) + " " + item->data(0, Qt::UserRole).toString();
        item->setHidden(!text.contains(query, Qt::CaseInsensitive));
    }
    update_controls();
}
void ReportColumnSelectionDialog::update_controls() {
    const auto count = selection().size();
    int visible = 0;
    for (int i = 0; i < list_->topLevelItemCount(); ++i)
        visible += !list_->topLevelItem(i)->isHidden();
    count_->setText(QStringLiteral("已选 %1 列 · 匹配 %2 列%3")
                        .arg(count)
                        .arg(visible)
                        .arg(count ? QString{} : QStringLiteral(" · 请至少选择一列")));
    confirm_->setEnabled(count > 0);
    const auto row = list_->indexOfTopLevelItem(list_->currentItem());
    const bool reorder = search_->text().trimmed().isEmpty();
    up_->setEnabled(reorder && row > 0);
    down_->setEnabled(reorder && row >= 0 && row + 1 < list_->topLevelItemCount());
}
void ReportColumnSelectionDialog::move_current(int offset) {
    const auto row = list_->indexOfTopLevelItem(list_->currentItem());
    if (!search_->text().trimmed().isEmpty() || row < 0 || row + offset < 0 ||
        row + offset >= list_->topLevelItemCount())
        return;
    auto *item = list_->takeTopLevelItem(row);
    list_->insertTopLevelItem(row + offset, item);
    list_->setCurrentItem(item);
    update_controls();
}
void ReportColumnSelectionDialog::accept() {
    if (confirm_->isEnabled())
        QDialog::accept();
}
ReportSelectionDialog::ReportSelectionDialog(std::vector<ReportChoice> choices, QString selected,
                                             QWidget *parent)
    : QDialog(parent) {
    setObjectName("reportSelectionDialog");
    setWindowTitle(QStringLiteral("选择报告"));
    setWindowIcon(workbench_icon(WorkbenchIcon::properties));
    resize(840, 580);
    setMinimumSize(360, 440);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    split_ = new QSplitter(this);
    split_->setObjectName("reportSelectionSplit");
    split_->setHandleWidth(8);
    split_->setChildrenCollapsible(false);
    auto *navigation = new QWidget(split_);
    navigation->setMinimumWidth(0);
    auto *navigation_layout = new QVBoxLayout(navigation);
    navigation_layout->setContentsMargins(0, 0, 0, 0);
    navigation_layout->setSpacing(6);
    search_ = new QLineEdit(this);
    search_->setObjectName("reportSelectionSearch");
    search_->setPlaceholderText(QStringLiteral("搜索名称、内容或对象"));
    search_->setAccessibleName(QStringLiteral("搜索报告"));
    search_->setClearButtonEnabled(true);
    search_->addAction(workbench_icon(WorkbenchIcon::search), QLineEdit::LeadingPosition);
    navigation_layout->addWidget(search_);
    source_ = new QTabBar(this);
    source_->setObjectName("reportSelectionSource");
    source_->setAccessibleName(QStringLiteral("报告来源"));
    source_->addTab(QStringLiteral("全部"));
    source_->addTab(QStringLiteral("内置报告"));
    source_->addTab(QStringLiteral("项目模板"));
    source_->setExpanding(false);
    source_->setDrawBase(false);
    navigation_layout->addWidget(source_);
    list_ = new QListWidget(navigation);
    list_->setObjectName("reportSelectionList");
    list_->setAccessibleName(QStringLiteral("报告列表"));
    list_->setTextElideMode(Qt::ElideRight);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMinimumSize(0, 64);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setMouseTracking(true);
    list_->setItemDelegate(new ReportChoiceDelegate(list_));
    navigation_layout->addWidget(list_, 1);
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
    preview_scroll_ = scroll;
    scroll->setObjectName("reportSelectionPreview");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setMinimumSize(0, 100);
    auto *details = new QWidget(scroll);
    details->setObjectName("reportSelectionDetails");
    scroll->setWidget(details);
    auto *preview_layout = new QVBoxLayout(details);
    preview_layout->setContentsMargins(12, 12, 12, 12);
    preview_layout->setSpacing(8);
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
    auto *identity = new QHBoxLayout;
    symbol_ = new QLabel(details);
    symbol_->setObjectName("reportSelectionSymbol");
    symbol_->setFixedSize(28, 28);
    symbol_->setAlignment(Qt::AlignCenter);
    identity->addWidget(symbol_);
    identity->setSpacing(12);
    auto *identity_text = new QVBoxLayout;
    identity_text->setSpacing(4);
    category_ = new QLabel(details);
    category_->setObjectName("reportSelectionCategory");
    identity_text->addWidget(category_);
    title_ = new PreviewTitle(details);
    title_->setObjectName("reportSelectionTitle");
    title_->setTextFormat(Qt::PlainText);
    title_->setWordWrap(true);
    title_->setMinimumWidth(0);
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identity_text->addWidget(title_);
    identity->addLayout(identity_text, 1);
    preview_layout->addLayout(identity);
    preview_layout->addSpacing(4);
    description_ = label("reportSelectionDescription");
    auto *divider = new QFrame(details);
    divider->setObjectName("reportSelectionDivider");
    divider->setFixedHeight(1);
    preview_layout->addSpacing(4);
    preview_layout->addWidget(divider);
    preview_layout->addSpacing(4);
    output_heading_ = label("reportSelectionFieldHeading");
    output_heading_->setText(QStringLiteral("输出形式"));
    output_ = label("reportSelectionOutput");
    preview_layout->addSpacing(4);
    scope_heading_ = label("reportSelectionFieldHeading");
    scope_heading_->setText(QStringLiteral("对象范围"));
    scope_ = label("reportSelectionScope");
    note_ = label("reportSelectionNote");
    warning_ = label("reportSelectionWarning");
    preview_layout->addStretch();
    split_->setSizes({340, 432});
    split_->setStretchFactor(0, 1);
    split_->setStretchFactor(1, 1);
    layout->addWidget(split_, 1);
    auto *footer = new QHBoxLayout;
    count_ = new QLabel(this);
    count_->setObjectName("reportSelectionCount");
    footer->addWidget(count_);
    footer->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    confirm_ = buttons->button(QDialogButtonBox::Ok);
    confirm_->setObjectName("reportSelectionConfirm");
    confirm_->setText(QStringLiteral("使用此报告"));
    confirm_->setProperty("primary", true);
    confirm_->setDefault(true);
    // QDialogButtonBox polishes standard buttons before their role/name is assigned here.
    confirm_->style()->unpolish(confirm_);
    confirm_->style()->polish(confirm_);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    footer->addWidget(buttons);
    layout->addLayout(footer);
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
                workbench_icon(choice.icon, theme::color(theme::accent)), choice.name, list_);
            item->setData(Qt::UserRole, choice.key);
            const auto subtitle = choice.saved ? choice.kind + QStringLiteral(" · ") + choice.scope
                                  : choice.output.isEmpty() ? choice.kind
                                                            : choice.output;
            item->setData(subtitle_role, subtitle);
            item->setData(unavailable_role, !choice.unavailable_reason.isEmpty());
            item->setData(Qt::AccessibleDescriptionRole,
                          subtitle + " " + choice.unavailable_reason);
            item->setToolTip(choice.name + "\n" + subtitle + "\n" + choice.scope +
                             (choice.unavailable_reason.isEmpty()
                                  ? QString{}
                                  : "\n" + choice.unavailable_reason));
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
        const auto searchable = choice.name + " " + choice.kind + " " + choice.description + " " +
                                choice.scope + " " + choice.output;
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
    title_->setToolTip(title_->text());
    category_->setText(
        valid ? (found->saved ? QStringLiteral("项目模板") : QStringLiteral("内置报告"))
              : QStringLiteral("报告预览"));
    symbol_->setPixmap(workbench_icon(valid ? found->icon : WorkbenchIcon::search,
                                      theme::color(theme::accent_bright))
                           .pixmap(24, 24));
    description_->setText(valid ? found->description
                                : QStringLiteral("从列表选择报告，查看它的内容与适用对象。"));
    output_->setText(valid ? (found->output.isEmpty() ? found->kind : found->output) : QString{});
    output_heading_->setVisible(valid);
    scope_heading_->setVisible(valid);
    scope_->setText(valid ? found->scope : QString{});
    note_->setText(valid ? found->note : QString{});
    note_->setVisible(valid && !found->note.isEmpty());
    warning_->setText(valid ? found->unavailable_reason : QString{});
    warning_->setVisible(valid && !found->unavailable_reason.isEmpty());
    confirm_->setEnabled(valid && found->unavailable_reason.isEmpty());
    preview_scroll_->verticalScrollBar()->setValue(0);
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
    layout()->setContentsMargins(12, 12, 12, 12);
    if (split_->orientation() != orientation) {
        (split_->orientation() == Qt::Horizontal ? wide_sizes_ : narrow_sizes_) = split_->sizes();
        split_->setOrientation(orientation);
        const auto &saved = orientation == Qt::Horizontal ? wide_sizes_ : narrow_sizes_;
        split_->setSizes(saved.isEmpty() ? QList<int>{2, 1} : saved);
    }
    QTimer::singleShot(0, this, [this] {
        if (list_->currentItem())
            list_->scrollToItem(list_->currentItem());
    });
}
} // namespace orbitforge::desktop
