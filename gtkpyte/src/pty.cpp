// src/pty.cpp
//
// POSIX pseudo-terminal via forkpty. Linux is the target; the macOS branch on
// the include costs nothing and keeps the file honest about what it needs.
#include "pty.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace gtkpyte {
namespace {

// Build the argv a child needs: program, then args, then a null terminator.
// The strings outlive the call because the caller keeps them alive across the
// fork, and after execvp nothing in this process reads them again.
std::vector<char *> build_argv(const std::string &program,
                               const std::vector<std::string> &args) {
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(program.c_str()));
    for (const std::string &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

winsize make_winsize(int cols, int rows) {
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
    ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);
    return ws;
}

}  // namespace

Pty::Pty() = default;

Pty::~Pty() {
    terminate();
    close_fd();
}

void Pty::close_fd() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Pty::start(const std::string &program,
                const std::vector<std::string> &args,
                int cols,
                int rows,
                const std::vector<std::string> &env) {
    if (fd_ >= 0) {
        error_ = "already started";
        return false;
    }

    winsize ws = make_winsize(cols, rows);

    // Sane line discipline for the child. Left at the kernel default this is
    // already close, but being explicit means a caller that inherited an odd
    // terminal does not hand its oddness to the shell.
    termios tio{};
    tio.c_iflag = ICRNL | IXON | IUTF8;
    tio.c_oflag = OPOST | ONLCR;
    tio.c_cflag = CS8 | CREAD | CLOCAL;
    tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    tio.c_cc[VINTR] = 3;    // ^C
    tio.c_cc[VQUIT] = 28;   // ^\ .
    tio.c_cc[VERASE] = 127; // DEL, which is what we send for Backspace
    tio.c_cc[VKILL] = 21;   // ^U
    tio.c_cc[VEOF] = 4;     // ^D
    tio.c_cc[VSUSP] = 26;   // ^Z
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

    std::vector<char *> argv = build_argv(program, args);

    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, &tio, &ws);
    if (pid < 0) {
        error_ = std::string("forkpty: ") + std::strerror(errno);
        return false;
    }

    if (pid == 0) {
        // Child. Nothing here may return -- on any failure the child must
        // _exit, or a second copy of the host application keeps running with
        // a pty attached to it.
        ::setenv("TERM", "xterm-256color", 1);
        // Without COLORTERM an application that CAN send direct colour will
        // often decide not to, and fall back to a 256-colour approximation or
        // to no colour at all. TERM alone does not advertise it.
        ::setenv("COLORTERM", "truecolor", 1);
        for (const std::string &entry : env) {
            const std::size_t eq = entry.find('=');
            if (eq != std::string::npos) {
                ::setenv(entry.substr(0, eq).c_str(),
                         entry.substr(eq + 1).c_str(), 1);
            }
        }
        // A terminal emulator has no business advertising a line count that
        // came from whatever launched the host application.
        ::unsetenv("LINES");
        ::unsetenv("COLUMNS");

        ::execvp(program.c_str(), argv.data());
        ::_exit(127);  // execvp only returns on failure
    }

    // Parent.
    const int flags = ::fcntl(master, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(master, F_SETFL, flags | O_NONBLOCK);
    }
    ::fcntl(master, F_SETFD, FD_CLOEXEC);

    fd_ = master;
    pid_ = pid;
    exited_ = false;
    error_.clear();
    return true;
}

bool Pty::running() const { return pid_ > 0 && !exited_; }

long Pty::read(char *buf, unsigned long len) {
    if (fd_ < 0) {
        return -1;
    }
    for (;;) {
        const ssize_t n = ::read(fd_, buf, static_cast<size_t>(len));
        if (n >= 0) {
            return static_cast<long>(n);
        }
        if (errno == EINTR) {
            continue;  // a signal landed mid-call; that is not an error
        }
        // EIO on a pty master means the child closed the slave. Every other
        // caller in this codebase treats 0 as "child gone", so normalise it
        // here rather than making each one know the platform quirk.
        if (errno == EIO) {
            return 0;
        }
        return -1;
    }
}

long Pty::write(const char *buf, unsigned long len) {
    if (fd_ < 0) {
        return -1;
    }
    unsigned long written = 0;
    while (written < len) {
        const ssize_t n =
            ::write(fd_, buf + written, static_cast<size_t>(len - written));
        if (n > 0) {
            written += static_cast<unsigned long>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // The child is not draining its input right now. Return what
            // was written; the caller queues the rest (PtySession does) --
            // dropping it here is how a large paste loses its tail.
            break;
        }
        break;
    }
    return static_cast<long>(written);
}

long Pty::write(const std::string &data) {
    return write(data.data(), static_cast<unsigned long>(data.size()));
}

void Pty::resize(int cols, int rows) {
    if (fd_ < 0) {
        return;
    }
    winsize ws = make_winsize(cols, rows);
    ::ioctl(fd_, TIOCSWINSZ, &ws);
    // forkpty made the child a session leader on this terminal, so the kernel
    // delivers SIGWINCH to its foreground group for us.
}

void Pty::terminate() {
    if (pid_ <= 0 || exited_) {
        return;
    }
    const pid_t pid = static_cast<pid_t>(pid_);
    ::kill(pid, SIGHUP);
    for (int i = 0; i < 20; ++i) {  // ~200ms for a well-behaved shell to go
        if (reap()) {
            return;
        }
        ::usleep(10000);
    }
    ::kill(pid, SIGKILL);
    reap();
}

bool Pty::reap(int *status) {
    if (pid_ <= 0) {
        return false;
    }
    if (exited_) {
        if (status != nullptr) {
            *status = exit_status_;
        }
        return true;
    }
    int st = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &st, WNOHANG);
    if (r != static_cast<pid_t>(pid_)) {
        return false;
    }
    exited_ = true;
    exit_status_ = st;
    if (status != nullptr) {
        *status = st;
    }
    return true;
}

}  // namespace gtkpyte
