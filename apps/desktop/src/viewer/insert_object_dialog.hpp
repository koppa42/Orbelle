#pragma once
#include "object_selection.hpp"
#include <QDialog>
#include <QSettings>
#include <functional>
#include <optional>
class QCheckBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace orbelle::desktop {
enum class InsertObjectType { satellite, ground_station, sensor, region };
enum class InsertObjectMethod { define_properties, defaults, orbit_wizard, tle_file };
struct InsertObjectRequest {
    InsertObjectType type;
    InsertObjectMethod method;
    std::string parent_id;
};
// Explicit local preferences keep isolated data homes and tests out of user settings.
class InsertObjectDialog final : public QDialog {
  public:
    InsertObjectDialog(const QString &preferences_file, QWidget *parent = nullptr);
    void set_scenario(const QString &name);
    void set_parents(std::vector<ObjectChoice> parents,
                     std::optional<std::string> selected = std::nullopt);
    void select_type(InsertObjectType type);
    void set_busy(bool busy);
    void show_result(const QString &message);
    [[nodiscard]] InsertObjectMethod default_method(InsertObjectType type) const;
    [[nodiscard]] std::optional<InsertObjectRequest> selection() const;
    std::function<void(InsertObjectRequest)> insert;

  private:
    void refresh_methods();
    void refresh_details();
    void submit();
    bool save_preferences();
    QSettings settings_;
    QTreeWidget *types_, *methods_;
    QLabel *destination_, *description_, *result_;
    QPushButton *insert_, *save_default_;
    QCheckBox *keep_open_;
    ObjectSelectionField *parent_;
    QLabel *parent_label_;
    bool busy_ = false;
};
} // namespace orbelle::desktop
