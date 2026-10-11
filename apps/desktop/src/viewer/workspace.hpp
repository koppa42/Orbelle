#pragma once
#include <QIcon>
#include <QPointer>
#include <QWidget>
#include <memory>
#include <vector>

class QDialog;
class QLabel;
class QRubberBand;
class QSplitter;

namespace orbitforge::desktop {
// Views and documents keep their widgets and drafts when closed or moved.
class Workspace final : public QWidget {
  public:
    explicit Workspace(QWidget *parent = nullptr);
    ~Workspace() override;
    int add_view(QWidget *content, const QString &title, const QIcon &icon);
    void select_view(int index);
    // Documents with their own scrolling keep fixed actions outside the scrollable area.
    void open(QDialog *document, bool wrap_in_scroll_area = true);
    void close_page(QWidget *content);
    void detach_page(QWidget *content);
    void dock_page(QWidget *content);
    void split_page(QWidget *content, QWidget *beside, Qt::Edge edge);
    void merge_page(QWidget *content, QWidget *beside);
    [[nodiscard]] bool contains(QWidget *content) const;
    [[nodiscard]] bool is_floating(QWidget *content) const;
    [[nodiscard]] QDialog *current_document() const;

  protected:
    bool eventFilter(QObject *object, QEvent *event) override;

  private:
    struct Pane;
    struct Page {
        QPointer<QWidget> content;
        QWidget *host = nullptr;
        Pane *pane = nullptr;
        QString title;
        QIcon icon;
    };
    struct DropTarget {
        Pane *pane = nullptr;
        int insertion = -1;
        Qt::Edge edge{};
    };
    Page *find(QWidget *content) const;
    Page *current(Pane *pane) const;
    Pane *make_pane(QDialog *window = nullptr);
    Pane *main_pane() const;
    void append(Page &page, Pane *pane, int insertion = -1);
    void remove(Page &page);
    void activate(Page &page);
    void update_title(Page &page);
    void tidy(Pane *pane);
    void move(Page &page, Pane *target, Qt::Edge edge = {}, int insertion = -1);
    void context_menu(Pane *pane, int tab, const QPoint &global);
    DropTarget drop_target(const QPoint &global) const;
    void drag_to(Page *page, const QPoint &global);
    void finish_drag(Page *page, const QPoint &global, bool cancelled);
    QWidget *parking_;
    QSplitter *root_;
    Pane *active_ = nullptr;
    QRubberBand *drop_hint_;
    QLabel *drag_label_;
    std::vector<Pane *> panes_;
    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<Page *> views_;
};
} // namespace orbitforge::desktop
