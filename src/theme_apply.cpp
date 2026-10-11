// src/theme_apply.cpp -- see theme_apply.h.
#include "theme_apply.h"

#include <gtk/gtk.h>

#include <algorithm>

namespace tmssh {

namespace {

constexpr const char *kThemeResourceDir = "/io/github/scottpeterman/tmssh/themes";

GdkRGBA to_gdk(const Rgba &c) { return GdkRGBA{c.red, c.green, c.blue, c.alpha}; }

} // namespace

std::optional<AppVariant> parse_app_variant(std::string_view s) {
    if (s == "dark") return AppVariant::Dark;
    if (s == "light") return AppVariant::Light;
    return std::nullopt;
}

const char *to_string(AppVariant v) { return v == AppVariant::Light ? "light" : "dark"; }

namespace {
bool g_pin_adwaita = false;
} // namespace

void release_appimage_theme_pin() {
    const char *appdir = g_getenv("APPDIR");
    const char *theme = g_getenv("GTK_THEME");
    if (appdir == nullptr || *appdir == '\0' || theme == nullptr) return;
    g_unsetenv("GTK_THEME");
    g_pin_adwaita = true;
}

void set_app_variant(AppVariant v) {
    GtkSettings *settings = gtk_settings_get_default();
    if (settings == nullptr) return;
    // Deprecated from GTK 4.20 in favour of GtkInterfaceColorScheme, which
    // the 4.8 floor (Debian 12, Crostini) does not have.
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    if (g_pin_adwaita) g_object_set(settings, "gtk-theme-name", "Adwaita", nullptr);
    g_object_set(settings, "gtk-application-prefer-dark-theme", v == AppVariant::Dark, nullptr);
    G_GNUC_END_IGNORE_DEPRECATIONS
}

void apply_terminal_theme(GtkpyteTerminal *terminal, const Theme &theme) {
    const GdkRGBA fg = to_gdk(theme.foreground);
    const GdkRGBA bg = to_gdk(theme.background);
    GdkRGBA palette[16];
    for (int i = 0; i < 16; ++i) palette[i] = to_gdk(theme.ansi[i]);
    gtkpyte_terminal_set_colors(terminal, &fg, &bg, palette, 16);

    const GdkRGBA cursor = to_gdk(theme.cursor);
    const GdkRGBA selection = to_gdk(theme.selection);
    gtkpyte_terminal_set_color_cursor(terminal, &cursor);
    gtkpyte_terminal_set_color_selection(terminal, &selection);
}

ThemeSet load_themes(std::vector<std::string> &messages) {
    ThemeSet set;

    GError *error = nullptr;
    char **names = g_resources_enumerate_children(kThemeResourceDir, G_RESOURCE_LOOKUP_FLAGS_NONE,
                                                  &error);
    if (names == nullptr) {
        messages.push_back(std::string("bundled themes: ") +
                           (error ? error->message : "not found"));
        g_clear_error(&error);
    } else {
        // Sorted so a duplicate name resolves the same way on every run.
        std::vector<std::string> files;
        for (char **n = names; *n != nullptr; ++n) files.emplace_back(*n);
        g_strfreev(names);
        std::sort(files.begin(), files.end());

        for (const auto &file : files) {
            const std::string path = std::string(kThemeResourceDir) + "/" + file;
            GBytes *bytes =
                g_resources_lookup_data(path.c_str(), G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
            if (bytes == nullptr) {
                messages.push_back(file + ": " + (error ? error->message : "unreadable"));
                g_clear_error(&error);
                continue;
            }
            gsize len = 0;
            const char *data = static_cast<const char *>(g_bytes_get_data(bytes, &len));
            ThemeParse r = parse_theme(std::string_view(data, len));
            g_bytes_unref(bytes);
            if (!r.theme) {
                messages.push_back(file + ": " + r.error);
                continue;
            }
            set.add(std::move(*r.theme));
        }
    }

    for (auto &m : set.load_dir(user_themes_dir())) messages.push_back(std::move(m));
    return set;
}

} // namespace tmssh
