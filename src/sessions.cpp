// src/sessions.cpp -- see sessions.h.
#include "sessions.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <tuple>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace tmssh {

// ─────────────────────────────────────────────────────────────────────
// Small helpers
// ─────────────────────────────────────────────────────────────────────

namespace {

std::string trim(std::string_view s) {
    const char *ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(ws);
    return std::string(s.substr(b, e - b + 1));
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool iequal_trimmed(std::string_view a, std::string_view b) {
    return lower(trim(a)) == lower(trim(b));
}

std::string dq(std::string_view s) { return "\"" + std::string(s) + "\""; }

std::string join_host_port(const std::string &host, int port, int def) {
    if (host.empty()) return {};
    if (port == 0 || port == def) return host;
    return host + ":" + std::to_string(port);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Transport
// ─────────────────────────────────────────────────────────────────────

const char *to_string(Transport t) { return t == Transport::Telnet ? "telnet" : "ssh"; }

std::optional<Transport> parse_transport(std::string_view s) {
    std::string v = lower(trim(s));
    if (v.empty() || v == "ssh") return Transport::Ssh;
    if (v == "telnet") return Transport::Telnet;
    return std::nullopt;
}

int default_port(Transport t) { return t == Transport::Telnet ? 23 : 22; }

// ─────────────────────────────────────────────────────────────────────
// Session
// ─────────────────────────────────────────────────────────────────────

bool operator==(const JumpSpec &a, const JumpSpec &b) {
    return std::tie(a.host, a.port, a.username, a.key_path) ==
           std::tie(b.host, b.port, b.username, b.key_path);
}

bool operator==(const Session &a, const Session &b) {
    return std::tie(a.name, a.transport, a.host, a.port, a.username, a.auth_type, a.key_path,
                    a.jump, a.legacy_algorithms, a.host_key_policy, a.known_hosts,
                    a.connect_timeout_sec, a.telnet_crlf, a.term_type, a.terminal_theme,
                    a.font_size, a.scrollback_lines) ==
           std::tie(b.name, b.transport, b.host, b.port, b.username, b.auth_type, b.key_path,
                    b.jump, b.legacy_algorithms, b.host_key_policy, b.known_hosts,
                    b.connect_timeout_sec, b.telnet_crlf, b.term_type, b.terminal_theme,
                    b.font_size, b.scrollback_lines);
}

Session Session::normalized() const {
    Session n = *this;
    n.name = trim(n.name);
    n.host = trim(n.host);
    n.username = trim(n.username);
    n.auth_type = lower(trim(n.auth_type));
    n.key_path = trim(n.key_path);
    n.host_key_policy = lower(trim(n.host_key_policy));
    n.known_hosts = trim(n.known_hosts);
    n.term_type = trim(n.term_type);
    n.terminal_theme = trim(n.terminal_theme);
    n.jump.host = trim(n.jump.host);
    n.jump.username = trim(n.jump.username);
    n.jump.key_path = trim(n.jump.key_path);

    if (n.port == 0) n.port = default_port(n.transport);

    // Filled regardless of transport: the edit dialog holds both groups, and
    // a hidden ssh group must not show blanks when the selector flips to it.
    if (n.auth_type.empty()) n.auth_type = kAuthAgent;
    if (n.host_key_policy.empty()) n.host_key_policy = kHostKeyTofu;
    if (n.connect_timeout_sec == 0) n.connect_timeout_sec = kDefaultConnectTimeoutSec;
    if (n.jump.in_use() && n.jump.port == 0) n.jump.port = 22;
    if (n.term_type.empty()) n.term_type = kDefaultTermType;

    // font_size, scrollback_lines and terminal_theme stay as given: zero and
    // empty mean "follow the application", and writing a default in would pin
    // this session to it forever.

    if (n.name.empty()) n.name = n.label();
    return n;
}

std::string Session::label() const {
    std::string n = trim(name);
    return n.empty() ? target() : n;
}

std::string Session::target() const {
    std::string h = trim(host);
    std::string hp = join_host_port(h, port, default_port(transport));
    if (transport == Transport::Ssh) {
        std::string u = trim(username);
        if (!u.empty() && !hp.empty()) return u + "@" + hp;
    }
    return hp;
}

std::string Session::key() const {
    std::string h = lower(trim(host));
    if (h.empty()) return {};
    int p = port == 0 ? default_port(transport) : port;
    std::string u = transport == Transport::Ssh ? trim(username) : std::string{};
    return std::string(to_string(transport)) + ":" + (u.empty() ? "" : u + "@") + h + ":" +
           std::to_string(p);
}

std::vector<FieldError> validate(const Session &in) {
    Session s = in.normalized();
    std::vector<FieldError> errs;
    auto add = [&](const char *field, std::string msg) { errs.push_back({field, std::move(msg)}); };

    if (s.host.empty())
        add("host", "required");
    else if (s.host.find_first_of(" \t") != std::string::npos)
        add("host", "contains whitespace");
    if (s.port < 1 || s.port > 65535) add("port", "must be 1-65535");

    if (s.transport == Transport::Ssh) {
        if (s.auth_type == kAuthPublicKey) {
            if (s.key_path.empty()) add("key_path", "required for public-key auth");
        } else if (s.auth_type != kAuthAgent && s.auth_type != kAuthPassword) {
            add("auth_type", "unknown auth type " + dq(s.auth_type));
        }
        // No vault to fall back on. A blank user would make ssh log in as the
        // local account, which on network gear is a confusing failure.
        if (s.username.empty()) add("username", "required");
        if (s.host_key_policy != kHostKeyStrict && s.host_key_policy != kHostKeyTofu &&
            s.host_key_policy != kHostKeyInsecure)
            add("host_key_policy", "unknown policy " + dq(s.host_key_policy));
        if (s.connect_timeout_sec < 1) add("connect_timeout_sec", "must be at least 1");
        if (s.jump.in_use()) {
            if (s.jump.port < 1 || s.jump.port > 65535) add("jump.port", "must be 1-65535");
            if (s.jump.username.empty()) add("jump.username", "required");
        }
    }

    if (s.font_size != 0 && (s.font_size < 6 || s.font_size > 72)) add("font_size", "must be 6-72");
    if (s.scrollback_lines < 0) add("scrollback_lines", "must not be negative");
    return errs;
}

// ─────────────────────────────────────────────────────────────────────
// Tree editing
// ─────────────────────────────────────────────────────────────────────

int Folder::session_index(std::string_view want) const {
    for (std::size_t i = 0; i < sessions.size(); ++i)
        if (iequal_trimmed(sessions[i].label(), want)) return static_cast<int>(i);
    return -1;
}

int Tree::folder_index(std::string_view want) const {
    for (std::size_t i = 0; i < folders.size(); ++i)
        if (iequal_trimmed(folders[i].name, want)) return static_cast<int>(i);
    return -1;
}

std::size_t Tree::session_count() const {
    std::size_t n = 0;
    for (const auto &f : folders) n += f.sessions.size();
    return n;
}

std::string Tree::add_folder(std::string_view name_in) {
    std::string name = trim(name_in);
    if (name.empty()) return "folder name is required";
    if (folder_index(name) >= 0) return "a folder called " + dq(name) + " already exists";
    folders.push_back(Folder{name, {}});
    return {};
}

std::string Tree::rename_folder(std::string_view old_name, std::string_view new_name_in) {
    std::string new_name = trim(new_name_in);
    if (new_name.empty()) return "folder name is required";
    int i = folder_index(old_name);
    if (i < 0) return "no folder called " + dq(old_name);
    int j = folder_index(new_name);
    if (j >= 0 && j != i) return "a folder called " + dq(new_name) + " already exists";
    folders[i].name = new_name;
    return {};
}

std::string Tree::remove_folder(std::string_view name, bool force) {
    int i = folder_index(name);
    if (i < 0) return "no folder called " + dq(name);
    auto n = folders[i].sessions.size();
    if (n > 0 && !force)
        return "folder " + dq(folders[i].name) + " still has " + std::to_string(n) +
               " session(s)";
    folders.erase(folders.begin() + i);
    return {};
}

std::string Tree::add(std::string_view folder, const Session &in) {
    Session s = in.normalized();
    if (s.label().empty()) return "session needs a name or a host";
    int i = folder_index(folder);
    if (i < 0) {
        if (auto err = add_folder(folder); !err.empty()) return err;
        i = folder_index(folder);
    }
    if (folders[i].session_index(s.label()) >= 0)
        return dq(folders[i].name) + " already has a session called " + dq(s.label());
    folders[i].sessions.push_back(std::move(s));
    return {};
}

std::string Tree::replace(std::string_view folder, std::string_view name, const Session &in) {
    int i = folder_index(folder);
    if (i < 0) return "no folder called " + dq(folder);
    int j = folders[i].session_index(name);
    if (j < 0) return "no session called " + dq(name) + " in " + dq(folder);
    Session s = in.normalized();
    if (s.label().empty()) return "session needs a name or a host";
    int k = folders[i].session_index(s.label());
    if (k >= 0 && k != j)
        return dq(folders[i].name) + " already has a session called " + dq(s.label());
    folders[i].sessions[j] = std::move(s);
    return {};
}

std::string Tree::remove(std::string_view folder, std::string_view name) {
    int i = folder_index(folder);
    if (i < 0) return "no folder called " + dq(folder);
    int j = folders[i].session_index(name);
    if (j < 0) return "no session called " + dq(name) + " in " + dq(folder);
    folders[i].sessions.erase(folders[i].sessions.begin() + j);
    return {};
}

std::string Tree::move(std::string_view from_folder, std::string_view name,
                       std::string_view to_folder) {
    int i = folder_index(from_folder);
    if (i < 0) return "no folder called " + dq(from_folder);
    int j = folders[i].session_index(name);
    if (j < 0) return "no session called " + dq(name) + " in " + dq(from_folder);
    if (folder_index(to_folder) == i) return {};
    Session s = folders[i].sessions[j];
    // Add first: if the destination refuses, the session is still where it was.
    if (auto err = add(to_folder, s); !err.empty()) return err;
    return remove(from_folder, name);
}

// ─────────────────────────────────────────────────────────────────────
// Reading
// ─────────────────────────────────────────────────────────────────────

namespace {

// Field readers that never throw. A value of the wrong type becomes a warning
// and the field keeps its default, so one typo does not lose the whole file.
struct Reader {
    std::vector<std::string> &warnings;
    std::string where; // "folder/session", for messages

    std::string str(const YAML::Node &m, const char *key) {
        YAML::Node v = m[key];
        if (!v || v.IsNull()) return {};
        if (!v.IsScalar()) {
            warnings.push_back(where + ": " + key + " is not a scalar, ignored");
            return {};
        }
        return v.Scalar();
    }

    int integer(const YAML::Node &m, const char *key) {
        YAML::Node v = m[key];
        if (!v || v.IsNull()) return 0;
        if (v.IsScalar()) {
            // Accepts quoted numbers too: the older file wrote port as a string.
            const std::string &raw = v.Scalar();
            std::string t = trim(raw);
            if (!t.empty()) {
                char *end = nullptr;
                errno = 0;
                long n = std::strtol(t.c_str(), &end, 10);
                if (errno == 0 && end && *end == '\0' && n >= -2147483647L && n <= 2147483647L)
                    return static_cast<int>(n);
            }
        }
        warnings.push_back(where + ": " + key + " is not a number, ignored");
        return 0;
    }

    std::optional<bool> boolean(const YAML::Node &m, const char *key) {
        YAML::Node v = m[key];
        if (!v || v.IsNull()) return std::nullopt;
        if (v.IsScalar()) {
            std::string t = lower(trim(v.Scalar()));
            if (t == "true" || t == "yes" || t == "on") return true;
            if (t == "false" || t == "no" || t == "off") return false;
        }
        warnings.push_back(where + ": " + key + " is not true/false, ignored");
        return std::nullopt;
    }
};

// Returns nullopt for a session that is skipped (serial, unknown transport,
// not a mapping). skipped_serial is bumped for serial.
std::optional<Session> read_session(const YAML::Node &m, const std::string &folder,
                                    std::size_t index, LoadResult &res) {
    std::string where = folder + "/#" + std::to_string(index + 1);
    if (!m.IsMap()) {
        res.warnings.push_back(where + ": not a mapping, skipped");
        return std::nullopt;
    }
    Reader r{res.warnings, where};
    std::string nm = trim(r.str(m, "name"));
    if (!nm.empty()) r.where = folder + "/" + nm;

    std::string tr = r.str(m, "transport");
    if (lower(trim(tr)) == "serial") {
        ++res.skipped_serial;
        return std::nullopt;
    }
    auto transport = parse_transport(tr);
    if (!transport) {
        res.warnings.push_back(r.where + ": unknown transport " + dq(tr) + ", skipped");
        return std::nullopt;
    }

    Session s;
    s.name = nm;
    s.transport = *transport;
    s.host = r.str(m, "host");
    s.port = r.integer(m, "port");
    s.username = r.str(m, "username");
    s.auth_type = r.str(m, "auth_type");
    s.key_path = r.str(m, "key_path");
    if (YAML::Node j = m["jump"]; j && j.IsMap()) {
        Reader jr{res.warnings, r.where + " jump"};
        s.jump.host = jr.str(j, "host");
        s.jump.port = jr.integer(j, "port");
        s.jump.username = jr.str(j, "username");
        s.jump.key_path = jr.str(j, "key_path");
    }
    s.legacy_algorithms = r.boolean(m, "legacy_algorithms").value_or(false);
    s.host_key_policy = r.str(m, "host_key_policy");
    s.known_hosts = r.str(m, "known_hosts");
    s.connect_timeout_sec = r.integer(m, "connect_timeout_sec");
    s.telnet_crlf = r.boolean(m, "telnet_crlf");
    s.term_type = r.str(m, "term_type");
    s.terminal_theme = r.str(m, "terminal_theme");
    s.font_size = r.integer(m, "font_size");
    s.scrollback_lines = r.integer(m, "scrollback_lines");
    return s.normalized();
}

void read_folders(const YAML::Node &seq, LoadResult &res) {
    for (std::size_t fi = 0; fi < seq.size(); ++fi) {
        const YAML::Node f = seq[fi];
        if (!f.IsMap()) {
            res.warnings.push_back("folder #" + std::to_string(fi + 1) + ": not a mapping, skipped");
            continue;
        }
        Reader r{res.warnings, "folder #" + std::to_string(fi + 1)};
        std::string name = trim(r.str(f, "folder_name"));
        if (name.empty()) name = "Sessions";

        // Two folders differing only in case are one folder to every lookup;
        // merge rather than keep one the tree can never address.
        int at = res.tree.folder_index(name);
        const bool created = at < 0;
        if (at >= 0) {
            res.warnings.push_back("folder " + dq(name) + " appears twice, merged into " +
                                   dq(res.tree.folders[at].name));
        } else {
            res.tree.folders.push_back(Folder{name, {}});
            at = static_cast<int>(res.tree.folders.size()) - 1;
        }

        YAML::Node ss = f["sessions"];
        if (!ss || ss.IsNull()) continue;
        if (!ss.IsSequence()) {
            res.warnings.push_back("folder " + dq(name) + ": sessions is not a list, ignored");
            continue;
        }
        const int serial_before = res.skipped_serial;
        for (std::size_t si = 0; si < ss.size(); ++si) {
            auto s = read_session(ss[si], name, si, res);
            if (!s) continue;
            Folder &dst = res.tree.folders[at];
            if (dst.session_index(s->label()) >= 0) {
                res.warnings.push_back(name + "/" + s->label() + ": duplicate name, skipped");
                continue;
            }
            dst.sessions.push_back(std::move(*s));
        }
        // A folder that held only serial consoles would show up empty; drop it.
        // A folder that was empty in the file stays: somebody made it.
        if (created && res.tree.folders[at].sessions.empty() &&
            res.skipped_serial > serial_before)
            res.tree.folders.pop_back();
    }
}

} // namespace

LoadResult parse_tree(std::string_view text) {
    LoadResult res;
    if (trim(text).empty()) return res;

    YAML::Node root;
    try {
        root = YAML::Load(std::string(text));
    } catch (const YAML::Exception &e) {
        res.error = std::string("parse session file: ") + e.what();
        return res;
    }

    if (root.IsNull()) return res;
    if (root.IsSequence()) {
        read_folders(root, res);
        return res;
    }
    if (!root.IsMap()) {
        res.error = "parse session file: expected a mapping or a list of folders";
        return res;
    }
    YAML::Node folders = root["folders"];
    if (folders && !folders.IsNull()) {
        if (!folders.IsSequence()) {
            res.error = "parse session file: folders is not a list";
            return res;
        }
        read_folders(folders, res);
    }
    return res;
}

LoadResult load_file(const std::string &path) {
    std::error_code ec;
    if (!fs::exists(path, ec) && !ec) return LoadResult{};
    // ifstream opens a directory "successfully" on Linux and reads nothing,
    // which would look like an empty inventory.
    if (fs::is_directory(path, ec)) {
        LoadResult res;
        res.error = "read " + path + ": is a directory";
        return res;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LoadResult res;
        res.error = "read " + path + ": " + std::strerror(errno);
        return res;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
        LoadResult res;
        res.error = "read " + path + ": I/O error";
        return res;
    }
    return parse_tree(ss.str());
}

// ─────────────────────────────────────────────────────────────────────
// Writing
// ─────────────────────────────────────────────────────────────────────

namespace {

// True when a plain scalar would read back as something other than this
// string: empty, null, a bool, a number, or text YAML gives meaning to.
bool needs_quotes(const std::string &s) {
    if (s.empty()) return true;
    std::string l = lower(s);
    static const char *special[] = {"~", "null", "true", "false", "yes", "no", "on", "off",
                                    "y", "n", ".nan", ".inf", "-.inf", "+.inf"};
    for (const char *k : special)
        if (l == k) return true;

    char *end = nullptr;
    std::strtod(s.c_str(), &end);
    if (end && *end == '\0') return true; // numeric-looking: 123, 1e3, 0x1f, 1.5

    const unsigned char first = static_cast<unsigned char>(s.front());
    const unsigned char last = static_cast<unsigned char>(s.back());
    if (std::isspace(first) || std::isspace(last)) return true;
    if (std::strchr("-?:,[]{}#&*!|>'\"%@`", s.front())) return true;
    if (s.find(": ") != std::string::npos || s.find(" #") != std::string::npos) return true;
    for (unsigned char c : s)
        if (c < 0x20) return true;
    return false;
}

void emit_str(YAML::Emitter &out, const char *key, const std::string &v) {
    out << YAML::Key << key << YAML::Value;
    if (needs_quotes(v))
        out << YAML::DoubleQuoted << v;
    else
        out << v;
}

void emit_session(YAML::Emitter &out, const Session &in) {
    const Session s = in.normalized();
    auto str_if = [&](const char *k, const std::string &v) {
        if (!v.empty()) emit_str(out, k, v);
    };
    auto int_if = [&](const char *k, int v) {
        if (v != 0) out << YAML::Key << k << YAML::Value << v;
    };

    out << YAML::BeginMap;
    emit_str(out, "name", s.name);
    out << YAML::Key << "transport" << YAML::Value << to_string(s.transport);
    str_if("host", s.host);
    int_if("port", s.port);

    if (s.transport == Transport::Ssh) {
        str_if("username", s.username);
        str_if("auth_type", s.auth_type);
        str_if("key_path", s.key_path);
        if (s.jump.in_use()) {
            out << YAML::Key << "jump" << YAML::Value << YAML::BeginMap;
            emit_str(out, "host", s.jump.host);
            int_if("port", s.jump.port);
            str_if("username", s.jump.username);
            str_if("key_path", s.jump.key_path);
            out << YAML::EndMap;
        }
        if (s.legacy_algorithms) out << YAML::Key << "legacy_algorithms" << YAML::Value << true;
        str_if("host_key_policy", s.host_key_policy);
        str_if("known_hosts", s.known_hosts);
        int_if("connect_timeout_sec", s.connect_timeout_sec);
    } else if (s.telnet_crlf.has_value()) {
        out << YAML::Key << "telnet_crlf" << YAML::Value << *s.telnet_crlf;
    }

    str_if("term_type", s.term_type);
    str_if("terminal_theme", s.terminal_theme);
    int_if("font_size", s.font_size);
    int_if("scrollback_lines", s.scrollback_lines);
    out << YAML::EndMap;
}

} // namespace

std::string emit_tree(const Tree &tree) {
    YAML::Emitter out;
    out.SetBoolFormat(YAML::TrueFalseBool);
    out << YAML::BeginMap;
    out << YAML::Key << "version" << YAML::Value << kFileVersion;
    out << YAML::Key << "folders" << YAML::Value << YAML::BeginSeq;
    for (const auto &f : tree.folders) {
        out << YAML::BeginMap;
        emit_str(out, "folder_name", f.name);
        out << YAML::Key << "sessions" << YAML::Value;
        if (f.sessions.empty()) out << YAML::Flow;
        out << YAML::BeginSeq;
        for (const auto &s : f.sessions) emit_session(out, s);
        out << YAML::EndSeq << YAML::EndMap;
    }
    out << YAML::EndSeq << YAML::EndMap;
    std::string text = out.c_str();
    text += '\n';
    return text;
}

std::string save_file(const std::string &path, const Tree &tree) {
    return write_text_atomic(path, emit_tree(tree));
}

std::string write_text_atomic(const std::string &path, const std::string &text) {
    fs::path target(path);
    fs::path dir = target.parent_path();
    if (dir.empty()) dir = ".";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return "create " + dir.string() + ": " + ec.message();

    std::string tmpl = (dir / ".sessions-XXXXXX").string();
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

    // 0600: hosts, usernames and key paths are not secrets, but they are a
    // list of what to attack.
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

std::string seed_sessions_yaml(const std::string &username) {
    std::string user_line;
    if (!username.empty()) {
        Session probe;
        probe.name = "localhost";
        probe.host = "localhost";
        probe.username = username;
        Tree t;
        (void)t.add("Local", probe);
        // Borrow the emitter's quoting so an odd username stays a string.
        const std::string emitted = emit_tree(t);
        const auto at = emitted.find("username: ");
        if (at != std::string::npos) {
            const auto end = emitted.find('\n', at);
            user_line = "        " + emitted.substr(at, end - at) + "\n";
        }
    }
    return "# tmssh sessions\n"
           "#\n"
           "# Edit this file directly; tmssh reloads it when you save. Folders are one\n"
           "# level deep. A session needs a name and a host; transport defaults to ssh.\n"
           "#\n"
           "# ssh:     port, username, auth_type (agent | password | publickey),\n"
           "#          key_path, host_key_policy (tofu | strict | insecure),\n"
           "#          known_hosts, connect_timeout_sec, legacy_algorithms (true),\n"
           "#          jump: {host, port, username, key_path}\n"
           "# telnet:  port, telnet_crlf (false to send a bare CR)\n"
           "# any:     term_type, terminal_theme, font_size, scrollback_lines\n"
           "#\n"
           "# Changes made from tmssh itself (delete, import) rewrite this file\n"
           "# without these comments.\n"
           "version: 1\n"
           "folders:\n"
           "  - folder_name: Local\n"
           "    sessions:\n"
           "      - name: localhost\n"
           "        transport: ssh\n"
           "        host: localhost\n" +
           user_line;
}

std::string default_sessions_path() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg == '/') return (fs::path(xdg) / "tmssh" / "sessions.yaml").string();
    const char *home = std::getenv("HOME");
    fs::path base = (home && *home) ? fs::path(home) : fs::path(".");
    return (base / ".config" / "tmssh" / "sessions.yaml").string();
}

std::string pathfinder_sessions_path() {
    const char *home = std::getenv("HOME");
    fs::path base = (home && *home) ? fs::path(home) : fs::path(".");
    return (base / ".pathfinderssh" / "sessions.yaml").string();
}

// ─────────────────────────────────────────────────────────────────────
// Import
// ─────────────────────────────────────────────────────────────────────

ImportResult import_tree(Tree &dst, const Tree &src) {
    ImportResult res;

    std::vector<std::string> keys;
    for (const auto &f : dst.folders)
        for (const auto &s : f.sessions)
            if (auto k = s.key(); !k.empty()) keys.push_back(std::move(k));
    auto known = [&](const std::string &k) {
        return std::find(keys.begin(), keys.end(), k) != keys.end();
    };

    for (const auto &sf : src.folders) {
        for (const auto &in : sf.sessions) {
            Session s = in.normalized();
            const std::string where = sf.name + "/" + s.label();
            const std::string k = s.key();
            if (k.empty()) {
                res.rejected.push_back(where);
                continue;
            }
            if (known(k)) {
                res.skipped.push_back(where);
                continue;
            }

            int fi = dst.folder_index(sf.name);
            if (fi >= 0 && dst.folders[fi].session_index(s.name) >= 0) {
                const std::string base = s.name;
                for (int n = 2;; ++n) {
                    std::string candidate = base + " (" + std::to_string(n) + ")";
                    if (dst.folders[fi].session_index(candidate) < 0) {
                        s.name = candidate;
                        break;
                    }
                }
                res.renamed.push_back(where + " -> " + s.name);
            }
            if (auto err = dst.add(sf.name, s); !err.empty()) {
                res.rejected.push_back(where + ": " + err);
                continue;
            }
            keys.push_back(k);
            ++res.added;
        }
    }
    return res;
}

} // namespace tmssh
