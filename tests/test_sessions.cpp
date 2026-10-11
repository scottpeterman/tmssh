// tests/test_sessions.cpp -- the session model, the file format, tree edits,
// atomic save and import.
#include "sessions.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace tmssh;

namespace {

Session ssh(const std::string &name, const std::string &host, const std::string &user = "admin") {
    Session s;
    s.name = name;
    s.host = host;
    s.username = user;
    return s;
}

// A fresh directory under the build's temp dir, removed on scope exit.
struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "tmssh-test-XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        REQUIRE(::mkdtemp(buf.data()) != nullptr);
        path = buf.data();
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string slurp(const fs::path &p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool has_warning(const LoadResult &r, const std::string &needle) {
    for (const auto &w : r.warnings)
        if (w.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Session
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("normalized fills defaults and trims") {
    Session s;
    s.name = "  spine  ";
    s.host = " 10.0.0.1 ";
    Session n = s.normalized();
    CHECK(n.name == "spine");
    CHECK(n.host == "10.0.0.1");
    CHECK(n.port == 22);
    CHECK(n.auth_type == kAuthAgent);
    CHECK(n.host_key_policy == kHostKeyTofu);
    CHECK(n.connect_timeout_sec == kDefaultConnectTimeoutSec);
    CHECK(n.term_type == kDefaultTermType);
    // inherit-from-app fields stay unset
    CHECK(n.font_size == 0);
    CHECK(n.scrollback_lines == 0);
    CHECK(n.terminal_theme.empty());
}

TEST_CASE("telnet defaults to port 23") {
    Session s;
    s.transport = Transport::Telnet;
    s.host = "cs1";
    CHECK(s.normalized().port == 23);
}

TEST_CASE("jump port defaults only when a jump host is set") {
    Session s = ssh("a", "h");
    CHECK(s.normalized().jump.port == 0);
    s.jump.host = "bastion";
    CHECK(s.normalized().jump.port == 22);
}

TEST_CASE("label and target") {
    Session s;
    s.host = "10.0.0.1";
    s.username = "admin";
    CHECK(s.label() == "admin@10.0.0.1");
    s.port = 2201;
    CHECK(s.target() == "admin@10.0.0.1:2201");
    s.name = "lab";
    CHECK(s.label() == "lab");

    Session t;
    t.transport = Transport::Telnet;
    t.host = "cs1";
    t.port = 2003;
    t.username = "ignored";
    CHECK(t.target() == "cs1:2003");
    t.port = 23;
    CHECK(t.target() == "cs1");
}

TEST_CASE("an unnamed session takes its target as its name") {
    Session s;
    s.host = "r1";
    s.username = "u";
    CHECK(s.normalized().name == "u@r1");
}

TEST_CASE("key is transport, ssh user, lowercased host and effective port") {
    Session a = ssh("x", "R1.Lab");
    Session b = ssh("y", "r1.lab");
    b.port = 22;
    CHECK(a.key() == "ssh:admin@r1.lab:22");
    CHECK(a.key() == b.key());

    Session other_user = ssh("z", "r1.lab", "operator");
    CHECK(other_user.key() != a.key());

    Session no_user = ssh("n", "r1.lab", "");
    CHECK(no_user.key() == "ssh:r1.lab:22");

    Session t = a;
    t.transport = Transport::Telnet;
    CHECK(t.key() == "telnet:r1.lab:23"); // telnet has no user in the model

    Session none;
    CHECK(none.key().empty());
}

TEST_CASE("crlf defaults on") {
    Session s;
    CHECK(s.crlf());
    s.telnet_crlf = false;
    CHECK_FALSE(s.crlf());
}

// ─────────────────────────────────────────────────────────────────────
// Validation
// ─────────────────────────────────────────────────────────────────────

namespace {
bool has_field(const std::vector<FieldError> &errs, const std::string &f) {
    for (const auto &e : errs)
        if (e.field == f) return true;
    return false;
}
} // namespace

TEST_CASE("a complete ssh session validates") {
    CHECK(validate(ssh("a", "10.0.0.1")).empty());
}

TEST_CASE("validation reports every problem at once") {
    Session s;
    s.port = 70000;
    s.auth_type = "kerberos";
    s.host_key_policy = "maybe";
    s.font_size = 3;
    s.scrollback_lines = -1;
    auto errs = validate(s);
    CHECK(has_field(errs, "host"));
    CHECK(has_field(errs, "port"));
    CHECK(has_field(errs, "auth_type"));
    CHECK(has_field(errs, "username"));
    CHECK(has_field(errs, "host_key_policy"));
    CHECK(has_field(errs, "font_size"));
    CHECK(has_field(errs, "scrollback_lines"));
}

TEST_CASE("host with whitespace is rejected") {
    CHECK(has_field(validate(ssh("a", "10.0.0.1 22")), "host"));
}

TEST_CASE("publickey needs a key path") {
    Session s = ssh("a", "h");
    s.auth_type = kAuthPublicKey;
    CHECK(has_field(validate(s), "key_path"));
    s.key_path = "~/.ssh/id_ed25519";
    CHECK(validate(s).empty());
}

TEST_CASE("jump host needs a username") {
    Session s = ssh("a", "h");
    s.jump.host = "bastion";
    CHECK(has_field(validate(s), "jump.username"));
    s.jump.username = "me";
    CHECK(validate(s).empty());
}

TEST_CASE("telnet skips the ssh rules") {
    Session s;
    s.transport = Transport::Telnet;
    s.host = "cs1";
    s.auth_type = "nonsense";
    CHECK(validate(s).empty());
}

// ─────────────────────────────────────────────────────────────────────
// Parsing
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("parses the native PathfinderSSH shape") {
    auto r = parse_tree(R"(
version: 1
folders:
  - folder_name: Lab
    sessions:
      - name: eng-spine-1
        transport: ssh
        host: 172.16.2.5
        username: admin
        auth_type: password
        terminal_theme: ice
      - name: cs1-port3
        transport: telnet
        host: cs1
        port: 2003
        telnet_crlf: false
  - folder_name: Empty
    sessions: []
)");
    REQUIRE(r.ok());
    CHECK(r.warnings.empty());
    REQUIRE(r.tree.folders.size() == 2);
    const auto &lab = r.tree.folders[0];
    CHECK(lab.name == "Lab");
    REQUIRE(lab.sessions.size() == 2);

    const auto &a = lab.sessions[0];
    CHECK(a.transport == Transport::Ssh);
    CHECK(a.host == "172.16.2.5");
    CHECK(a.port == 22);
    CHECK(a.auth_type == kAuthPassword);
    CHECK(a.terminal_theme == "ice");

    const auto &t = lab.sessions[1];
    CHECK(t.transport == Transport::Telnet);
    CHECK(t.port == 2003);
    REQUIRE(t.telnet_crlf.has_value());
    CHECK_FALSE(*t.telnet_crlf);

    CHECK(r.tree.folders[1].sessions.empty());
}

TEST_CASE("missing transport means ssh") {
    auto r = parse_tree("folders:\n  - folder_name: A\n    sessions:\n      - name: x\n        host: h\n");
    REQUIRE(r.ok());
    CHECK(r.tree.folders[0].sessions[0].transport == Transport::Ssh);
}

TEST_CASE("serial sessions are skipped and counted") {
    auto r = parse_tree(R"(
version: 1
folders:
  - folder_name: Console
    sessions:
      - name: usb0
        transport: serial
        serial_port: /dev/ttyUSB0
        baud: 9600
      - name: r1
        transport: ssh
        host: r1
)");
    REQUIRE(r.ok());
    CHECK(r.skipped_serial == 1);
    CHECK(r.warnings.empty());
    REQUIRE(r.tree.folders[0].sessions.size() == 1);
    CHECK(r.tree.folders[0].sessions[0].name == "r1");
}

TEST_CASE("keys tmssh does not model are ignored without warnings") {
    auto r = parse_tree(R"(
folders:
  - folder_name: Lab
    sessions:
      - name: r1
        host: r1
        credential: lab-default
        anti_idle: {mode: on, interval_sec: 60}
        paste_line_delay_ms: 20
        console_baud: 9600
        log_enabled: true
        vendor: Arista
        notes: core
)");
    REQUIRE(r.ok());
    CHECK(r.warnings.empty());
    CHECK(r.tree.folders[0].sessions.size() == 1);
}

TEST_CASE("unknown transport is a warning, not an error") {
    auto r = parse_tree("folders:\n  - folder_name: A\n    sessions:\n      - name: x\n        transport: rlogin\n        host: h\n");
    REQUIRE(r.ok());
    CHECK(r.tree.folders[0].sessions.empty());
    CHECK(has_warning(r, "unknown transport"));
}

TEST_CASE("a bad number is a warning and the field keeps its default") {
    auto r = parse_tree("folders:\n  - folder_name: A\n    sessions:\n      - name: x\n        host: h\n        port: twenty-two\n");
    REQUIRE(r.ok());
    CHECK(r.tree.folders[0].sessions[0].port == 22);
    CHECK(has_warning(r, "port is not a number"));
}

TEST_CASE("quoted port, as the older file wrote it, is accepted") {
    auto r = parse_tree("folders:\n  - folder_name: A\n    sessions:\n      - name: x\n        host: h\n        port: \"2201\"\n");
    REQUIRE(r.ok());
    CHECK(r.warnings.empty());
    CHECK(r.tree.folders[0].sessions[0].port == 2201);
}

TEST_CASE("bare top-level list of folders") {
    auto r = parse_tree("- folder_name: Old\n  sessions:\n    - name: r1\n      host: r1\n");
    REQUIRE(r.ok());
    REQUIRE(r.tree.folders.size() == 1);
    CHECK(r.tree.folders[0].name == "Old");
}

TEST_CASE("folders differing only in case are merged") {
    auto r = parse_tree(R"(
folders:
  - folder_name: Lab
    sessions: [{name: a, host: a}]
  - folder_name: lab
    sessions: [{name: b, host: b}]
)");
    REQUIRE(r.ok());
    REQUIRE(r.tree.folders.size() == 1);
    CHECK(r.tree.folders[0].sessions.size() == 2);
    CHECK(has_warning(r, "appears twice"));
}

TEST_CASE("duplicate session names in one folder keep the first") {
    auto r = parse_tree(R"(
folders:
  - folder_name: Lab
    sessions: [{name: r1, host: a}, {name: R1, host: b}]
)");
    REQUIRE(r.ok());
    REQUIRE(r.tree.folders[0].sessions.size() == 1);
    CHECK(r.tree.folders[0].sessions[0].host == "a");
    CHECK(has_warning(r, "duplicate name"));
}

TEST_CASE("empty and blank text is an empty tree") {
    CHECK(parse_tree("").ok());
    CHECK(parse_tree("  \n\n").tree.folders.empty());
}

TEST_CASE("malformed YAML is an error") {
    auto r = parse_tree("folders: [unclosed");
    CHECK_FALSE(r.ok());
    CHECK(r.tree.folders.empty());
}

TEST_CASE("folders that is not a list is an error") {
    CHECK_FALSE(parse_tree("folders: nope\n").ok());
}

// ─────────────────────────────────────────────────────────────────────
// Emitting
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("emit then parse round-trips every modelled field") {
    Tree t;
    Session a = ssh("spine", "10.0.0.1");
    a.port = 2222;
    a.auth_type = kAuthPublicKey;
    a.key_path = "/home/me/.ssh/id_ed25519";
    a.jump.host = "bastion.example";
    a.jump.port = 2200;
    a.jump.username = "jumper";
    a.jump.key_path = "/home/me/.ssh/jump";
    a.legacy_algorithms = true;
    a.host_key_policy = kHostKeyStrict;
    a.known_hosts = "/home/me/.ssh/lab_hosts";
    a.connect_timeout_sec = 7;
    a.term_type = "vt100";
    a.terminal_theme = "gruvbox-dark";
    a.font_size = 13;
    a.scrollback_lines = 20000;

    Session b;
    b.name = "console";
    b.transport = Transport::Telnet;
    b.host = "cs1";
    b.port = 2003;
    b.telnet_crlf = false;

    REQUIRE(t.add("Lab", a).empty());
    REQUIRE(t.add("Lab", b).empty());
    REQUIRE(t.add_folder("Empty").empty());

    auto r = parse_tree(emit_tree(t));
    REQUIRE(r.ok());
    CHECK(r.warnings.empty());
    REQUIRE(r.tree.folders.size() == 2);
    CHECK(r.tree.folders[1].name == "Empty");
    REQUIRE(r.tree.folders[0].sessions.size() == 2);
    CHECK(r.tree.folders[0].sessions[0] == a.normalized());
    CHECK(r.tree.folders[0].sessions[1] == b.normalized());
}

TEST_CASE("a telnet session writes no ssh fields") {
    Tree t;
    Session s;
    s.name = "cs";
    s.transport = Transport::Telnet;
    s.host = "cs1";
    s.username = "leftover";
    s.key_path = "/leftover";
    s.legacy_algorithms = true;
    REQUIRE(t.add("A", s).empty());
    std::string y = emit_tree(t);
    CHECK(y.find("username") == std::string::npos);
    CHECK(y.find("key_path") == std::string::npos);
    CHECK(y.find("legacy_algorithms") == std::string::npos);
    CHECK(y.find("host_key_policy") == std::string::npos);
    CHECK(y.find("telnet_crlf") == std::string::npos); // unset stays unset
}

TEST_CASE("an ssh session writes no telnet field and no unused jump") {
    Tree t;
    Session s = ssh("a", "h");
    s.telnet_crlf = false;
    REQUIRE(t.add("A", s).empty());
    std::string y = emit_tree(t);
    CHECK(y.find("telnet_crlf") == std::string::npos);
    CHECK(y.find("jump") == std::string::npos);
}

TEST_CASE("names YAML would misread survive a round trip") {
    const char *names[] = {"true", "no", "null", "~", "123", "1e3", "0x1f", "- dash", "a: b",
                           "#hash", "  padded", "::1", "it's", "say \"hi\"", "on"};
    Tree t;
    int i = 0;
    for (const char *n : names) {
        Session s = ssh(n, "h" + std::to_string(i++));
        REQUIRE(t.add("F", s).empty());
    }
    auto r = parse_tree(emit_tree(t));
    REQUIRE(r.ok());
    REQUIRE(r.tree.folders[0].sessions.size() == std::size(names));
    for (std::size_t k = 0; k < std::size(names); ++k) {
        Session want;
        want.name = names[k];
        CHECK(r.tree.folders[0].sessions[k].name == want.normalized().name);
    }
}

TEST_CASE("IPv6 hosts survive a round trip") {
    Tree t;
    REQUIRE(t.add("F", ssh("v6", "2001:db8::1")).empty());
    REQUIRE(t.add("F", ssh("lo6", "::1")).empty());
    auto r = parse_tree(emit_tree(t));
    REQUIRE(r.ok());
    CHECK(r.tree.folders[0].sessions[0].host == "2001:db8::1");
    CHECK(r.tree.folders[0].sessions[1].host == "::1");
}

TEST_CASE("emitted file carries a version") {
    Tree t;
    CHECK(emit_tree(t).find("version: 1") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────
// Tree editing
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("folder names are unique, case-insensitively") {
    Tree t;
    CHECK(t.add_folder("Lab").empty());
    CHECK_FALSE(t.add_folder(" lab ").empty());
    CHECK_FALSE(t.add_folder("  ").empty());
}

TEST_CASE("rename folder") {
    Tree t;
    REQUIRE(t.add_folder("A").empty());
    REQUIRE(t.add_folder("B").empty());
    CHECK_FALSE(t.rename_folder("A", "b").empty());
    CHECK(t.rename_folder("A", "a").empty()); // case change of itself is fine
    CHECK(t.folders[0].name == "a");
    CHECK_FALSE(t.rename_folder("missing", "x").empty());
}

TEST_CASE("remove folder refuses a non-empty folder unless forced") {
    Tree t;
    REQUIRE(t.add("Lab", ssh("a", "h")).empty());
    CHECK_FALSE(t.remove_folder("Lab", false).empty());
    CHECK(t.folders.size() == 1);
    CHECK(t.remove_folder("Lab", true).empty());
    CHECK(t.folders.empty());
}

TEST_CASE("add creates the folder and refuses duplicate names") {
    Tree t;
    CHECK(t.add("New", ssh("r1", "h1")).empty());
    REQUIRE(t.folders.size() == 1);
    CHECK_FALSE(t.add("new", ssh("R1", "h2")).empty());
    CHECK(t.session_count() == 1);
    CHECK_FALSE(t.add("New", Session{}).empty()); // no name, no host
}

TEST_CASE("replace keeps position and refuses a clashing rename") {
    Tree t;
    REQUIRE(t.add("F", ssh("a", "1")).empty());
    REQUIRE(t.add("F", ssh("b", "2")).empty());
    REQUIRE(t.add("F", ssh("c", "3")).empty());

    CHECK(t.replace("F", "b", ssh("b2", "22")).empty());
    CHECK(t.folders[0].sessions[1].name == "b2");
    CHECK(t.folders[0].sessions[1].host == "22");

    CHECK_FALSE(t.replace("F", "b2", ssh("a", "x")).empty());
    CHECK(t.folders[0].sessions[1].name == "b2");
}

TEST_CASE("remove") {
    Tree t;
    REQUIRE(t.add("F", ssh("a", "1")).empty());
    CHECK(t.remove("F", "A").empty());
    CHECK(t.folders[0].sessions.empty());
    CHECK_FALSE(t.remove("F", "a").empty());
}

TEST_CASE("move, and a refused move leaves the session in place") {
    Tree t;
    REQUIRE(t.add("A", ssh("r1", "1")).empty());
    REQUIRE(t.add("B", ssh("r1", "2")).empty());
    REQUIRE(t.add("A", ssh("r2", "3")).empty());

    CHECK_FALSE(t.move("A", "r1", "B").empty());
    CHECK(t.folders[0].sessions.size() == 2);

    CHECK(t.move("A", "r2", "C").empty());
    CHECK(t.folders[0].sessions.size() == 1);
    REQUIRE(t.folder_index("C") >= 0);
    CHECK(t.folders[t.folder_index("C")].sessions[0].host == "3");

    CHECK(t.move("A", "r1", "a").empty()); // same folder: no-op
    CHECK(t.folders[0].sessions.size() == 1);
}

// ─────────────────────────────────────────────────────────────────────
// Files
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("missing file is an empty tree, not an error") {
    TempDir d;
    auto r = load_file((d.path / "nope.yaml").string());
    CHECK(r.ok());
    CHECK(r.tree.folders.empty());
}

TEST_CASE("save creates the directory, writes 0600, leaves no temp files") {
    TempDir d;
    fs::path p = d.path / "deep" / "er" / "sessions.yaml";
    Tree t;
    REQUIRE(t.add("Lab", ssh("r1", "10.0.0.1")).empty());

    REQUIRE(save_file(p.string(), t) == "");
    REQUIRE(fs::exists(p));

    struct stat st {};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    int entries = 0;
    for (const auto &e : fs::directory_iterator(p.parent_path())) {
        (void)e;
        ++entries;
    }
    CHECK(entries == 1);

    auto r = load_file(p.string());
    REQUIRE(r.ok());
    CHECK(r.tree.session_count() == 1);
}

TEST_CASE("save replaces an existing file whole") {
    TempDir d;
    fs::path p = d.path / "sessions.yaml";
    Tree big;
    for (int i = 0; i < 50; ++i) REQUIRE(big.add("F", ssh("r" + std::to_string(i), "h")).empty());
    REQUIRE(save_file(p.string(), big) == "");

    Tree small;
    REQUIRE(small.add("F", ssh("only", "h")).empty());
    REQUIRE(save_file(p.string(), small) == "");
    CHECK(slurp(p) == emit_tree(small));
}

TEST_CASE("save into an unwritable directory fails cleanly") {
    if (::geteuid() == 0) return; // root ignores the mode
    TempDir d;
    fs::path ro = d.path / "ro";
    fs::create_directories(ro);
    fs::permissions(ro, fs::perms::owner_read | fs::perms::owner_exec);
    Tree t;
    CHECK_FALSE(save_file((ro / "s.yaml").string(), t).empty());
    fs::permissions(ro, fs::perms::owner_all);
}

TEST_CASE("unreadable path is an error") {
    TempDir d;
    // A directory where the file should be.
    fs::create_directories(d.path / "sessions.yaml");
    auto r = load_file((d.path / "sessions.yaml").string());
    CHECK_FALSE(r.ok());
}

TEST_CASE("default path honours XDG_CONFIG_HOME") {
    const char *old = std::getenv("XDG_CONFIG_HOME");
    std::string saved = old ? old : "";
    ::setenv("XDG_CONFIG_HOME", "/tmp/xdg-test", 1);
    CHECK(default_sessions_path() == "/tmp/xdg-test/tmssh/sessions.yaml");
    // A relative value is not a valid XDG path and is ignored.
    ::setenv("XDG_CONFIG_HOME", "relative", 1);
    CHECK(default_sessions_path().find("/.config/tmssh/sessions.yaml") != std::string::npos);
    if (old)
        ::setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
    else
        ::unsetenv("XDG_CONFIG_HOME");
}

// ─────────────────────────────────────────────────────────────────────
// Import
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("import adds new, skips known addresses, renames clashes") {
    Tree dst;
    REQUIRE(dst.add("Lab", ssh("spine1", "10.0.0.1")).empty());
    REQUIRE(dst.add("Lab", ssh("leaf1", "10.0.0.2")).empty());

    Tree src;
    REQUIRE(src.add("lab", ssh("renamed-spine", "10.0.0.1")).empty()); // same device
    REQUIRE(src.add("lab", ssh("leaf1", "10.0.0.3")).empty());          // name clash
    REQUIRE(src.add("Edge", ssh("edge1", "10.0.1.1")).empty());         // new folder
    Session nohost;
    nohost.name = "ghost";
    src.folders.back().sessions.push_back(nohost);

    auto r = import_tree(dst, src);
    CHECK(r.added == 2);
    CHECK(r.skipped.size() == 1);
    CHECK(r.renamed.size() == 1);
    CHECK(r.rejected.size() == 1);

    REQUIRE(dst.folders.size() == 2);
    const auto &lab = dst.folders[0];
    CHECK(lab.sessions[0].name == "spine1"); // untouched
    CHECK(lab.sessions.size() == 3);
    CHECK(lab.sessions[2].name == "leaf1 (2)");
    CHECK(lab.sessions[2].host == "10.0.0.3");
}

TEST_CASE("import is safe to re-run") {
    Tree dst;
    Tree src;
    REQUIRE(src.add("A", ssh("r1", "1")).empty());
    REQUIRE(src.add("A", ssh("r2", "2")).empty());
    CHECK(import_tree(dst, src).added == 2);
    auto again = import_tree(dst, src);
    CHECK(again.added == 0);
    CHECK(again.skipped.size() == 2);
    CHECK(dst.session_count() == 2);
}

TEST_CASE("import keeps two accounts on one device") {
    Tree dst;
    Tree src;
    REQUIRE(src.add("A", ssh("as-admin", "10.0.0.1", "admin")).empty());
    REQUIRE(src.add("A", ssh("as-oper", "10.0.0.1", "operator")).empty());
    CHECK(import_tree(dst, src).added == 2);
}

TEST_CASE("import skips duplicates within the source itself") {
    Tree dst;
    Tree src;
    REQUIRE(src.add("A", ssh("r1", "1")).empty());
    REQUIRE(src.add("B", ssh("r1-again", "1")).empty());
    auto r = import_tree(dst, src);
    CHECK(r.added == 1);
    CHECK(r.skipped.size() == 1);
}

// ─────────────────────────────────────────────────────────────────────
// Compatibility with files PathfinderSSH actually writes
// ─────────────────────────────────────────────────────────────────────

// Written by PathfinderSSH's own sessions.SaveFile (internal/sessions, yaml.v3).
static const char *kPathfinderFile = R"(version: 1
folders:
    - folder_name: Lab
      sessions:
        - name: eng-spine-1
          transport: ssh
          host: 172.16.2.5
          port: 22
          username: admin
          credential: lab
          auth_type: password
          jump:
            host: bastion
            port: 22
            username: jumper
          legacy_algorithms: true
          host_key_policy: tofu
          connect_timeout_sec: 20
          term_type: xterm-256color
          terminal_theme: ice
          anti_idle:
            mode: inherit
          vendor: Arista
        - name: cs1-p3
          transport: telnet
          host: cs1
          port: 2003
          connect_timeout_sec: 20
          telnet_crlf: false
          term_type: xterm-256color
          anti_idle:
            mode: inherit
    - folder_name: Console
      sessions:
        - name: usb0
          transport: serial
          serial_port: /dev/ttyUSB0
          baud: 9600
          data_bits: 8
          parity: none
          stop_bits: "1"
          anti_idle:
            mode: inherit
    - folder_name: Edge
      sessions:
        - name: "true"
          transport: ssh
          host: 2001:db8::1
          port: 22
          username: u
          auth_type: agent
          host_key_policy: tofu
          connect_timeout_sec: 20
          term_type: xterm-256color
          anti_idle:
            mode: inherit
)";

TEST_CASE("reads a file PathfinderSSH wrote") {
    auto r = parse_tree(kPathfinderFile);
    REQUIRE(r.ok());
    CHECK(r.warnings.empty());
    CHECK(r.skipped_serial == 1);

    // The serial-only Console folder is dropped rather than shown empty.
    REQUIRE(r.tree.folders.size() == 2);
    CHECK(r.tree.folders[0].name == "Lab");
    CHECK(r.tree.folders[1].name == "Edge");

    const auto &spine = r.tree.folders[0].sessions[0];
    CHECK(spine.target() == "admin@172.16.2.5");
    CHECK(spine.auth_type == kAuthPassword);
    CHECK(spine.jump.host == "bastion");
    CHECK(spine.jump.username == "jumper");
    CHECK(spine.legacy_algorithms);
    CHECK(spine.terminal_theme == "ice");
    CHECK(validate(spine).empty());

    const auto &cs = r.tree.folders[0].sessions[1];
    CHECK(cs.transport == Transport::Telnet);
    CHECK_FALSE(cs.crlf());

    const auto &edge = r.tree.folders[1].sessions[0];
    CHECK(edge.name == "true");
    CHECK(edge.host == "2001:db8::1");
}

TEST_CASE("an empty folder in the file is kept and written as []") {
    auto r = parse_tree("folders:\n  - folder_name: Later\n    sessions: []\n");
    REQUIRE(r.ok());
    REQUIRE(r.tree.folders.size() == 1);
    CHECK(emit_tree(r.tree).find("sessions: []") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────
// First-run seed
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("the seed file parses cleanly to Local/localhost as the user") {
    auto r = parse_tree(seed_sessions_yaml("speterman"));
    REQUIRE(r.ok());
    CHECK(r.warnings.empty());
    REQUIRE(r.tree.folders.size() == 1);
    CHECK(r.tree.folders[0].name == "Local");
    REQUIRE(r.tree.folders[0].sessions.size() == 1);
    const Session &s = r.tree.folders[0].sessions[0];
    CHECK(s.name == "localhost");
    CHECK(s.transport == Transport::Ssh);
    CHECK(s.host == "localhost");
    CHECK(s.username == "speterman");
    CHECK(s.target() == "speterman@localhost");
}

TEST_CASE("the seed keeps odd usernames as strings and omits an empty one") {
    auto odd = parse_tree(seed_sessions_yaml("true"));
    REQUIRE(odd.ok());
    CHECK(odd.tree.folders[0].sessions[0].username == "true");

    auto none = parse_tree(seed_sessions_yaml(""));
    REQUIRE(none.ok());
    CHECK(none.tree.folders[0].sessions[0].username.empty());
}

TEST_CASE("write_text_atomic writes exactly the text") {
    TempDir d;
    fs::path p = d.path / "x" / "seed.yaml";
    const std::string text = seed_sessions_yaml("me");
    REQUIRE(write_text_atomic(p.string(), text) == "");
    CHECK(slurp(p) == text);
}
