#pragma once
#include <optional>
class QString;
class QWidget;

namespace orbitforge::desktop {
enum class InsertObjectType { satellite };
enum class InsertObjectMethod { define_properties, defaults };
struct InsertObjectRequest {
    InsertObjectType type;
    InsertObjectMethod method;
};
[[nodiscard]] std::optional<InsertObjectRequest> insert_object_dialog(QWidget *parent,
                                                                      const QString &scenario_name);
} // namespace orbitforge::desktop
