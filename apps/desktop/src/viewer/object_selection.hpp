#pragma once
#include <QDialog>
#include <QPointer>
#include <QPushButton>
#include <initializer_list>
#include <orbitforge/application/project.hpp>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace orbitforge::desktop {
enum class ObjectKind { satellite, ground_station, sensor, other };
struct ObjectChoice {
    std::string id;
    QString name;
    ObjectKind kind = ObjectKind::other;
    std::string parent_id;
    QString path;
    bool selectable = true;
};
struct ObjectSelection {
    std::vector<std::string> ids;
    bool all = false;
    bool operator==(const ObjectSelection &) const = default;
};
struct ObjectSelectionOptions {
    QString purpose;
    bool multiple = false;
    // Empty disables the explicit all-objects option. Empty IDs alone never mean all.
    QString all_label;
};
std::vector<ObjectChoice>
project_object_choices(const application::Project &project,
                       std::initializer_list<ObjectKind> allowed = {
                           ObjectKind::satellite, ObjectKind::ground_station, ObjectKind::sensor});

class ObjectSelectionDialog final : public QDialog {
    Q_OBJECT
  public:
    ObjectSelectionDialog(std::vector<ObjectChoice> objects, ObjectSelectionOptions options,
                          ObjectSelection selection, QWidget *parent = nullptr);
    void set_objects(std::vector<ObjectChoice> objects);
    [[nodiscard]] ObjectSelection selection() const;

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    void filter();
    void update_status();
    void confirm();
    ObjectSelectionOptions options_;
    ObjectSelection value_;
    std::vector<ObjectChoice> objects_;
    bool rebuilding_ = false;
    QLineEdit *search_;
    QComboBox *kind_;
    QTreeWidget *tree_;
    QCheckBox *all_;
    QLabel *count_ = nullptr, *details_ = nullptr, *empty_ = nullptr;
    QPushButton *confirm_, *clear_;
};

// A compact field opens the same bounded selector used by multi-object report scopes.
class ObjectSelectionField final : public QPushButton {
    Q_OBJECT
  public:
    explicit ObjectSelectionField(QWidget *parent = nullptr);
    void set_options(ObjectSelectionOptions options);
    void set_objects(std::vector<ObjectChoice> objects);
    void set_selection(ObjectSelection selection);
    void set_id(std::string id);
    [[nodiscard]] const ObjectSelection &selection() const { return selection_; }
    [[nodiscard]] std::string id() const;
    [[nodiscard]] bool contains(const std::string &id) const;
    [[nodiscard]] const std::vector<ObjectChoice> &objects() const { return objects_; }
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;
    void choose();

  signals:
    void selectionChanged();

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    void refresh();
    ObjectSelectionOptions options_;
    ObjectSelection selection_;
    std::vector<ObjectChoice> objects_;
    QPointer<ObjectSelectionDialog> chooser_;
};
} // namespace orbitforge::desktop
