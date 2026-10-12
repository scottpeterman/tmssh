// src/session_log.cpp -- see session_log.h.
#include "session_log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace tmssh {

namespace {

// A line longer than this is written out in pieces, so output that never
// sends a newline cannot grow without bound.
constexpr std::size_t kMaxLine = 16384;
constexpr char32_t kReplacement = 0xFFFD;

void append_utf8(std::string &out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string trimmed_utf8(const std::u32string &line) {
    std::size_t n = line.size();
    while (n > 0 && line[n - 1] == U' ') --n;
    std::string out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) append_utf8(out, line[i]);
    return out;
}

// CSI parameters: "1;32" -> {1, 32}; empty fields are 0.
std::vector<int> parse_params(const std::string &s) {
    std::vector<int> out;
    int cur = 0;
    bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            cur = std::min(cur * 10 + (c - '0'), 100000);
            any = true;
        } else if (c == ';' || c == ':') {
            out.push_back(cur);
            cur = 0;
            any = false;
        }
    }
    if (any || !out.empty()) out.push_back(cur);
    return out;
}

int param(const std::vector<int> &p, std::size_t i, int dflt) {
    return (i < p.size() && p[i] != 0) ? p[i] : dflt;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// LogCleaner
// ─────────────────────────────────────────────────────────────────────

std::string LogCleaner::feed(const char *data, std::size_t len) {
    std::string out;
    for (std::size_t i = 0; i < len; ++i) byte(static_cast<unsigned char>(data[i]), out);
    return out;
}

std::string LogCleaner::flush() {
    std::string out = trimmed_utf8(line_);
    line_.clear();
    col_ = 0;
    return out;
}

void LogCleaner::end_line(std::string &out) {
    out += trimmed_utf8(line_);
    out += '\n';
    line_.clear();
    col_ = 0;
}

void LogCleaner::utf8_abort(std::string &out) {
    if (cp_need_ == 0) return;
    cp_need_ = 0;
    put(kReplacement, out);
}

void LogCleaner::put(char32_t ch, std::string &out) {
    if (col_ >= kMaxLine) {
        end_line(out);
    }
    if (col_ > line_.size()) line_.resize(col_, U' ');
    if (col_ == line_.size())
        line_.push_back(ch);
    else
        line_[col_] = ch;
    ++col_;
}

void LogCleaner::control(unsigned char b, std::string &out) {
    switch (b) {
    case '\n':
    case 0x0B: // VT
    case 0x0C: // FF
        end_line(out);
        break;
    case '\r':
        col_ = 0;
        break;
    case '\b':
        if (col_ > 0) --col_;
        break;
    case '\t':
        col_ = (col_ / 8 + 1) * 8;
        break;
    default:
        break; // BEL, SO/SI and the rest leave no text
    }
}

void LogCleaner::csi_dispatch(char final, std::string &out) {
    if (csi_private_) return; // DEC private modes and the like: no text effect
    const std::vector<int> p = parse_params(params_);
    const std::size_t n = static_cast<std::size_t>(param(p, 0, 1));
    switch (final) {
    case 'K': { // erase in line
        const int mode = p.empty() ? 0 : p[0];
        if (mode == 0) {
            if (col_ < line_.size()) line_.resize(col_);
        } else if (mode == 1) {
            const std::size_t end = std::min(col_ + 1, line_.size());
            std::fill(line_.begin(), line_.begin() + static_cast<std::ptrdiff_t>(end), U' ');
        } else if (mode == 2) {
            line_.clear(); // the column stays; put() pads up to it
        }
        break;
    }
    case 'C': // cursor forward
    case 'a':
        col_ = std::min(col_ + n, kMaxLine);
        break;
    case 'D': // cursor back
        col_ -= std::min(col_, n);
        break;
    case 'G': // cursor to column
    case '`':
        col_ = std::min(n - 1, kMaxLine);
        break;
    case 'P': // delete characters
        if (col_ < line_.size()) line_.erase(col_, std::min(n, line_.size() - col_));
        break;
    case '@': // insert blanks
        if (col_ < line_.size()) line_.insert(col_, std::min(n, kMaxLine), U' ');
        break;
    case 'X': // erase characters
        for (std::size_t i = col_; i < line_.size() && i < col_ + n; ++i) line_[i] = U' ';
        break;
    case 'H': // cursor position, and
    case 'f':
    case 'J': // erase in display: a new screen, so whatever was on the line is done
        if (!line_.empty()) end_line(out);
        col_ = (final == 'J') ? 0 : static_cast<std::size_t>(param(p, 1, 1) - 1);
        break;
    default:
        break; // colours, scrolling regions, vertical movement: nothing to log
    }
}

void LogCleaner::byte(unsigned char b, std::string &out) {
    // CAN and SUB abort any sequence in progress.
    if ((b == 0x18 || b == 0x1A) && state_ != State::Ground) {
        state_ = State::Ground;
        return;
    }

    switch (state_) {
    case State::Ground:
        if (cp_need_ > 0) {
            if ((b & 0xC0) == 0x80) {
                cp_ = (cp_ << 6) | (b & 0x3F);
                if (--cp_need_ == 0) put(cp_, out);
                return;
            }
            utf8_abort(out); // and handle this byte afresh
        }
        if (b == 0x1B) {
            state_ = State::Esc;
        } else if (b < 0x20) {
            control(b, out);
        } else if (b < 0x7F) {
            put(b, out);
        } else if (b == 0x7F) {
            // DEL: no text
        } else if ((b & 0xE0) == 0xC0) {
            cp_ = b & 0x1F;
            cp_need_ = 1;
        } else if ((b & 0xF0) == 0xE0) {
            cp_ = b & 0x0F;
            cp_need_ = 2;
        } else if ((b & 0xF8) == 0xF0) {
            cp_ = b & 0x07;
            cp_need_ = 3;
        } else {
            put(kReplacement, out);
        }
        return;

    case State::Esc:
        if (b == '[') {
            params_.clear();
            csi_private_ = false;
            state_ = State::Csi;
        } else if (b == ']') {
            state_ = State::Osc;
        } else if (b == 'P' || b == 'X' || b == '^' || b == '_') {
            state_ = State::Str; // DCS, SOS, PM, APC
        } else if (b >= 0x20 && b <= 0x2F) {
            state_ = State::EscIntermediate; // charset designation, ESC # 8, ...
        } else if (b == 'E') {
            end_line(out); // NEL
            state_ = State::Ground;
        } else if (b == 0x1B) {
            // stays in Esc
        } else {
            state_ = State::Ground; // ESC 7, ESC 8, ESC =, ESC M, ...
        }
        return;

    case State::EscIntermediate:
        if (b >= 0x30 && b <= 0x7E) state_ = State::Ground;
        else if (b == 0x1B) state_ = State::Esc;
        return;

    case State::Csi:
        if (b >= 0x40 && b <= 0x7E) {
            state_ = State::Ground;
            csi_dispatch(static_cast<char>(b), out);
        } else if (b >= 0x30 && b <= 0x3F) {
            if (b == '?' || b == '>' || b == '<' || b == '=') csi_private_ = true;
            if (params_.size() < 64) params_ += static_cast<char>(b);
        } else if (b >= 0x20 && b <= 0x2F) {
            csi_private_ = true; // intermediates: nothing handled here uses them
        } else if (b == 0x1B) {
            state_ = State::Esc;
        } else if (b < 0x20) {
            control(b, out); // C0 inside a CSI still acts, as on a VT
        }
        return;

    case State::Osc:
        if (b == 0x07) state_ = State::Ground;
        else if (b == 0x1B) state_ = State::OscEsc;
        return;

    case State::OscEsc:
        state_ = (b == '\\') ? State::Ground : State::Osc;
        return;

    case State::Str:
        if (b == 0x1B) state_ = State::StrEsc;
        else if (b == 0x07) state_ = State::Ground;
        return;

    case State::StrEsc:
        state_ = (b == '\\') ? State::Ground : State::Str;
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────
// SessionLog
// ─────────────────────────────────────────────────────────────────────

SessionLog::~SessionLog() {
    close("log closed " + log_timestamp(std::time(nullptr)));
}

bool SessionLog::create(const std::string &dir, const std::string &name,
                        const std::string &target, std::time_t when) {
    close("");
    error_.clear();

    std::error_code ec;
    const bool existed = fs::is_directory(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        error_ = "create " + dir + ": " + ec.message();
        return false;
    }
    // Only a directory tmssh just made is tightened; one the user already
    // has keeps the permissions they gave it.
    if (!existed) ::chmod(dir.c_str(), 0700);

    const std::string base = log_file_name(name, when);
    const std::string stem = base.substr(0, base.size() - 4); // drop ".log"
    for (int i = 1; i < 1000; ++i) {
        const std::string file = (i == 1) ? base : stem + "-" + std::to_string(i) + ".log";
        const std::string path = (fs::path(dir) / file).string();
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
        if (fd >= 0) {
            fd_ = fd;
            path_ = path;
            break;
        }
        if (errno != EEXIST) {
            error_ = "create " + path + ": " + std::strerror(errno);
            return false;
        }
    }
    if (fd_ < 0) {
        error_ = "no free log file name for " + base + " in " + dir;
        return false;
    }

    std::string header = "# tmssh session log: " + name + "\n";
    if (!target.empty()) header += "# target: " + target + "\n";
    header += "# started " + log_timestamp(when) + "\n";
    return put(header);
}

bool SessionLog::put(const std::string &text) {
    if (fd_ < 0) return false;
    const char *p = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        const ssize_t n = ::write(fd_, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (error_.empty()) error_ = "write " + path_ + ": " + std::strerror(errno);
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

bool SessionLog::write(const char *data, std::size_t len) {
    if (fd_ < 0) return false;
    const std::string text = cleaner_.feed(data, len);
    return text.empty() || put(text);
}

bool SessionLog::note(const std::string &text) {
    if (fd_ < 0) return false;
    std::string out;
    const std::string pending = cleaner_.flush();
    if (!pending.empty()) out += pending + "\n";
    out += "# tmssh: " + text + "\n";
    return put(out);
}

void SessionLog::close(const std::string &footer) {
    if (fd_ < 0) return;
    if (!footer.empty()) note(footer);
    else if (std::string pending = cleaner_.flush(); !pending.empty()) put(pending + "\n");
    ::close(fd_);
    fd_ = -1;
}

// ─────────────────────────────────────────────────────────────────────
// Names
// ─────────────────────────────────────────────────────────────────────

std::string log_name_part(const std::string &label) {
    std::string out;
    bool gap = false;
    for (unsigned char c : label) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '.' || c == '-' || c == '_';
        if (keep) {
            if (gap && !out.empty()) out += '_';
            gap = false;
            out += static_cast<char>(c);
        } else {
            gap = true;
        }
    }
    // A leading dot would hide the file.
    while (!out.empty() && out.front() == '.') out.erase(0, 1);
    return out.empty() ? std::string("session") : out;
}

std::string log_file_name(const std::string &label, std::time_t when) {
    std::tm tm{};
    localtime_r(&when, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    return log_name_part(label) + "_" + stamp + ".log";
}

std::string log_timestamp(std::time_t when) {
    std::tm tm{};
    localtime_r(&when, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %z", &tm);
    return buf;
}

std::string expand_log_dir(const std::string &dir) {
    const char *home_env = std::getenv("HOME");
    const std::string home = (home_env && *home_env) ? home_env : ".";
    if (dir.empty()) return (fs::path(home) / "tmssh-logs").string();
    if (dir == "~") return home;
    if (dir.rfind("~/", 0) == 0) return (fs::path(home) / dir.substr(2)).string();
    return dir;
}

} // namespace tmssh
