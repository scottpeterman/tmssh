// src/main.cpp
//
//   tmssh
//   tmssh --theme gruvbox-dark --app light
//   tmssh --font "JetBrains Mono 12"
//   tmssh --sessions ~/lab/sessions.yaml
//   tmssh --list-themes
//   tmssh --version
//
// --theme, --app and --font apply to this run; choices made from the menu
// are saved to ~/.config/tmssh/settings.yaml.

#include <gtk/gtk.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "app_window.h"
#include "theme_apply.h"
#include "themes.h"

namespace {

tmssh::LaunchOptions g_options;
tmssh::ThemeSet g_themes;

void on_activate(GtkApplication *app, gpointer) {
    // Owned by its window: freed when the window is destroyed.
    auto *window = new tmssh::AppWindow(app, g_themes, g_options);
    window->present();
}

void usage(std::ostream &out) {
    out << "usage: tmssh [--theme NAME] [--app dark|light] [--font \"Family Size\"]\n"
           "             [--sessions FILE] [--settings FILE] [--list-themes] [--version]\n";
}

void list_themes() {
    std::size_t width = 0;
    for (const tmssh::Theme *t : g_themes.sorted()) width = std::max(width, t->name.size());
    for (const tmssh::Theme *t : g_themes.sorted()) {
        std::cout << std::left << std::setw(static_cast<int>(width) + 2) << t->name
                  << std::setw(7) << (t->dark ? "dark" : "light") << t->label << "\n";
    }
}

// Takes "--opt value" or "--opt=value"; advances i past a separate value.
bool take(const char *name, int argc, char *argv[], int &i, std::string &out) {
    const std::size_t n = std::strlen(name);
    if (std::strcmp(argv[i], name) == 0) {
        if (i + 1 >= argc) return false;
        out = argv[++i];
        return true;
    }
    if (std::strncmp(argv[i], name, n) == 0 && argv[i][n] == '=') {
        out = argv[i] + n + 1;
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char *argv[]) {
    bool want_list = false;
    for (int i = 1; i < argc; ++i) {
        std::string value;
        if (take("--font", argc, argv, i, value)) {
            g_options.font = value;
        } else if (take("--theme", argc, argv, i, value)) {
            g_options.theme = value;
        } else if (take("--app", argc, argv, i, value)) {
            auto v = tmssh::parse_app_variant(value);
            if (!v) {
                std::cerr << "tmssh: --app takes dark or light\n";
                return 2;
            }
            g_options.app = *v;
        } else if (take("--sessions", argc, argv, i, value)) {
            g_options.sessions_path = value;
        } else if (take("--settings", argc, argv, i, value)) {
            g_options.settings_path = value;
        } else if (std::strcmp(argv[i], "--version") == 0) {
            std::cout << "tmssh " << TMSSH_VERSION << "\n";
            return 0;
        } else if (std::strcmp(argv[i], "--list-themes") == 0) {
            want_list = true;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage(std::cout);
            return 0;
        } else {
            usage(std::cerr);
            return 2;
        }
    }

    // The resource is registered by its constructor, so the pack is readable
    // before GTK starts.
    std::vector<std::string> messages;
    g_themes = tmssh::load_themes(messages);
    for (const auto &m : messages) std::cerr << "tmssh: theme " << m << "\n";

    if (want_list) {
        list_themes();
        return 0;
    }
    if (g_options.theme && g_themes.find(*g_options.theme) == nullptr) {
        std::cerr << "tmssh: unknown theme \"" << *g_options.theme << "\" (see --list-themes)\n";
        return 2;
    }

    tmssh::release_appimage_theme_pin();

    // Found once installed (hicolor) or inside the AppImage; harmless when not.
    gtk_window_set_default_icon_name("io.github.scottpeterman.tmssh");

    GtkApplication *app =
        gtk_application_new("io.github.scottpeterman.tmssh", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    const int status = g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);
    return status;
}
