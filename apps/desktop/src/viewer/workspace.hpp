#pragma once
#include <QPointer>
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>

class QDialog;
class QIcon;
class QScrollArea;
class QStackedWidget;
class QTabBar;
class QToolButton;

namespace orbitforge::desktop {
// Shared host for view selectors and modeless document panels. Switching tabs never
// reapplies a document or reparents a native rendering window.
class Workspace final : public QWidget {
  public:
    explicit Workspace(QWidget *view, QWidget *parent = nullptr);
    ~Workspace() override;
    int add_view(const QString &title, const QIcon &icon);
    void select_view(int index);
    void indicate_view(int index);
    std::function<void(int)> view_requested;
    void open(QDialog *document);
    void close_document(QDialog *document);
    void detach_document(QDialog *document);
    void dock_document(QDialog *document);
    [[nodiscard]] bool contains(QDialog *document) const;
    [[nodiscard]] bool is_floating(QDialog *document) const;
    [[nodiscard]] QDialog *current_document() const;

  protected:
    bool eventFilter(QObject *object, QEvent *event) override;

  private:
    struct Document {
        QPointer<QDialog> content;
        QScrollArea *page = nullptr;
        QDialog *window = nullptr;
        bool open = false, floating = false;
    };
    Document *find(QDialog *document) const;
    int tab_for(const Document *document) const;
    void activate(int tab);
    void append_tab(Document &document);
    void update_title(Document &document);
    QWidget *view_;
    QTabBar *tabs_;
    QStackedWidget *pages_;
    QToolButton *detach_;
    int view_count_ = 0, last_view_ = 0;
    std::vector<std::unique_ptr<Document>> documents_;
};
} // namespace orbitforge::desktop
