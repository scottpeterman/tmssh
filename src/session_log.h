// src/session_log.h -- session logs: a terminal byte stream as plain text.
//
// The log is what the session printed, as text a person would read or grep:
// colours, titles and mode switches are dropped, and the line editing a
// terminal does in place -- carriage return, backspace, erase-line, cursor
// left and right -- is applied before a line is written. That is what keeps
// a pager's "--More--" and readline's redraws out of the log, and what makes
// a progress counter end up as its last value. Full-screen programs (vi, top)
// address the whole screen and do not reduce to lines; their logs are noisy.
//
// Only output is logged. What is typed reaches the log through the remote
// end's echo, so a password typed at a prompt that does not echo never does.
#pragma once

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

namespace tmssh {

// Stream to lines. Feed it bytes as they arrive, split anywhere -- escape
// sequences and UTF-8 may straddle chunks.
class LogCleaner {
public:
    // Complete lines produced by these bytes, each ending in '\n', trailing
    // spaces trimmed. Empty when no line finished.
    std::string feed(const char *data, std::size_t len);

    // The line still being written (a prompt, typically), trimmed and without
    // a newline; empty if there is none. Clears it.
    std::string flush();

private:
    enum class State { Ground, Esc, EscIntermediate, Csi, Osc, OscEsc, Str, StrEsc };

    void byte(unsigned char b, std::string &out);
    void control(unsigned char b, std::string &out);
    void put(char32_t ch, std::string &out);
    void csi_dispatch(char final, std::string &out);
    void end_line(std::string &out);
    void utf8_abort(std::string &out);

    State state_ = State::Ground;
    std::u32string line_;
    std::size_t col_ = 0;

    std::string params_;  // CSI parameter bytes, as received
    bool csi_private_ = false;

    char32_t cp_ = 0;     // UTF-8 code point being assembled
    int cp_need_ = 0;     // continuation bytes still to come
};

// One log file. Opened exclusively, mode 0600 in a 0700 directory: a session
// log holds whatever the device printed, configuration included.
class SessionLog {
public:
    SessionLog() = default;
    ~SessionLog(); // closes, noting the time
    SessionLog(const SessionLog &) = delete;
    SessionLog &operator=(const SessionLog &) = delete;

    // Creates <dir>/<name>_<YYYYmmdd-HHMMSS>.log, adding -2, -3, ... rather
    // than ever appending to a file that exists. Writes a header naming the
    // session, the target (when not empty) and the start time.
    bool create(const std::string &dir, const std::string &name, const std::string &target,
                std::time_t when);

    // Session output, cleaned.
    bool write(const char *data, std::size_t len);

    // A "# tmssh: ..." line, after any unfinished line is written out.
    bool note(const std::string &text);

    // Notes `footer` (when not empty) and closes. Safe to call twice.
    void close(const std::string &footer);

    bool is_open() const { return fd_ >= 0; }
    const std::string &path() const { return path_; }
    const std::string &error() const { return error_; } // the first failure

private:
    bool put(const std::string &text);

    int fd_ = -1;
    std::string path_;
    std::string error_;
    LogCleaner cleaner_;
};

// "spine-1 (lab)" -> "spine-1_lab": letters, digits, '.', '-' and '_' kept,
// runs of anything else become one '_'. Never empty.
std::string log_name_part(const std::string &label);

// <name>_<YYYYmmdd-HHMMSS>.log in local time.
std::string log_file_name(const std::string &label, std::time_t when);

// "2026-10-12 01:40:00 -0600"
std::string log_timestamp(std::time_t when);

// A leading "~" or "~/" becomes $HOME; empty becomes the default, ~/tmssh-logs.
std::string expand_log_dir(const std::string &dir);

} // namespace tmssh
