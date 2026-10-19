#pragma once
#include <QColor>
#include <QMargins>
#include <QString>
#include <string_view>

// Workbench colour tokens: neutral midnight surfaces and a restrained sakura interaction
// accent taken from the startup artwork. Semantic status colours remain distinct. Stylesheets
// reference tokens as `@name` or `@name/NN` (NN = opacity in percent); painted widgets use
// the same constants so chrome, charts and overlays stay consistent.
//
// Legibility: text, textSoft and textMuted keep at least 4.5:1 contrast on every surface;
// textFaint is reserved for disabled content. Filled actions use dark ink on pale sakura.
namespace orbitforge::desktop::theme {
struct Token {
    std::string_view name;
    std::string_view value;
};

// Surfaces, darkest to lightest: the canvas shows between floating panels.
inline constexpr Token canvas{"canvas", "#10111b"};
inline constexpr Token chrome{"chrome", "#171925"};
inline constexpr Token panel{"panel", "#1a1c2a"};
inline constexpr Token surface{"surface", "#1d2030"};
inline constexpr Token raised{"raised", "#272a3d"};
inline constexpr Token overlay{"overlay", "#303449"};
inline constexpr Token field{"field", "#141622"};
inline constexpr Token field_muted{"fieldMuted", "#202333"};
// Lines and borders.
inline constexpr Token line{"line", "#303246"};
inline constexpr Token border{"border", "#42455d"};
inline constexpr Token border_strong{"borderStrong", "#70738d"};
// Text.
inline constexpr Token text{"text", "#eeedf5"};
inline constexpr Token text_soft{"textSoft", "#c9cada"};
inline constexpr Token text_muted{"textMuted", "#a9abc2"};
inline constexpr Token text_faint{"textFaint", "#636e90"};
// Sakura interaction accent; selected regions remain low contrast in area, high in text.
inline constexpr Token accent{"accent", "#efb2ce"};
inline constexpr Token accent_bright{"accentBright", "#ffdaea"};
inline constexpr Token accent_deep{"accentDeep", "#efb2ce"};
inline constexpr Token accent_wash{"accentWash", "#423044"};
inline constexpr Token accent_hover{"accentHover", "#302638"};
inline constexpr Token on_accent{"onAccent", "#30202d"};
// Sakura highlight for brand moments, the object's own interval and impulses.
inline constexpr Token sakura{"sakura", "#f4b8d4"};
inline constexpr Token sakura_deep{"sakuraDeep", "#de8cb5"};
inline constexpr Token sakura_wash{"sakuraWash", "#3a2844"};
// Status.
inline constexpr Token success{"success", "#8edcc6"};
inline constexpr Token success_wash{"successWash", "#173434"};
inline constexpr Token warning{"warning", "#ecc98f"};
inline constexpr Token warning_wash{"warningWash", "#372f24"};
inline constexpr Token danger{"danger", "#f2a5a3"};
inline constexpr Token danger_wash{"dangerWash", "#3b2430"};

inline constexpr Token all[] = {
    canvas,      chrome,       panel,      surface,      raised,        overlay,
    field,       field_muted,  line,       border,       border_strong, text,
    text_soft,   text_muted,   text_faint, accent,       accent_bright, accent_deep,
    accent_wash, accent_hover, on_accent,  sakura,       sakura_deep,   sakura_wash,
    success,     success_wash, warning,    warning_wash, danger,        danger_wash};

inline QColor color(const Token &token, int alpha = 255) {
    QColor result(QLatin1StringView(token.value.data(), qsizetype(token.value.size())));
    result.setAlpha(alpha);
    return result;
}

// Spacing scale on a 4 px grid (2, 6 and 10 for fine adjustment), shared by every window so
// pages read with one rhythm. Logical pixels.
namespace space {
inline constexpr int xs = 4;                  // icon to text, tight clusters
inline constexpr int sm = 8;                  // controls within one row
inline constexpr int row = 10;                // between form rows
inline constexpr int md = 12;                 // label to field; card padding
inline constexpr int lg = 16;                 // between groups and major blocks
inline constexpr int xl = 24;                 // page side margins
inline const QMargins page{24, 20, 24, 20};   // dialogs and document pages
inline const QMargins footer{24, 12, 24, 20}; // action row closing a page
inline const QMargins panel{16, 16, 16, 16};  // side panels and settings columns
inline const QMargins card{12, 10, 12, 10};   // cards, callouts and popups
} // namespace space

// Replaces `@token` and `@token/NN` references; throws std::invalid_argument for unknown
// names or opacities above 100 so a typo fails loudly instead of silently dropping a rule.
QString resolve(QString stylesheet);
} // namespace orbitforge::desktop::theme
