#include "ui_text.hpp"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QResource>
#include <stdexcept>
#include <string>
#include <unordered_map>

// Resource symbols must be resolved in the global namespace. Explicitly referring
// to the initializer also retains the resource object when linking this static library.
static void initialize_text_resource() { Q_INIT_RESOURCE(desktop_text); }

namespace orbitforge::desktop::ui {
namespace {
struct Message {
    QString text;
    QByteArray utf8;
};
struct KeyHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view key) const {
        return std::hash<std::string_view>{}(key);
    }
};
using Catalogue = std::unordered_map<std::string, Message, KeyHash, std::equal_to<>>;
const Catalogue &catalogue() {
    // One read and UTF-8 conversion per entry, including for frequently painted labels.
    // Function-local initialization is thread safe; subsequent lookups only read data.
    static const auto messages = [] {
        initialize_text_resource();
        QFile file(QStringLiteral(":/orbitforge/text/zh_CN.json"));
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot open the embedded desktop text catalogue");
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            throw std::runtime_error("Invalid embedded desktop text catalogue");
        const auto root = document.object();
        if (root.value("locale").toString() != "zh-CN" || !root.value("messages").isObject())
            throw std::runtime_error("Invalid desktop text catalogue schema");
        const auto entries = root.value("messages").toObject();
        Catalogue result;
        result.reserve(static_cast<std::size_t>(entries.size()));
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it.key().isEmpty() || !it.value().isString() || it.value().toString().isEmpty())
                throw std::runtime_error("Invalid desktop text catalogue entry");
            auto value = it.value().toString();
            auto bytes = value.toUtf8();
            result.emplace(it.key().toStdString(), Message{std::move(value), std::move(bytes)});
        }
        return result;
    }();
    return messages;
}
const Message &message(std::string_view key) {
    const auto &messages = catalogue();
    const auto found = messages.find(key);
    if (found == messages.end())
        throw std::out_of_range("Unknown desktop text key: " + std::string(key));
    return found->second;
}
} // namespace
QString text(std::string_view key) { return message(key).text; }
const char *utf8(std::string_view key) { return message(key).utf8.constData(); }
} // namespace orbitforge::desktop::ui
