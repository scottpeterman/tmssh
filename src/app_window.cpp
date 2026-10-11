// src/app_window.cpp -- see app_window.h.
#include "app_window.h"

#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "gtkpyte-terminal.h"
#include "ptysession.h"

namespace fs = std::filesystem;

namespace tmssh {

namespace {

constexpr const char *kKeyFolder = "tmssh-folder";
constexpr const char *kKeySession = "tmssh-session";
constexpr const char *kKeyTab = "tmssh-tab";
constexpr const char *kKeyOwner = "tmssh-owner";

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

const char *data_str(GObject *obj, const char *key) {
    return static_cast<const char *>(g_object_get_data(obj, key));
}

// A row in the tree: a GtkStringObject carrying its folder and, for a
// session, the session's label.
GObject *make_row(const std::string &text, const std::string &folder, const std::string *session) {
    GtkStringObject *row = gtk_string_object_new(text.c_str());
    g_object_set_data_full(G_OBJECT(row), kKeyFolder, g_strdup(folder.c_str()), g_free);
    if (session != nullptr)
        g_object_set_data_full(G_OBJECT(row), kKeySession, g_strdup(session->c_str()), g_free);
    return G_OBJECT(row);
}

std::string user_shell() {
    const char *env = std::getenv("SHELL");
    if (env != nullptr && *env != '\0') return env;
    const passwd *pw = getpwuid(getuid());
    if (pw != nullptr && pw->pw_shell != nullptr && *pw->pw_shell != '\0') return pw->pw_shell;
    return "/bin/sh";
}

std::string exit_text(int status) {
    if (WIFEXITED(status)) return "exit " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return "signal " + std::to_string(WTERMSIG(status));
    return "ended";
}

// Focus the terminal once the current event is done with. Grabbing during a
// double-click hands focus over, then the button release gives it back to
// the row that was clicked, and typing lands in the tree.
void focus_later(GtkWidget *widget) {
    g_idle_add(
        [](gpointer w) -> gboolean {
            if (gtk_widget_get_mapped(GTK_WIDGET(w))) gtk_widget_grab_focus(GTK_WIDGET(w));
            g_object_unref(w);
            return G_SOURCE_REMOVE;
        },
        g_object_ref(widget));
}

// Tab label text, with the label's minimum width following it (see add_tab).
void set_tab_text(GtkWidget *label, const std::string &text) {
    gtk_label_set_text(GTK_LABEL(label), text.c_str());
    const auto len = static_cast<std::size_t>(g_utf8_strlen(text.c_str(), -1));
    gtk_label_set_width_chars(GTK_LABEL(label), static_cast<int>(std::min<std::size_t>(len, 30)));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Tabs
// ─────────────────────────────────────────────────────────────────────

struct AppWindow::Tab {
    AppWindow *owner = nullptr;
    GtkWidget *page = nullptr; // the scrolled window in the notebook
    GtkpyteTerminal *terminal = nullptr;
    GtkWidget *label = nullptr;
    std::string title;
    std::optional<Session> session; // nullopt: a local program
    std::string program;            // local program; empty means the shell
    std::vector<std::string> args;
    bool close_on_success = false; // an editor tab goes away when the editor exits cleanly
    std::unique_ptr<gtkpyte::PtySession> pty;
    bool running = false;
};

AppWindow::Tab *AppWindow::add_tab(const std::string &title, const std::string &tooltip) {
    auto *tab = new Tab;
    tab->owner = this;
    tab->title = title;

    GtkWidget *terminal = gtkpyte_terminal_new();
    tab->terminal = GTKPYTE_TERMINAL(terminal);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), terminal);
    gtk_widget_set_hexpand(scroller, TRUE);
    gtk_widget_set_vexpand(scroller, TRUE);
    tab->page = scroller;
    // The page owns the tab: closing it drops the pty, which hangs up the child.
    g_object_set_data_full(G_OBJECT(scroller), kKeyTab, tab,
                           [](gpointer p) { delete static_cast<Tab *>(p); });

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tab->label = gtk_label_new(nullptr);
    gtk_label_set_ellipsize(GTK_LABEL(tab->label), PANGO_ELLIPSIZE_END);
    // An ellipsizing label's minimum width is one "...", and the notebook
    // hands tabs their minimum; width-chars makes the minimum the text.
    gtk_label_set_max_width_chars(GTK_LABEL(tab->label), 30);
    set_tab_text(tab->label, title);
    gtk_box_append(GTK_BOX(box), tab->label);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    gtk_widget_set_focusable(close, FALSE);
    gtk_widget_set_tooltip_text(close, "Close tab (Ctrl+Shift+W)");
    g_object_set_data(G_OBJECT(close), kKeyTab, tab);
    g_signal_connect(close, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer) {
                         auto *t = static_cast<Tab *>(g_object_get_data(G_OBJECT(b), kKeyTab));
                         t->owner->close_tab(t);
                     }),
                     nullptr);
    gtk_box_append(GTK_BOX(box), close);
    if (!tooltip.empty()) gtk_widget_set_tooltip_text(box, tooltip.c_str());

    const int page =
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook_), scroller, box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebook_), scroller, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), page);
    return tab;
}

AppWindow::Tab *AppWindow::current_tab() {
    const int n = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook_));
    if (n < 0) return nullptr;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_), n);
    return page ? static_cast<Tab *>(g_object_get_data(G_OBJECT(page), kKeyTab)) : nullptr;
}

void AppWindow::apply_appearance(Tab *tab) {
    // Font: the application font, at the session's size if it sets one.
    const std::string font = options_.font.value_or(settings_.font);
    PangoFontDescription *desc = pango_font_description_from_string(font.c_str());
    if (tab->session && tab->session->font_size > 0)
        pango_font_description_set_size(desc, tab->session->font_size * PANGO_SCALE);
    gtkpyte_terminal_set_font_desc(tab->terminal, desc);
    pango_font_description_free(desc);

    int scrollback = settings_.scrollback_lines;
    if (tab->session && tab->session->scrollback_lines > 0)
        scrollback = tab->session->scrollback_lines;
    gtkpyte_terminal_set_scrollback(tab->terminal, scrollback);

    std::string theme = options_.theme.value_or(settings_.terminal_theme);
    if (tab->session && !tab->session->terminal_theme.empty() &&
        themes_.find(tab->session->terminal_theme) != nullptr)
        theme = tab->session->terminal_theme;
    if (const Theme *t = themes_.resolve(theme)) apply_terminal_theme(tab->terminal, *t);
}

void AppWindow::reapply_default_theme() {
    const int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook_));
    for (int i = 0; i < n; ++i) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_), i);
        auto *tab = static_cast<Tab *>(g_object_get_data(G_OBJECT(page), kKeyTab));
        if (tab) apply_appearance(tab);
    }
}

void AppWindow::start_tab(Tab *tab) {
    tab->pty.reset();
    tab->running = false;
    set_tab_text(tab->label, tab->title);

    std::string program;
    std::vector<std::string> args;
    std::vector<std::string> env;

    if (!tab->session) {
        program = tab->program.empty() ? user_shell() : tab->program;
        args = tab->args;
    } else {
        Command c = ssh_command(*tab->session, algorithms());
        program = c.program;
        args = c.args;
        env = c.env;
        const std::string banner = "\x1b[2m" + c.display() + "\x1b[0m\r\n";
        gtkpyte_terminal_feed(tab->terminal, banner.data(), banner.size());
    }

    tab->pty = std::make_unique<gtkpyte::PtySession>(tab->terminal);
    tab->pty->set_exit_handler([tab](int status) {
        tab->running = false;
        if (tab->close_on_success && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            // Deferred: this runs inside the session's own callback, and
            // removing the page deletes the session.
            struct Pending {
                GtkNotebook *notebook;
                GtkWidget *page;
            };
            auto *p = new Pending{GTK_NOTEBOOK(g_object_ref(tab->owner->notebook_)),
                                  GTK_WIDGET(g_object_ref(tab->page))};
            g_idle_add(
                [](gpointer data) -> gboolean {
                    auto *pd = static_cast<Pending *>(data);
                    const int n = gtk_notebook_page_num(pd->notebook, pd->page);
                    if (n >= 0) gtk_notebook_remove_page(pd->notebook, n);
                    g_object_unref(pd->page);
                    g_object_unref(pd->notebook);
                    delete pd;
                    return G_SOURCE_REMOVE;
                },
                p);
            return;
        }
        std::string text = "\r\n\x1b[2m[" + exit_text(status) + "]";
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
            text += " could not run the program; is it installed?";
        text += "  Ctrl+Shift+R reconnects, Ctrl+Shift+W closes.\x1b[0m\r\n";
        gtkpyte_terminal_feed(tab->terminal, text.data(), text.size());
        set_tab_text(tab->label, tab->title + " (ended)");
    });
    if (!tab->pty->start(program, args, env)) {
        const std::string text = "\r\ntmssh: " + tab->pty->error() + "\r\n";
        gtkpyte_terminal_feed(tab->terminal, text.data(), text.size());
        set_tab_text(tab->label, tab->title + " (failed)");
        tab->pty.reset();
        return;
    }
    tab->running = true;
}

void AppWindow::close_tab(Tab *tab) {
    const int n = gtk_notebook_page_num(GTK_NOTEBOOK(notebook_), tab->page);
    if (n >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(notebook_), n);
}

void AppWindow::open_program(const std::string &title, const std::string &program,
                             const std::vector<std::string> &args) {
    Tab *tab = add_tab(title, program);
    tab->program = program;
    tab->args = args;
    apply_appearance(tab);
    start_tab(tab);
    focus_later(GTK_WIDGET(tab->terminal));
}

void AppWindow::open_local() {
    Tab *tab = add_tab("local", user_shell());
    apply_appearance(tab);
    start_tab(tab);
    focus_later(GTK_WIDGET(tab->terminal));
}

void AppWindow::open_session(const std::string &folder, const std::string &name) {
    const int fi = tree_.folder_index(folder);
    if (fi < 0) return;
    const int si = tree_.folders[fi].session_index(name);
    if (si < 0) return;
    const Session &s = tree_.folders[fi].sessions[si];

    Tab *tab = add_tab(s.label(), s.target());
    tab->session = s;
    apply_appearance(tab);
    start_tab(tab);
    focus_later(GTK_WIDGET(tab->terminal));
}

const SshAlgorithms &AppWindow::algorithms() {
    // Probed on first use: three short ssh -Q runs, only when a session that
    // could need them is opened.
    if (!algorithms_) algorithms_ = probe_ssh_algorithms();
    return *algorithms_;
}

// ─────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────

AppWindow::AppWindow(GtkApplication *app, ThemeSet themes, LaunchOptions options)
    : app_(app), themes_(std::move(themes)), options_(std::move(options)) {
    sessions_path_ = options_.sessions_path.empty() ? default_sessions_path() : options_.sessions_path;
    settings_path_ = options_.settings_path.empty() ? default_settings_path() : options_.settings_path;

    SettingsLoad sl = load_settings(settings_path_);
    for (const auto &w : sl.warnings) std::cerr << "tmssh: settings " << w << "\n";
    settings_ = sl.settings;
    if (themes_.find(settings_.terminal_theme) == nullptr) settings_.terminal_theme = kDefaultTheme;

    LoadResult lr = load_file(sessions_path_);
    {
        std::ifstream in(sessions_path_, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        last_text_ = ss.str();
    }
    if (!lr.ok()) std::cerr << "tmssh: " << lr.error << "\n";
    for (const auto &w : lr.warnings) std::cerr << "tmssh: sessions: " << w << "\n";
    tree_ = std::move(lr.tree);

    const AppVariant variant =
        options_.app.value_or(parse_app_variant(settings_.app).value_or(AppVariant::Dark));
    set_app_variant(variant);

    build_ui();
    install_actions();
    rebuild_tree();
    watch_sessions_file();
    if (!lr.ok()) show_banner(lr.error);
}

AppWindow::~AppWindow() {
    if (reload_timer_ != 0) g_source_remove(reload_timer_);
    if (monitor_) {
        g_signal_handlers_disconnect_by_data(monitor_, this);
        g_file_monitor_cancel(monitor_);
        g_object_unref(monitor_);
    }
    if (tree_model_) g_object_unref(tree_model_);
}

void AppWindow::present() {
    gtk_window_present(GTK_WINDOW(window_));
    // After the window is up, so the dialog has a parent to sit on.
    offer_first_run_import();
}

void AppWindow::build_ui() {
    window_ = gtk_application_window_new(app_);
    gtk_window_set_title(GTK_WINDOW(window_), "tmssh");
    gtk_window_set_default_size(GTK_WINDOW(window_), settings_.window_width,
                                settings_.window_height);
    g_object_set_data(G_OBJECT(window_), kKeyOwner, this);
    g_signal_connect(window_, "close-request", G_CALLBACK(on_close_request), this);
    g_signal_connect(window_, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer self) {
                         delete static_cast<AppWindow *>(self);
                     }),
                     this);

    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), build_menu());
    gtk_widget_set_tooltip_text(menu_button, "Menu");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu_button);

    GtkWidget *new_tab = gtk_button_new_from_icon_name("tab-new-symbolic");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(new_tab), "win.local");
    gtk_widget_set_tooltip_text(new_tab, "New local terminal (Ctrl+Shift+T)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), new_tab);
    gtk_window_set_titlebar(GTK_WINDOW(window_), header);

    notebook_ = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook_), TRUE);
    gtk_widget_set_hexpand(notebook_, TRUE);
    gtk_widget_set_vexpand(notebook_, TRUE);
    g_signal_connect(notebook_, "switch-page", G_CALLBACK(on_switch_page), this);
    g_signal_connect(notebook_, "page-removed",
                     G_CALLBACK(+[](GtkNotebook *nb, GtkWidget *, guint, gpointer win) {
                         if (gtk_notebook_get_n_pages(nb) == 0)
                             gtk_window_set_title(GTK_WINDOW(win), "tmssh");
                     }),
                     window_);
    // Shown while there are no tabs.
    GtkWidget *empty = gtk_label_new("Double-click a session to connect,\n"
                                     "or Ctrl+Shift+T for a local terminal.");
    gtk_label_set_justify(GTK_LABEL(empty), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(empty, "dim-label");
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(stack), empty, "empty");
    gtk_stack_add_named(GTK_STACK(stack), notebook_, "tabs");
    auto update_stack = +[](GtkNotebook *nb, GtkWidget *, guint, gpointer stk) {
        // Tearing down the window removes every page after the stack has
        // started dropping its own children, and GtkStack's dispose runs
        // before the widget is marked as in destruction.
        const char *want = gtk_notebook_get_n_pages(nb) > 0 ? "tabs" : "empty";
        if (gtk_stack_get_child_by_name(GTK_STACK(stk), want) == nullptr) return;
        // An empty tab strip measures negative under Adwaita and GTK warns.
        gtk_notebook_set_show_tabs(nb, gtk_notebook_get_n_pages(nb) > 0);
        gtk_stack_set_visible_child_name(GTK_STACK(stk), want);
    };
    g_signal_connect_object(notebook_, "page-added", G_CALLBACK(update_stack), stack,
                            G_CONNECT_DEFAULT);
    g_signal_connect_object(notebook_, "page-removed", G_CALLBACK(update_stack), stack,
                            G_CONNECT_DEFAULT);

    paned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned_), build_sidebar());
    gtk_paned_set_end_child(GTK_PANED(paned_), stack);
    gtk_paned_set_resize_start_child(GTK_PANED(paned_), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned_), FALSE);
    gtk_paned_set_position(GTK_PANED(paned_), settings_.sidebar_width);
    gtk_window_set_child(GTK_WINDOW(window_), paned_);
}

GtkWidget *AppWindow::build_sidebar() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(box, 180, -1);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);

    filter_ = gtk_search_entry_new();
    g_object_set(filter_, "placeholder-text", "Filter sessions", nullptr);
    g_signal_connect(filter_, "search-changed", G_CALLBACK(on_filter_changed), this);
    gtk_box_append(GTK_BOX(box), filter_);

    banner_ = gtk_label_new(nullptr);
    gtk_label_set_wrap(GTK_LABEL(banner_), TRUE);
    gtk_label_set_xalign(GTK_LABEL(banner_), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(banner_), TRUE);
    gtk_widget_add_css_class(banner_, "error");
    gtk_widget_set_visible(banner_, FALSE);
    gtk_box_append(GTK_BOX(box), banner_);

    root_ = g_list_store_new(GTK_TYPE_STRING_OBJECT);
    tree_model_ = gtk_tree_list_model_new(G_LIST_MODEL(root_), FALSE, FALSE, create_child_model,
                                          this, nullptr);
    selection_ = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(tree_model_)));

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_factory_setup), this);
    g_signal_connect(factory, "bind", G_CALLBACK(on_factory_bind), this);
    g_signal_connect(factory, "unbind", G_CALLBACK(on_factory_unbind), this);

    list_view_ = gtk_list_view_new(GTK_SELECTION_MODEL(selection_), factory);
    gtk_widget_add_css_class(list_view_, "navigation-sidebar");
    g_signal_connect(list_view_, "activate", G_CALLBACK(on_row_activated), this);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list_view_);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);
    return box;
}

GMenuModel *AppWindow::build_menu() {
    GMenu *menu = g_menu_new();

    GMenu *tabs = g_menu_new();
    g_menu_append(tabs, "New Local Terminal", "win.local");
    g_menu_append(tabs, "Reconnect", "win.reconnect");
    g_menu_append(tabs, "Close Tab", "win.close-tab");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(tabs));
    g_object_unref(tabs);

    GMenu *import = g_menu_new();
    g_menu_append(import, "Import PathfinderSSH Sessions", "win.import-pathfinder");
    g_menu_append(import, "Import Session File…", "win.import-file");
    g_menu_append(import, "Edit Session File", "win.edit-sessions");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(import));
    g_object_unref(import);

    GMenu *appearance = g_menu_new();
    GMenu *window = g_menu_new();
    g_menu_append(window, "Dark", "win.app-variant::dark");
    g_menu_append(window, "Light", "win.app-variant::light");
    g_menu_append_submenu(appearance, "Window", G_MENU_MODEL(window));
    g_object_unref(window);

    GMenu *dark_themes = g_menu_new();
    GMenu *light_themes = g_menu_new();
    for (const Theme *t : themes_.sorted()) {
        const std::string action = "win.terminal-theme::" + t->name;
        // Menu labels read "_" as a mnemonic marker; "__" is a literal one.
        std::string label;
        for (char c : t->label) label += (c == '_') ? std::string("__") : std::string(1, c);
        g_menu_append(t->dark ? dark_themes : light_themes, label.c_str(), action.c_str());
    }
    GMenu *terminal = g_menu_new();
    g_menu_append_section(terminal, "Dark", G_MENU_MODEL(dark_themes));
    g_menu_append_section(terminal, "Light", G_MENU_MODEL(light_themes));
    g_object_unref(dark_themes);
    g_object_unref(light_themes);
    g_menu_append_submenu(appearance, "Terminal Theme", G_MENU_MODEL(terminal));
    g_object_unref(terminal);
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(appearance));
    g_object_unref(appearance);

    GMenu *app = g_menu_new();
    g_menu_append(app, "About tmssh", "win.about");
    g_menu_append(app, "Quit", "win.quit");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(app));
    g_object_unref(app);

    return G_MENU_MODEL(menu);
}

void AppWindow::install_actions() {
    const std::string variant =
        to_string(options_.app.value_or(parse_app_variant(settings_.app).value_or(AppVariant::Dark)));
    const std::string theme = options_.theme.value_or(settings_.terminal_theme);

    const GActionEntry entries[] = {
        {"local", act_local, nullptr, nullptr, nullptr, {}},
        {"connect", act_connect, nullptr, nullptr, nullptr, {}},
        {"delete", act_delete, nullptr, nullptr, nullptr, {}},
        {"reconnect", act_reconnect, nullptr, nullptr, nullptr, {}},
        {"close-tab", act_close_tab, nullptr, nullptr, nullptr, {}},
        {"next-tab", act_next_tab, nullptr, nullptr, nullptr, {}},
        {"prev-tab", act_prev_tab, nullptr, nullptr, nullptr, {}},
        {"find", act_find, nullptr, nullptr, nullptr, {}},
        {"import-pathfinder", act_import_pathfinder, nullptr, nullptr, nullptr, {}},
        {"import-file", act_import_file, nullptr, nullptr, nullptr, {}},
        {"edit-sessions", act_edit_sessions, nullptr, nullptr, nullptr, {}},
        {"about", act_about, nullptr, nullptr, nullptr, {}},
        {"quit", act_quit, nullptr, nullptr, nullptr, {}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(window_), entries, G_N_ELEMENTS(entries), this);

    // Radio actions: the state is the current choice.
    GSimpleAction *v = g_simple_action_new_stateful("app-variant", G_VARIANT_TYPE_STRING,
                                                    g_variant_new_string(variant.c_str()));
    g_signal_connect(v, "change-state", G_CALLBACK(act_app_variant), this);
    g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(v));
    g_object_unref(v);

    GSimpleAction *t = g_simple_action_new_stateful("terminal-theme", G_VARIANT_TYPE_STRING,
                                                    g_variant_new_string(theme.c_str()));
    g_signal_connect(t, "change-state", G_CALLBACK(act_terminal_theme), this);
    g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(t));
    g_object_unref(t);

    // Menu accelerators, and the same keys in the capture phase so they reach
    // the window before the terminal turns them into bytes for the remote end.
    struct Accel {
        const char *action;
        const char *keys;
    };
    const Accel accels[] = {
        {"win.local", "<Control><Shift>t"},      {"win.close-tab", "<Control><Shift>w"},
        {"win.reconnect", "<Control><Shift>r"},  {"win.next-tab", "<Control>Page_Down"},
        {"win.prev-tab", "<Control>Page_Up"},    {"win.find", "<Control><Shift>f"},
        {"win.quit", "<Control><Shift>q"},
    };
    GtkEventController *shortcuts = gtk_shortcut_controller_new();
    gtk_event_controller_set_propagation_phase(shortcuts, GTK_PHASE_CAPTURE);
    gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(shortcuts), GTK_SHORTCUT_SCOPE_GLOBAL);
    for (const Accel &a : accels) {
        const char *keys[] = {a.keys, nullptr};
        gtk_application_set_accels_for_action(app_, a.action, keys);
        gtk_shortcut_controller_add_shortcut(
            GTK_SHORTCUT_CONTROLLER(shortcuts),
            gtk_shortcut_new(gtk_shortcut_trigger_parse_string(a.keys),
                             gtk_named_action_new(a.action)));
    }
    gtk_widget_add_controller(window_, shortcuts);
}

// ─────────────────────────────────────────────────────────────────────
// Session tree
// ─────────────────────────────────────────────────────────────────────

bool AppWindow::session_matches(const Session &s) const {
    if (filter_text_.empty()) return true;
    return lower(s.label()).find(filter_text_) != std::string::npos ||
           lower(s.host).find(filter_text_) != std::string::npos ||
           lower(s.username).find(filter_text_) != std::string::npos;
}

GListModel *AppWindow::create_child_model(gpointer item, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    GObject *obj = G_OBJECT(item);
    if (data_str(obj, kKeySession) != nullptr) return nullptr; // sessions are leaves
    const char *folder = data_str(obj, kKeyFolder);
    const int fi = self->tree_.folder_index(folder ? folder : "");
    if (fi < 0) return nullptr;

    GListStore *store = g_list_store_new(GTK_TYPE_STRING_OBJECT);
    const Folder &f = self->tree_.folders[fi];
    for (const auto &s : f.sessions) {
        if (!self->session_matches(s)) continue;
        const std::string label = s.label();
        GObject *row = make_row(label, f.name, &label);
        g_list_store_append(store, row);
        g_object_unref(row);
    }
    return G_LIST_MODEL(store);
}

void AppWindow::capture_expanded() {
    // Only the unfiltered view's expansion is remembered; a filter expands
    // everything it shows, and clearing it puts the tree back as it was.
    if (!filter_text_.empty() || first_build_) return;
    expanded_.clear();
    const guint n = g_list_model_get_n_items(G_LIST_MODEL(tree_model_));
    for (guint i = 0; i < n; ++i) {
        GtkTreeListRow *row = gtk_tree_list_model_get_row(tree_model_, i);
        if (row == nullptr) continue;
        GObject *item = G_OBJECT(gtk_tree_list_row_get_item(row));
        if (data_str(item, kKeySession) == nullptr && gtk_tree_list_row_get_expanded(row))
            expanded_.insert(lower(data_str(item, kKeyFolder)));
        g_object_unref(item);
        g_object_unref(row);
    }
}

void AppWindow::rebuild_tree(bool capture) {
    if (capture) capture_expanded();

    g_list_store_remove_all(root_);
    for (const auto &f : tree_.folders) {
        int shown = 0;
        for (const auto &s : f.sessions)
            if (session_matches(s)) ++shown;
        if (!filter_text_.empty() && shown == 0) continue;
        GObject *row = make_row(f.name, f.name, nullptr);
        g_object_set_data(row, "tmssh-count", GINT_TO_POINTER(shown));
        g_list_store_append(root_, row);
        g_object_unref(row);
    }

    // Expanding inserts rows below the one expanded, so walk forward and
    // re-read the count as it grows.
    for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(tree_model_)); ++i) {
        GtkTreeListRow *row = gtk_tree_list_model_get_row(tree_model_, i);
        if (row == nullptr) continue;
        GObject *item = G_OBJECT(gtk_tree_list_row_get_item(row));
        if (data_str(item, kKeySession) == nullptr) {
            const bool expand = first_build_ || !filter_text_.empty() ||
                                expanded_.count(lower(data_str(item, kKeyFolder))) > 0;
            gtk_tree_list_row_set_expanded(row, expand);
        }
        g_object_unref(item);
        g_object_unref(row);
    }
    first_build_ = false;
}

void AppWindow::on_factory_setup(GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
    GtkWidget *expander = gtk_tree_expander_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new();
    GtkWidget *label = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    gtk_list_item_set_child(item, expander);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_row_pressed), item);
    gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(click));
}

void AppWindow::on_factory_bind(GtkSignalListItemFactory *, GtkListItem *item, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    auto *row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(item));
    GtkWidget *expander = gtk_list_item_get_child(item);
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
    g_object_set_data(G_OBJECT(expander), kKeyOwner, self);

    GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
    GtkWidget *icon = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_next_sibling(icon);

    GObject *obj = G_OBJECT(gtk_tree_list_row_get_item(row));
    const char *text = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
    const char *session = data_str(obj, kKeySession);
    if (session == nullptr) {
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), "folder-symbolic");
        char *esc = g_markup_escape_text(text, -1);
        const int count = GPOINTER_TO_INT(g_object_get_data(obj, "tmssh-count"));
        const std::string markup = std::string("<b>") + esc + "</b>  <span alpha=\"55%\">" +
                                   std::to_string(count) + "</span>";
        g_free(esc);
        gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
        gtk_widget_set_tooltip_text(expander, nullptr);
    } else {
        const char *folder = data_str(obj, kKeyFolder);
        const int fi = self->tree_.folder_index(folder ? folder : "");
        const int si = fi >= 0 ? self->tree_.folders[fi].session_index(session) : -1;
        const Session *s = si >= 0 ? &self->tree_.folders[fi].sessions[si] : nullptr;
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), "utilities-terminal-symbolic");
        gtk_label_set_text(GTK_LABEL(label), text);
        if (s) gtk_widget_set_tooltip_text(expander, s->target().c_str());
    }
    g_object_unref(obj);
}

void AppWindow::on_factory_unbind(GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
    GtkWidget *expander = gtk_list_item_get_child(item);
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), nullptr);
}

bool AppWindow::selected(std::string &folder, std::string &session) const {
    auto *row = GTK_TREE_LIST_ROW(gtk_single_selection_get_selected_item(selection_));
    if (row == nullptr) return false;
    GObject *obj = G_OBJECT(gtk_tree_list_row_get_item(row));
    const char *f = data_str(obj, kKeyFolder);
    const char *s = data_str(obj, kKeySession);
    folder = f ? f : "";
    session = s ? s : "";
    g_object_unref(obj);
    return true;
}

void AppWindow::activate_row(guint position) {
    GtkTreeListRow *row = gtk_tree_list_model_get_row(tree_model_, position);
    if (row == nullptr) return;
    GObject *obj = G_OBJECT(gtk_tree_list_row_get_item(row));
    const char *folder = data_str(obj, kKeyFolder);
    const char *session = data_str(obj, kKeySession);
    if (session != nullptr)
        open_session(folder ? folder : "", session);
    else
        gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
    g_object_unref(obj);
    g_object_unref(row);
}

void AppWindow::on_row_activated(GtkListView *, guint position, gpointer self) {
    static_cast<AppWindow *>(self)->activate_row(position);
}

void AppWindow::on_row_pressed(GtkGestureClick *g, int, double x, double y, gpointer data) {
    auto *item = GTK_LIST_ITEM(data);
    GtkWidget *expander = gtk_list_item_get_child(item);
    auto *self = static_cast<AppWindow *>(g_object_get_data(G_OBJECT(expander), kKeyOwner));
    if (self == nullptr) return;
    gtk_single_selection_set_selected(self->selection_, gtk_list_item_get_position(item));
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    self->show_row_menu(expander, x, y);
}

void AppWindow::show_row_menu(GtkWidget *row_widget, double x, double y) {
    std::string folder, session;
    if (!selected(folder, session)) return;

    GMenu *menu = g_menu_new();
    if (!session.empty()) {
        g_menu_append(menu, "Connect", "win.connect");
        g_menu_append(menu, "Delete Session", "win.delete");
    } else {
        g_menu_append(menu, "Delete Folder", "win.delete");
    }
    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_widget_set_parent(popover, row_widget);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    const GdkRectangle at = {static_cast<int>(x), static_cast<int>(y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &at);
    // Unparented once closed; the idle lets an activated item finish first.
    g_signal_connect(popover, "closed", G_CALLBACK(+[](GtkPopover *p, gpointer) {
                         g_idle_add(
                             [](gpointer w) -> gboolean {
                                 gtk_widget_unparent(GTK_WIDGET(w));
                                 return G_SOURCE_REMOVE;
                             },
                             p);
                     }),
                     nullptr);
    gtk_popover_popup(GTK_POPOVER(popover));
}

void AppWindow::on_filter_changed(GtkSearchEntry *entry, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    std::string text = lower(gtk_editable_get_text(GTK_EDITABLE(entry)));
    // Capture the unfiltered expansion before the first keystroke hides it.
    if (self->filter_text_.empty() && !text.empty()) self->capture_expanded();
    self->filter_text_ = text;
    // Never capture here: on the keystroke that clears the filter, the view
    // still shows the filtered expansion, which is not the one to keep.
    self->rebuild_tree(false);
}

// ─────────────────────────────────────────────────────────────────────
// Window events
// ─────────────────────────────────────────────────────────────────────

void AppWindow::on_switch_page(GtkNotebook *, GtkWidget *page, guint, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    auto *tab = static_cast<Tab *>(g_object_get_data(G_OBJECT(page), kKeyTab));
    const std::string title = tab ? "tmssh — " + tab->title : "tmssh";
    gtk_window_set_title(GTK_WINDOW(self->window_), title.c_str());
    if (tab) focus_later(GTK_WIDGET(tab->terminal));
}

gboolean AppWindow::on_close_request(GtkWindow *window, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    if (!gtk_window_is_maximized(window)) {
        // In GTK 4 the default size follows the window as it is resized, and
        // unlike the widget size it is the same measure set_default_size
        // takes, so a save/restore cycle does not drift.
        int w = 0, h = 0;
        gtk_window_get_default_size(window, &w, &h);
        if (w > 0) self->settings_.window_width = w;
        if (h > 0) self->settings_.window_height = h;
    }
    self->settings_.sidebar_width = gtk_paned_get_position(GTK_PANED(self->paned_));
    self->save_settings_now();
    return FALSE; // let it close
}

// ─────────────────────────────────────────────────────────────────────
// Persistence and dialogs
// ─────────────────────────────────────────────────────────────────────

void AppWindow::save_sessions() {
    const std::string text = emit_tree(tree_);
    if (auto err = write_text_atomic(sessions_path_, text); !err.empty()) {
        message("Could not save sessions", err);
        return;
    }
    last_text_ = text;
    show_banner("");
    watch_sessions_file(); // the file may not have existed until now
}

void AppWindow::write_seed() {
    const passwd *pw = getpwuid(getuid());
    const std::string user = (pw && pw->pw_name) ? pw->pw_name : "";
    const std::string text = seed_sessions_yaml(user);
    if (auto err = write_text_atomic(sessions_path_, text); !err.empty()) {
        message("Could not create the session file", err);
        return;
    }
    last_text_ = text;
    LoadResult lr = parse_tree(text);
    tree_ = std::move(lr.tree);
    expanded_.insert("local");
    rebuild_tree(false);
    watch_sessions_file();
}

// ─────────────────────────────────────────────────────────────────────
// The session file: edited by hand, reloaded on save
// ─────────────────────────────────────────────────────────────────────

void AppWindow::show_banner(const std::string &text) {
    gtk_label_set_text(GTK_LABEL(banner_), text.c_str());
    gtk_widget_set_visible(banner_, !text.empty());
}

void AppWindow::watch_sessions_file() {
    if (monitor_ != nullptr) return;
    std::error_code ec;
    if (!fs::exists(sessions_path_, ec)) return;
    GFile *file = g_file_new_for_path(sessions_path_.c_str());
    GError *error = nullptr;
    // WATCH_MOVES: editors that save by writing a temp file and renaming it
    // over this one (vim, VS Code, tmssh itself) show up as a move.
    monitor_ = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, &error);
    g_object_unref(file);
    if (monitor_ == nullptr) {
        std::cerr << "tmssh: cannot watch " << sessions_path_ << ": "
                  << (error ? error->message : "unknown error") << "\n";
        g_clear_error(&error);
        return;
    }
    g_signal_connect(monitor_, "changed", G_CALLBACK(on_sessions_changed), this);
}

void AppWindow::on_sessions_changed(GFileMonitor *, GFile *, GFile *, GFileMonitorEvent event,
                                    gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    switch (event) {
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_RENAMED:
    case G_FILE_MONITOR_EVENT_CHANGED:
        break;
    default:
        return;
    }
    // One save is several events; settle for a moment and read once.
    if (self->reload_timer_ != 0) g_source_remove(self->reload_timer_);
    self->reload_timer_ = g_timeout_add(
        300,
        [](gpointer p) -> gboolean {
            auto *me = static_cast<AppWindow *>(p);
            me->reload_timer_ = 0;
            me->reload_sessions();
            return G_SOURCE_REMOVE;
        },
        self);
}

void AppWindow::reload_sessions() {
    std::error_code ec;
    if (!fs::is_regular_file(sessions_path_, ec)) return; // mid-save, or removed: keep what we have
    std::ifstream in(sessions_path_, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    if (text == last_text_) {
        // Our own save, or the file is back to what is already loaded: in
        // either case any earlier complaint about it no longer applies.
        show_banner("");
        return;
    }

    LoadResult lr = parse_tree(text);
    if (!lr.ok()) {
        // Keep the tree that worked; say what is wrong with the file.
        show_banner("Session file not loaded: " + lr.error);
        return;
    }
    last_text_ = text;
    capture_expanded();
    std::set<std::string> before;
    for (const auto &f : tree_.folders) before.insert(lower(f.name));
    tree_ = std::move(lr.tree);
    // A folder just added in the editor opens expanded, so it is seen.
    for (const auto &f : tree_.folders)
        if (before.count(lower(f.name)) == 0) expanded_.insert(lower(f.name));
    rebuild_tree(false);
    if (lr.warnings.empty())
        show_banner("");
    else
        show_banner(std::to_string(lr.warnings.size()) + " problem(s) in the session file; first: " +
                    lr.warnings.front());
}

void AppWindow::edit_sessions_file() {
    std::error_code ec;
    if (!fs::exists(sessions_path_, ec)) save_sessions();

    // The desktop's editor for the file first.
    GFile *file = g_file_new_for_path(sessions_path_.c_str());
    char *uri = g_file_get_uri(file);
    g_object_unref(file);
    GError *error = nullptr;
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gtk_widget_get_display(window_));
    const gboolean ok = g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(ctx), &error);
    g_object_unref(ctx);
    g_free(uri);
    if (ok) return;
    g_clear_error(&error);

    // No handler for YAML: the user's terminal editor, in a tab.
    std::string editor;
    for (const char *var : {"VISUAL", "EDITOR"}) {
        const char *v = std::getenv(var);
        if (v != nullptr && *v != '\0') {
            editor = v;
            break;
        }
    }
    if (editor.empty()) {
        for (const char *candidate : {"sensible-editor", "nano", "vi"}) {
            char *found = g_find_program_in_path(candidate);
            if (found != nullptr) {
                editor = found;
                g_free(found);
                break;
            }
        }
    }
    if (editor.empty()) {
        message("No editor found", "Open " + sessions_path_ + " in any editor; tmssh reloads it on save.");
        return;
    }
    // $EDITOR may carry arguments ("code -w"); let the shell split it.
    open_program("sessions.yaml", "/bin/sh", {"-c", editor + " \"$1\"", "sh", sessions_path_});
    if (Tab *tab = current_tab()) tab->close_on_success = true;
}

void AppWindow::act_edit_sessions(GSimpleAction *, GVariant *, gpointer self) {
    static_cast<AppWindow *>(self)->edit_sessions_file();
}

void AppWindow::save_settings_now() {
    if (auto err = save_settings(settings_path_, settings_); !err.empty())
        std::cerr << "tmssh: " << err << "\n";
}

void AppWindow::confirm(const std::string &text, const std::string &detail,
                        const std::string &ok_label, std::function<void()> on_ok) {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
                                               text.c_str());
    if (!detail.empty())
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail.c_str());
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), ok_label.c_str(), GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    auto *cb = new std::function<void()>(std::move(on_ok));
    g_object_set_data_full(G_OBJECT(dialog), "tmssh-cb", cb,
                           [](gpointer p) { delete static_cast<std::function<void()> *>(p); });
    g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkDialog *d, int response, gpointer) {
                         auto *f = static_cast<std::function<void()> *>(
                             g_object_get_data(G_OBJECT(d), "tmssh-cb"));
                         const bool ok = response == GTK_RESPONSE_ACCEPT;
                         std::function<void()> run = ok ? *f : nullptr;
                         gtk_window_destroy(GTK_WINDOW(d));
                         if (run) run();
                     }),
                     nullptr);
    gtk_window_present(GTK_WINDOW(dialog));
}

void AppWindow::message(const std::string &text, const std::string &detail) {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s",
                                               text.c_str());
    if (!detail.empty())
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail.c_str());
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_window_destroy), nullptr);
    gtk_window_present(GTK_WINDOW(dialog));
}

void AppWindow::offer_first_run_import() {
    std::error_code ec;
    if (fs::exists(sessions_path_, ec)) return; // not a first run
    const std::string source = pathfinder_sessions_path();
    LoadResult lr;
    if (fs::is_regular_file(source, ec)) lr = load_file(source);
    if (!lr.ok() || lr.tree.session_count() == 0) {
        write_seed(); // nothing to offer: start with localhost
        return;
    }

    std::string detail = "Found " + std::to_string(lr.tree.session_count()) + " session(s) in " +
                         std::to_string(lr.tree.folders.size()) + " folder(s) in " + source + ".";
    if (lr.skipped_unsupported > 0)
        detail += " " + std::to_string(lr.skipped_unsupported) +
                  " serial or telnet session(s) will be skipped; tmssh is ssh only.";
    detail += "\n\nPathfinderSSH's file is only read, never changed. "
              "You can import again later from the menu.";

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                               "Import your PathfinderSSH sessions?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail.c_str());
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Start Empty", GTK_RESPONSE_REJECT);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Import", GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_object_set_data(G_OBJECT(dialog), kKeyOwner, this);
    g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkDialog *d, int response, gpointer) {
                         auto *self = static_cast<AppWindow *>(
                             g_object_get_data(G_OBJECT(d), kKeyOwner));
                         gtk_window_destroy(GTK_WINDOW(d));
                         if (response == GTK_RESPONSE_ACCEPT) {
                             self->import_from(pathfinder_sessions_path());
                         } else {
                             // Either answer is final: once the file exists
                             // the question is not asked again.
                             self->write_seed();
                         }
                     }),
                     nullptr);
    gtk_window_present(GTK_WINDOW(dialog));
}

void AppWindow::import_from(const std::string &path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        message("Nothing to import", path + " does not exist.");
        return;
    }
    LoadResult lr = load_file(path);
    if (!lr.ok()) {
        message("Could not read " + path, lr.error);
        return;
    }
    capture_expanded();
    std::set<std::string> before;
    for (const auto &f : tree_.folders) before.insert(lower(f.name));
    ImportResult r = import_tree(tree_, lr.tree);
    // Folders the import created open expanded, so what came in is visible.
    for (const auto &f : tree_.folders)
        if (before.count(lower(f.name)) == 0) expanded_.insert(lower(f.name));
    save_sessions();
    rebuild_tree(false);

    std::string detail = std::to_string(r.added) + " added";
    if (!r.skipped.empty()) detail += ", " + std::to_string(r.skipped.size()) + " already here";
    if (!r.renamed.empty())
        detail += ", " + std::to_string(r.renamed.size()) + " renamed to avoid a clash";
    if (!r.rejected.empty()) detail += ", " + std::to_string(r.rejected.size()) + " without a host";
    if (lr.skipped_unsupported > 0)
        detail += ", " + std::to_string(lr.skipped_unsupported) + " serial or telnet skipped";
    detail += ".";
    if (!lr.warnings.empty())
        detail += "\n\n" + std::to_string(lr.warnings.size()) +
                  " problem(s) in the file; the first: " + lr.warnings.front();
    message("Imported from " + fs::path(path).filename().string(), detail);
}

// ─────────────────────────────────────────────────────────────────────
// Actions
// ─────────────────────────────────────────────────────────────────────

void AppWindow::act_local(GSimpleAction *, GVariant *, gpointer self) {
    static_cast<AppWindow *>(self)->open_local();
}

void AppWindow::act_connect(GSimpleAction *, GVariant *, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    std::string folder, session;
    if (self->selected(folder, session) && !session.empty()) self->open_session(folder, session);
}

void AppWindow::act_delete(GSimpleAction *, GVariant *, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    std::string folder, session;
    if (!self->selected(folder, session)) return;
    if (!session.empty()) {
        self->confirm("Delete session \"" + session + "\"?", "From folder \"" + folder + "\".",
                      "Delete", [self, folder, session] {
                          if (self->tree_.remove(folder, session).empty()) {
                              self->save_sessions();
                              self->rebuild_tree();
                          }
                      });
        return;
    }
    const int fi = self->tree_.folder_index(folder);
    const std::size_t n = fi >= 0 ? self->tree_.folders[fi].sessions.size() : 0;
    self->confirm("Delete folder \"" + folder + "\"?",
                  n == 0 ? std::string("It is empty.")
                         : "It holds " + std::to_string(n) + " session(s), which go with it.",
                  "Delete", [self, folder] {
                      if (self->tree_.remove_folder(folder, true).empty()) {
                          self->save_sessions();
                          self->rebuild_tree();
                      }
                  });
}

void AppWindow::act_reconnect(GSimpleAction *, GVariant *, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    if (Tab *tab = self->current_tab()) {
        const char *sep = "\r\n";
        gtkpyte_terminal_feed(tab->terminal, sep, 2);
        self->start_tab(tab);
    }
}

void AppWindow::act_close_tab(GSimpleAction *, GVariant *, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    if (Tab *tab = self->current_tab()) self->close_tab(tab);
}

void AppWindow::act_next_tab(GSimpleAction *, GVariant *, gpointer data) {
    auto *nb = GTK_NOTEBOOK(static_cast<AppWindow *>(data)->notebook_);
    const int n = gtk_notebook_get_n_pages(nb);
    if (n > 0) gtk_notebook_set_current_page(nb, (gtk_notebook_get_current_page(nb) + 1) % n);
}

void AppWindow::act_prev_tab(GSimpleAction *, GVariant *, gpointer data) {
    auto *nb = GTK_NOTEBOOK(static_cast<AppWindow *>(data)->notebook_);
    const int n = gtk_notebook_get_n_pages(nb);
    if (n > 0) gtk_notebook_set_current_page(nb, (gtk_notebook_get_current_page(nb) + n - 1) % n);
}

void AppWindow::act_find(GSimpleAction *, GVariant *, gpointer data) {
    gtk_widget_grab_focus(static_cast<AppWindow *>(data)->filter_);
}

void AppWindow::act_import_pathfinder(GSimpleAction *, GVariant *, gpointer data) {
    static_cast<AppWindow *>(data)->import_from(pathfinder_sessions_path());
}

void AppWindow::act_import_file(GSimpleAction *, GVariant *, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    // GtkFileChooserNative: deprecated from 4.10, but GtkFileDialog does not
    // exist on the 4.8 floor.
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Import Session File", GTK_WINDOW(self->window_), GTK_FILE_CHOOSER_ACTION_OPEN, "Import",
        "Cancel");
    GtkFileFilter *yaml = gtk_file_filter_new();
    gtk_file_filter_set_name(yaml, "Session files (*.yaml, *.yml)");
    gtk_file_filter_add_pattern(yaml, "*.yaml");
    gtk_file_filter_add_pattern(yaml, "*.yml");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), yaml);
    g_object_unref(yaml);
    g_object_set_data(G_OBJECT(chooser), kKeyOwner, self);
    g_signal_connect(chooser, "response",
                     G_CALLBACK(+[](GtkNativeDialog *d, int response, gpointer) {
                         auto *owner = static_cast<AppWindow *>(
                             g_object_get_data(G_OBJECT(d), kKeyOwner));
                         if (response == GTK_RESPONSE_ACCEPT) {
                             GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d));
                             char *path = file ? g_file_get_path(file) : nullptr;
                             if (path) owner->import_from(path);
                             g_free(path);
                             if (file) g_object_unref(file);
                         }
                         g_object_unref(d);
                     }),
                     nullptr);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

void AppWindow::act_about(GSimpleAction *, GVariant *, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    const char *authors[] = {"Scott Peterman", nullptr};
    gtk_show_about_dialog(GTK_WINDOW(self->window_), "program-name", "tmssh", "comments",
                          "Tiny Managed SSH: a session manager and ssh terminal on gtkpyte.",
                          "version", "0.1.0", "website", "https://github.com/scottpeterman/tmssh",
                          "license-type", GTK_LICENSE_GPL_3_0, "authors", authors, "logo-icon-name",
                          "utilities-terminal", nullptr);
}

void AppWindow::act_quit(GSimpleAction *, GVariant *, gpointer data) {
    gtk_window_close(GTK_WINDOW(static_cast<AppWindow *>(data)->window_));
}

void AppWindow::act_app_variant(GSimpleAction *action, GVariant *value, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    const char *v = g_variant_get_string(value, nullptr);
    auto variant = parse_app_variant(v);
    if (!variant) return;
    g_simple_action_set_state(action, value);
    set_app_variant(*variant);
    self->options_.app.reset(); // the menu choice now outranks the command line
    self->settings_.app = v;
    self->save_settings_now();
}

void AppWindow::act_terminal_theme(GSimpleAction *action, GVariant *value, gpointer data) {
    auto *self = static_cast<AppWindow *>(data);
    const char *name = g_variant_get_string(value, nullptr);
    if (self->themes_.find(name) == nullptr) return;
    g_simple_action_set_state(action, value);
    self->options_.theme.reset();
    self->settings_.terminal_theme = name;
    self->save_settings_now();
    // Tabs whose session names its own theme keep it.
    self->reapply_default_theme();
}

} // namespace tmssh
