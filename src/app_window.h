// src/app_window.h -- the main window: session tree on the left, tabs on the
// right, everything else in the header bar menu.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "sessions.h"
#include "settings.h"
#include "ssh_command.h"
#include "theme_apply.h"
#include "themes.h"

namespace tmssh {

// What the command line asked for. Overrides apply to this run only; menu
// changes made during the run are what gets saved.
struct LaunchOptions {
    std::optional<std::string> theme;
    std::optional<AppVariant> app;
    std::optional<std::string> font;
    std::string sessions_path; // empty: default_sessions_path()
    std::string settings_path; // empty: default_settings_path()
};

class AppWindow {
public:
    AppWindow(GtkApplication *app, ThemeSet themes, LaunchOptions options);
    ~AppWindow();

    AppWindow(const AppWindow &) = delete;
    AppWindow &operator=(const AppWindow &) = delete;

    void present();

    // Opens a tab for a session in the tree. Public for the tests' sake as
    // much as the tree's.
    void open_session(const std::string &folder, const std::string &name);
    void open_local();
    // A tab running a local program, e.g. an editor on the session file.
    void open_program(const std::string &title, const std::string &program,
                      const std::vector<std::string> &args);

    struct Tab; // per-tab state, defined in the .cpp

private:
    // --- building
    void build_ui();
    void install_actions();
    GMenuModel *build_menu();
    GtkWidget *build_sidebar();

    // --- session tree
    // capture=false when the caller already captured and then adjusted
    // expanded_ (an import expanding the folders it added).
    void rebuild_tree(bool capture = true);
    void capture_expanded();
    bool session_matches(const Session &s) const;
    // folder and session label of the selected row; session empty for a folder
    bool selected(std::string &folder, std::string &session) const;
    void activate_row(guint position);
    void show_row_menu(GtkWidget *row_widget, double x, double y);

    // --- tabs
    Tab *add_tab(const std::string &title, const std::string &tooltip);
    Tab *current_tab();
    void start_tab(Tab *tab);
    void close_tab(Tab *tab);
    void apply_appearance(Tab *tab);
    void apply_font(Tab *tab);
    void reapply_default_theme();
    void show_terminal_menu(Tab *tab, double x, double y);

    // --- font zoom: one step is a point, applied to every tab and kept
    void zoom_by(int steps);
    void zoom_reset();
    void for_each_tab(const std::function<void(Tab *)> &fn);

    // --- session logs
    bool start_log(Tab *tab);
    void stop_log(Tab *tab, const std::string &why);
    void show_log_state(Tab *tab);
    // The "log" toggle follows the tab on show; null for none.
    void sync_log_action(Tab *tab);

    // --- persistence
    void save_sessions();
    void write_seed();
    void edit_sessions_file();
    void watch_sessions_file();
    void reload_sessions();
    void show_banner(const std::string &text); // empty hides it
    void save_settings_now();
    void save_settings_soon(); // coalesces a burst of zoom steps into one write
    void offer_first_run_import();
    void import_from(const std::string &path);
    void confirm(const std::string &text, const std::string &detail, const std::string &ok_label,
                 std::function<void()> on_ok);
    void message(const std::string &text, const std::string &detail);

    const SshAlgorithms &algorithms();

    // --- signal and action trampolines
    static GListModel *create_child_model(gpointer item, gpointer self);
    static void on_factory_setup(GtkSignalListItemFactory *, GtkListItem *item, gpointer self);
    static void on_factory_bind(GtkSignalListItemFactory *, GtkListItem *item, gpointer self);
    static void on_factory_unbind(GtkSignalListItemFactory *, GtkListItem *item, gpointer self);
    static void on_row_activated(GtkListView *, guint position, gpointer self);
    static void on_row_pressed(GtkGestureClick *g, int n, double x, double y, gpointer item);
    static void on_filter_changed(GtkSearchEntry *entry, gpointer self);
    static void on_switch_page(GtkNotebook *, GtkWidget *page, guint, gpointer self);
    static gboolean on_close_request(GtkWindow *, gpointer self);
    static void on_terminal_pressed(GtkGestureClick *g, int n, double x, double y, gpointer tab);
    static gboolean on_zoom_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer tab);

    static void act_local(GSimpleAction *, GVariant *, gpointer self);
    static void act_connect(GSimpleAction *, GVariant *, gpointer self);
    static void act_delete(GSimpleAction *, GVariant *, gpointer self);
    static void act_reconnect(GSimpleAction *, GVariant *, gpointer self);
    static void act_close_tab(GSimpleAction *, GVariant *, gpointer self);
    static void act_next_tab(GSimpleAction *, GVariant *, gpointer self);
    static void act_prev_tab(GSimpleAction *, GVariant *, gpointer self);
    static void act_find(GSimpleAction *, GVariant *, gpointer self);
    static void act_copy(GSimpleAction *, GVariant *, gpointer self);
    static void act_paste(GSimpleAction *, GVariant *, gpointer self);
    static void act_select_all(GSimpleAction *, GVariant *, gpointer self);
    static void act_zoom_in(GSimpleAction *, GVariant *, gpointer self);
    static void act_zoom_out(GSimpleAction *, GVariant *, gpointer self);
    static void act_zoom_reset(GSimpleAction *, GVariant *, gpointer self);
    static void act_log(GSimpleAction *, GVariant *, gpointer self);
    static void act_auto_log(GSimpleAction *, GVariant *, gpointer self);
    static void act_open_log_folder(GSimpleAction *, GVariant *, gpointer self);
    static void act_import_pathfinder(GSimpleAction *, GVariant *, gpointer self);
    static void act_import_file(GSimpleAction *, GVariant *, gpointer self);
    static void act_edit_sessions(GSimpleAction *, GVariant *, gpointer self);
    static void on_sessions_changed(GFileMonitor *, GFile *file, GFile *other,
                                    GFileMonitorEvent event, gpointer self);
    static void act_about(GSimpleAction *, GVariant *, gpointer self);
    static void act_quit(GSimpleAction *, GVariant *, gpointer self);
    static void act_app_variant(GSimpleAction *, GVariant *, gpointer self);
    static void act_terminal_theme(GSimpleAction *, GVariant *, gpointer self);

    GtkApplication *app_;
    ThemeSet themes_;
    LaunchOptions options_;

    std::string sessions_path_;
    std::string settings_path_;
    Tree tree_;
    Settings settings_;
    std::optional<SshAlgorithms> algorithms_;

    GtkWidget *window_ = nullptr;
    GtkWidget *paned_ = nullptr;
    GtkWidget *filter_ = nullptr;
    GtkWidget *banner_ = nullptr; // sidebar message: a session file that will not parse
    GFileMonitor *monitor_ = nullptr;
    guint reload_timer_ = 0;
    guint save_timer_ = 0;
    double zoom_accum_ = 0.0; // Ctrl+scroll travel not yet a whole step
    // What tmssh last wrote or loaded, so its own saves are not reloaded.
    std::string last_text_;
    GtkWidget *list_view_ = nullptr;
    GtkWidget *notebook_ = nullptr;
    GListStore *root_ = nullptr;
    GtkTreeListModel *tree_model_ = nullptr;
    GtkSingleSelection *selection_ = nullptr;

    std::string filter_text_; // lower-cased
    std::set<std::string> expanded_; // lower-cased folder names, unfiltered view
    bool first_build_ = true;
};

} // namespace tmssh
