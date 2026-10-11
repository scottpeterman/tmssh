// src/ptysession.h
#pragma once

#include <glib.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "gtkpyte-terminal.h"
#include "pty.h"

namespace gtkpyte {

// Wires a local child process to a GtkpyteTerminal: pty output into
// gtkpyte_terminal_feed(), "commit" into the pty, "resized" into TIOCSWINSZ.
//
// The pty master is watched with g_unix_fd_add on the default main context,
// so everything here runs on the GTK thread and the widget is never touched
// from anywhere else.
class PtySession {
public:
    using ExitHandler = std::function<void(int wait_status)>;

    explicit PtySession(GtkpyteTerminal *terminal);
    ~PtySession();

    PtySession(const PtySession &) = delete;
    PtySession &operator=(const PtySession &) = delete;

    // Start `program` (searched on PATH) at the terminal's current grid size.
    bool start(const std::string &program, const std::vector<std::string> &args,
               const std::vector<std::string> &env = {});

    // Called once, after the child has gone and its output is drained.
    void set_exit_handler(ExitHandler handler) { on_exit_ = std::move(handler); }

    const std::string &error() const { return pty_.error(); }
    long pid() const { return pty_.pid(); }

private:
    static gboolean on_readable(gint fd, GIOCondition condition, gpointer data);
    static void on_commit(GtkpyteTerminal *terminal, GBytes *bytes, gpointer data);
    static void on_resized(GtkpyteTerminal *terminal, int cols, int rows, gpointer data);
    static gboolean on_reap_retry(gpointer data);
    static gboolean on_writable(gint fd, GIOCondition condition, gpointer data);

    // Queue bytes for the child and write as much as the kernel takes now.
    // The rest goes out from on_writable as the child reads. Nothing is ever
    // dropped, and ordering holds because every write goes through here --
    // a keystroke typed during a long paste lands after the paste, not inside
    // it.
    void send(const char *data, gsize len);
    // Write queued bytes until done or the kernel pushes back; true when the
    // queue is empty.
    bool drain();

    // Output has hit end of file. The child may still be on its way out, so
    // its status is collected on a short retry before the handler runs.
    void child_closed();
    void finish(int wait_status);

    GtkpyteTerminal *terminal_;
    Pty pty_;
    guint watch_ = 0;
    guint write_watch_ = 0;
    std::string out_;          // bytes accepted but not yet written
    std::size_t out_pos_ = 0;  // how much of out_ has been written
    guint reap_timer_ = 0;
    int reap_tries_ = 0;
    gulong commit_handler_ = 0;
    gulong resized_handler_ = 0;
    bool finished_ = false;
    ExitHandler on_exit_;
};

}  // namespace gtkpyte
