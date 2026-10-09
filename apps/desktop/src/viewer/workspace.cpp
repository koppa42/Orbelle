#include "workspace.hpp"
#include "workbench_style.hpp"
#include <QDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace orbitforge::desktop {
Workspace::Workspace(QWidget *view, QWidget *parent) : QWidget(parent), view_(view) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 6, 0);
    bar->setSpacing(2);
    tabs_ = new QTabBar;
    tabs_->setObjectName("workspaceTabs");
    tabs_->setAccessibleName(QStringLiteral("主工作区页面"));
    tabs_->setExpanding(false);
    tabs_->setDrawBase(false);
    tabs_->setUsesScrollButtons(true);
    bar->addWidget(tabs_, 1);
    detach_ = new QToolButton;
    detach_->setObjectName("detachDocument");
    detach_->setIcon(workbench_icon(WorkbenchIcon::detach));
    detach_->setToolTip(QStringLiteral("在独立窗口打开当前页面"));
    detach_->setAccessibleName(detach_->toolTip());
    detach_->setEnabled(false);
    bar->addWidget(detach_);
    layout->addLayout(bar);
    pages_ = new QStackedWidget;
    pages_->addWidget(view_);
    layout->addWidget(pages_, 1);
    connect(tabs_, &QTabBar::currentChanged, this, [this](int tab) { activate(tab); });
    connect(detach_, &QToolButton::clicked, this, [this] { detach_document(current_document()); });
}
Workspace::~Workspace() {
    // Child destruction can emit title/close events; disconnect before members are destroyed.
    for (const auto &document : documents_) {
        if (document->content) {
            document->content->removeEventFilter(this);
            disconnect(document->content, nullptr, this, nullptr);
        }
        document->window->removeEventFilter(this);
    }
}
int Workspace::add_view(const QString &title, const QIcon &icon) {
    const QSignalBlocker blocker(tabs_);
    return tabs_->insertTab(view_count_++, icon, title);
}
void Workspace::select_view(int index) {
    if (index < 0 || index >= view_count_)
        return;
    const QSignalBlocker blocker(tabs_);
    tabs_->setCurrentIndex(index);
    activate(index);
}
void Workspace::indicate_view(int index) {
    last_view_ = index;
    if (!current_document()) {
        const QSignalBlocker blocker(tabs_);
        tabs_->setCurrentIndex(index);
    }
}
Workspace::Document *Workspace::find(QDialog *content) const {
    for (const auto &document : documents_)
        if (document->content == content)
            return document.get();
    return nullptr;
}
int Workspace::tab_for(const Document *document) const {
    for (int i = view_count_; i < tabs_->count(); ++i)
        if (tabs_->tabData(i).value<quintptr>() == reinterpret_cast<quintptr>(document))
            return i;
    return -1;
}
void Workspace::append_tab(Document &document) {
    const QSignalBlocker blocker(tabs_);
    const int tab =
        tabs_->addTab(workbench_icon(WorkbenchIcon::properties), document.content->windowTitle());
    tabs_->setTabData(tab, QVariant::fromValue(reinterpret_cast<quintptr>(&document)));
    auto *close = new QToolButton;
    close->setIcon(workbench_icon(WorkbenchIcon::close));
    close->setFixedSize(22, 22);
    close->setToolTip(QStringLiteral("关闭页面"));
    close->setAccessibleName(close->toolTip());
    connect(close, &QToolButton::clicked, this,
            [this, content = document.content] { close_document(content); });
    tabs_->setTabButton(tab, QTabBar::RightSide, close);
}
void Workspace::update_title(Document &document) {
    const auto tab = tab_for(&document);
    if (tab >= 0)
        tabs_->setTabText(tab, document.content->windowTitle());
    document.window->setWindowTitle(document.content->windowTitle());
}
void Workspace::activate(int tab) {
    if (tab < 0)
        return;
    if (tab < view_count_) {
        last_view_ = tab;
        pages_->setCurrentWidget(view_);
        detach_->setEnabled(false);
        if (view_requested)
            view_requested(tab);
    } else {
        auto *document = reinterpret_cast<Document *>(tabs_->tabData(tab).value<quintptr>());
        if (document && document->content) {
            pages_->setCurrentWidget(document->page);
            document->content->show();
            detach_->setEnabled(true);
        }
    }
}
void Workspace::open(QDialog *content) {
    auto *document = find(content);
    if (!document) {
        auto entry = std::make_unique<Document>();
        document = entry.get();
        entry->content = content;
        content->setWindowFlags(Qt::Widget);
        content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
        entry->page = new QScrollArea;
        entry->page->setFrameShape(QFrame::NoFrame);
        entry->page->setWidgetResizable(true);
        entry->page->setWidget(content);
        pages_->addWidget(entry->page);
        entry->window = new QDialog(this, Qt::Window);
        entry->window->setAttribute(Qt::WA_QuitOnClose, false);
        entry->window->resize(980, 680);
        auto *layout = new QVBoxLayout(entry->window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *bar = new QHBoxLayout;
        bar->addStretch();
        auto *dock = new QToolButton;
        dock->setText(QStringLiteral("放回工作区"));
        dock->setIcon(workbench_icon(WorkbenchIcon::views));
        dock->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        connect(dock, &QToolButton::clicked, this, [this, content] { dock_document(content); });
        bar->addWidget(dock);
        layout->addLayout(bar);
        content->installEventFilter(this);
        entry->window->installEventFilter(this);
        connect(content, &QWidget::windowTitleChanged, this,
                [this, document] { update_title(*document); });
        connect(content, &QDialog::finished, this, [this, content] { close_document(content); });
        documents_.push_back(std::move(entry));
    }
    if (document->floating && document->open) {
        document->window->show();
        document->window->raise();
        document->window->activateWindow();
        return;
    }
    if (!document->open) {
        document->open = true;
        append_tab(*document);
    }
    update_title(*document);
    const QSignalBlocker blocker(tabs_);
    tabs_->setCurrentIndex(tab_for(document));
    activate(tabs_->currentIndex());
}
void Workspace::close_document(QDialog *content) {
    auto *document = find(content);
    if (!document || !document->open)
        return;
    const bool was_current = current_document() == content;
    document->open = false;
    const auto tab = tab_for(document);
    if (tab >= 0) {
        const QSignalBlocker blocker(tabs_);
        tabs_->removeTab(tab);
    }
    if (document->floating) {
        document->window->hide();
        pages_->addWidget(document->page);
        document->floating = false;
    }
    document->page->hide();
    content->hide();
    if (was_current)
        select_view(last_view_);
}
void Workspace::detach_document(QDialog *content) {
    auto *document = find(content);
    if (!document || !document->open || document->floating)
        return;
    {
        const QSignalBlocker blocker(tabs_);
        tabs_->removeTab(tab_for(document));
    }
    document->floating = true;
    pages_->removeWidget(document->page);
    document->window->layout()->addWidget(document->page);
    document->page->show();
    content->show();
    select_view(last_view_);
    document->window->show();
    document->window->raise();
}
void Workspace::dock_document(QDialog *content) {
    auto *document = find(content);
    if (!document || !document->floating)
        return;
    document->window->hide();
    document->floating = false;
    pages_->addWidget(document->page);
    append_tab(*document);
    open(content);
}
bool Workspace::contains(QDialog *document) const {
    const auto *entry = find(document);
    return entry && entry->open;
}
bool Workspace::is_floating(QDialog *document) const {
    const auto *entry = find(document);
    return entry && entry->floating;
}
QDialog *Workspace::current_document() const {
    for (const auto &document : documents_)
        if (document->open && !document->floating && pages_->currentWidget() == document->page)
            return document->content;
    return nullptr;
}
bool Workspace::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::Close) {
        for (const auto &document : documents_)
            if (document->content == object || document->window == object) {
                close_document(document->content);
                event->ignore();
                return true;
            }
    }
    return QWidget::eventFilter(object, event);
}
} // namespace orbitforge::desktop
