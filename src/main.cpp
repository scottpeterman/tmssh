// src/main.cpp
//
// tmssh, interim: one terminal running a local program, with the theme pack
// and the app dark/light setting wired in. The session tree and tabs replace
// this window in the next step; the options stay.
//
//   tmssh
//   tmssh --theme gruvbox-dark --app light
//   tmssh --font "JetBrains Mono 12"
//   tmssh --list-themes
//   tmssh -- htop
//
// The default is a dark window around the light "ice" terminal, the same
// pairing PathfinderSSH ships.

#include <gtk/gtk.h>

#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "gtkpyte-terminal.h"
#include "ptysession.h"
#include "theme_apply.h"
#include "themes.h"

namespace {

struct Options {
    std::string font;
    std::string theme = tmssh::kDefaultTheme;
    tmssh::AppVariant app = tmssh::AppVariant::Dark;
    std::string program;
    std::vector<std::string> args;
};

Options g_options;
tmssh::ThemeSet g_themes;

std::string user_shell() {
    const char *env = std::getenv("SHELL");
    if (env != nullptr && *env != '\0') return env;
    const passwd *pw = getpwuid(getuid());
    if (pw != nullptr && pw->pw_shell != nullptr && *pw->pw_shell != '\0') return pw->pw_shell;
    return "/bin/sh";
}

gboolean destroy_window(gpointer window) {
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(window);
    return G_SOURCE_REMOVE;
}

void delete_session(gpointer session) { delete static_cast<gtkpyte::PtySession *>(session); }

void on_startup(GApplication *, gpointer) { tmssh::set_app_variant(g_options.app); }

void on_activate(GtkApplication *app, gpointer) {
    GtkWidget *window = gtk_application_window_new(app);
    GtkWidget *terminal = gtkpyte_terminal_new();
    auto *term = GTKPYTE_TERMINAL(terminal);
    if (!g_options.font.empty()) gtkpyte_terminal_set_font(term, g_options.font.c_str());
    if (const tmssh::Theme *t = g_themes.resolve(g_options.theme))
        tmssh::apply_terminal_theme(term, *t);

    const std::string program = g_options.program.empty() ? user_shell() : g_options.program;
    gtk_window_set_title(GTK_WINDOW(window), "tmssh");

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), terminal);
    gtk_window_set_child(GTK_WINDOW(window), scroller);

    auto *session = new gtkpyte::PtySession(term);
    g_object_set_data_full(G_OBJECT(window), "tmssh-session", session, delete_session);
    session->set_exit_handler([window](int status) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
            std::cerr << "tmssh: could not run the program (exit 127)\n";
        g_idle_add(destroy_window, g_object_ref(window));
    });

    if (!session->start(program, g_options.args)) {
        std::cerr << "tmssh: " << session->error() << "\n";
        gtk_window_destroy(GTK_WINDOW(window));
        return;
    }

    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(terminal);
}

void usage(std::ostream &out) {
    out << "usage: tmssh [--theme NAME] [--app dark|light] [--font \"Family Size\"]"
           " [--list-themes] [-- program [args...]]\n";
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
        } else if (std::strcmp(argv[i], "--list-themes") == 0) {
            want_list = true;
        } else if (std::strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) {
                g_options.program = argv[i + 1];
                for (int j = i + 2; j < argc; ++j) g_options.args.emplace_back(argv[j]);
            }
            break;
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
    if (g_themes.find(g_options.theme) == nullptr) {
        std::cerr << "tmssh: unknown theme \"" << g_options.theme << "\" (see --list-themes)\n";
        return 2;
    }

    GtkApplication *app =
        gtk_application_new("io.github.scottpeterman.tmssh", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    const int status = g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);
    return status;
}
