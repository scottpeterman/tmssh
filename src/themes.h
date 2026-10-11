// src/themes.h -- terminal colour themes, as data.
//
// A theme is a terminal palette only: background, foreground, cursor,
// selection and the 16 ANSI colours. The application chrome is a separate
// dark/light setting and is never derived from the terminal theme, so a dark
// window around a light terminal is an ordinary combination.
//
// The file format is PathfinderSSH's theme YAML, read as-is:
//
//   name: "gruvbox-dark"
//   label: "Gruvbox Dark"
//   type: "dark"
//   terminal:
//     background: "#282828"
//     foreground: "#ebdbb2"
//     cursor: "#ebdbb2"
//     selection: "#504945"
//     black: "#282828"
//     ...
//     bright_white: "#ebdbb2"
//
// Colours are #rgb, #rrggbb, #rrggbbaa, rgb(r, g, b) or rgba(r, g, b, a).
// Nothing here touches GTK; theme_apply.h puts a Theme on a widget.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tmssh {

// Components 0..1, the same layout as GdkRGBA.
struct Rgba {
    float red = 0, green = 0, blue = 0, alpha = 1;
};

bool operator==(const Rgba &a, const Rgba &b);

// Parses one colour; nullopt if it is not one of the accepted forms.
std::optional<Rgba> parse_color(std::string_view text);

// The 16 ANSI keys, in palette order: black..white, then bright_black..
// bright_white.
extern const std::array<const char *, 16> kAnsiKeys;

inline constexpr const char *kDefaultTheme = "ice";

struct Theme {
    std::string name;
    std::string label;
    bool dark = true;
    Rgba background;
    Rgba foreground;
    Rgba cursor;
    // Painted over the text, so it always carries alpha. An opaque colour in
    // the file is given kSelectionAlpha on load, the same strength as the
    // widget's own default highlight.
    Rgba selection;
    std::array<Rgba, 16> ansi{};
};

inline constexpr float kSelectionAlpha = 110.0f / 255.0f;

struct ThemeParse {
    std::optional<Theme> theme;
    std::string error;
};

// Parses one theme file. Every colour key is required: a theme missing one
// would render with an arbitrary default in that slot, which is a worse
// failure than not offering the theme. The exceptions are cursor (defaults to
// the foreground) and selection (defaults to the foreground at
// kSelectionAlpha). label defaults to name; type defaults to dark.
ThemeParse parse_theme(std::string_view yaml);

// A set of themes by name. Adding a theme whose name is already present
// replaces it in place, which is how user themes override bundled ones.
class ThemeSet {
public:
    void add(Theme theme);

    const Theme *find(std::string_view name) const;

    // The named theme, or kDefaultTheme, or the first theme there is. An
    // unknown name in a session must never be why it does not open. nullptr
    // only when the set is empty.
    const Theme *resolve(std::string_view name) const;

    // Sorted by label, case-insensitively, for menus.
    std::vector<const Theme *> sorted() const;

    std::size_t size() const { return themes_.size(); }
    bool empty() const { return themes_.empty(); }

    // Loads every *.yaml / *.yml file in a directory. A missing directory is
    // not an error. A file that fails to parse is skipped and named in the
    // returned messages; the rest still load.
    std::vector<std::string> load_dir(const std::string &dir);

private:
    std::vector<Theme> themes_;
};

// $XDG_CONFIG_HOME/tmssh/themes, else ~/.config/tmssh/themes.
std::string user_themes_dir();

} // namespace tmssh
