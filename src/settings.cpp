// src/settings.cpp -- see settings.h.
#include "settings.h"

#include <yaml-cpp/yaml.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace tmssh {

namespace {

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Writes text to path through a temp file and a rename, mode 0600.
std::string atomic_write(const std::string &path, const std::string &text) {
    fs::path target(path);
    fs::path dir = target.parent_path();
    if (dir.empty()) dir = ".";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return "create " + dir.string() + ": " + ec.message();

    std::string tmpl = (dir / ".settings-XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemp(buf.data());
    if (fd < 0) return "create temp file in " + dir.string() + ": " + std::strerror(errno);
    const std::string tmp(buf.data());
    auto fail = [&](const std::string &what) {
        int saved = errno;
        ::close(fd);
        ::unlink(tmp.c_str());
        return what + ": " + std::strerror(saved);
    };
    if (::fchmod(fd, 0600) != 0) return fail("chmod " + tmp);
    const char *p = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return fail("write " + tmp);
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) return fail("fsync " + tmp);
    if (::close(fd) != 0) {
        int saved = errno;
        ::unlink(tmp.c_str());
        return "close " + tmp + ": " + std::strerror(saved);
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        int saved = errno;
        ::unlink(tmp.c_str());
        return "rename to " + path + ": " + std::strerror(saved);
    }
    return {};
}

} // namespace

SettingsLoad parse_settings(const std::string &yaml) {
    SettingsLoad out;
    if (trim(yaml).empty()) return out;

    YAML::Node root;
    try {
        root = YAML::Load(yaml);
    } catch (const YAML::Exception &e) {
        out.warnings.push_back(std::string("settings unreadable, using defaults: ") + e.what());
        return out;
    }
    if (!root.IsMap()) {
        out.warnings.push_back("settings is not a mapping, using defaults");
        return out;
    }

    Settings &s = out.settings;
    auto str = [&](const char *key, std::string &dst) {
        YAML::Node v = root[key];
        if (!v || v.IsNull()) return;
        if (!v.IsScalar() || trim(v.Scalar()).empty()) {
            out.warnings.push_back(std::string(key) + ": not a string, ignored");
            return;
        }
        dst = trim(v.Scalar());
    };
    auto num = [&](const char *key, int &dst, int lo, int hi) {
        YAML::Node v = root[key];
        if (!v || v.IsNull()) return;
        char *end = nullptr;
        std::string t = v.IsScalar() ? trim(v.Scalar()) : std::string{};
        errno = 0;
        long n = t.empty() ? 0 : std::strtol(t.c_str(), &end, 10);
        if (t.empty() || errno != 0 || !end || *end != '\0' || n < lo || n > hi) {
            out.warnings.push_back(std::string(key) + ": must be " + std::to_string(lo) + "-" +
                                   std::to_string(hi) + ", ignored");
            return;
        }
        dst = static_cast<int>(n);
    };

    std::string app = s.app;
    str("app", app);
    if (app == "dark" || app == "light")
        s.app = app;
    else
        out.warnings.push_back("app: must be dark or light, ignored");

    str("terminal_theme", s.terminal_theme);
    str("font", s.font);
    num("scrollback_lines", s.scrollback_lines, 0, 1000000);
    num("window_width", s.window_width, 200, 20000);
    num("window_height", s.window_height, 150, 20000);
    num("sidebar_width", s.sidebar_width, 0, 5000);
    return out;
}

SettingsLoad load_settings(const std::string &path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return SettingsLoad{};
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_settings(ss.str());
}

std::string emit_settings(const Settings &s) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "app" << YAML::Value << s.app;
    out << YAML::Key << "terminal_theme" << YAML::Value << YAML::DoubleQuoted << s.terminal_theme;
    out << YAML::Key << "font" << YAML::Value << YAML::DoubleQuoted << s.font;
    out << YAML::Key << "scrollback_lines" << YAML::Value << s.scrollback_lines;
    out << YAML::Key << "window_width" << YAML::Value << s.window_width;
    out << YAML::Key << "window_height" << YAML::Value << s.window_height;
    out << YAML::Key << "sidebar_width" << YAML::Value << s.sidebar_width;
    out << YAML::EndMap;
    return std::string(out.c_str()) + "\n";
}

std::string save_settings(const std::string &path, const Settings &s) {
    return atomic_write(path, emit_settings(s));
}

std::string default_settings_path() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg == '/') return (fs::path(xdg) / "tmssh" / "settings.yaml").string();
    const char *home = std::getenv("HOME");
    fs::path base = (home && *home) ? fs::path(home) : fs::path(".");
    return (base / ".config" / "tmssh" / "settings.yaml").string();
}

} // namespace tmssh
