// src/pty.h
#pragma once

#include <string>
#include <vector>

namespace gtkpyte {

// A child process on the far end of a pseudo-terminal.
//
// Deliberately free of GTK: this is process and file-descriptor work, and
// keeping it separate means it can be tested without a widget, an event loop
// or a display. PtySession owns one of these and watches fd() on the GLib main loop.
//
// Reads are non-blocking. read() returning 0 means the child closed the
// terminal -- it has exited or is about to -- and is not the same as -1 with
// EAGAIN, which just means nothing is waiting.
class Pty {
public:
    Pty();
    ~Pty();

    Pty(const Pty &) = delete;
    Pty &operator=(const Pty &) = delete;

    // Launch `program` under a new pty sized cols x rows. Extra environment
    // entries are applied in the child ("TERM=xterm-256color" is set for you
    // unless env overrides it). Returns false and fills error() on failure.
    bool start(const std::string &program,
               const std::vector<std::string> &args,
               int cols,
               int rows,
               const std::vector<std::string> &env = {});

    // The master side. Watch it for readability; -1 before start().
    int fd() const { return fd_; }
    bool running() const;
    long pid() const { return static_cast<long>(pid_); }
    const std::string &error() const { return error_; }

    // Non-blocking. Returns bytes read, 0 at end of file, -1 on error or when
    // nothing is available (errno EAGAIN).
    long read(char *buf, unsigned long len);
    // Non-blocking: writes what the kernel accepts and returns that count,
    // which is less than `len` when the child's input queue is full. The
    // caller owns the remainder.
    long write(const char *buf, unsigned long len);
    long write(const std::string &data);

    // Tell the child its window changed (TIOCSWINSZ + SIGWINCH). Applications
    // that do not repaint after a resize are usually missing this, not
    // missing a redraw.
    void resize(int cols, int rows);

    // SIGHUP, then SIGKILL if it is still there. Safe to call twice.
    void terminate();

    // Reap the child if it has exited. Returns true when a status was
    // collected, and writes it to `status` if non-null.
    bool reap(int *status = nullptr);

private:
    void close_fd();

    int fd_ = -1;
    long long pid_ = -1;
    bool exited_ = false;
    int exit_status_ = 0;
    std::string error_;
};

}  // namespace gtkpyte
