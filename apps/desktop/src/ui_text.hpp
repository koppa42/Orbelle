#pragma once
#include <QString>
#include <string_view>

namespace orbelle::desktop::ui {
// Display text lives in resources/text/zh_CN.json. Keys are stable identifiers,
// never object IDs, serialized values, unit symbols or user-authored names.
[[nodiscard]] QString text(std::string_view key);
// Mark keys in constexpr metadata so the resource validator can check them too.
[[nodiscard]] constexpr std::string_view key(std::string_view value) { return value; }
// For std::exception messages and APIs accepting UTF-8. Storage remains valid for
// the process lifetime; the catalogue is immutable after its first lookup.
[[nodiscard]] const char *utf8(std::string_view key);
} // namespace orbelle::desktop::ui
