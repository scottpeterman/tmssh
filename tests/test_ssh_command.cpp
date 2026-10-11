// tests/test_ssh_command.cpp -- Session -> ssh command line, and settings.
#include "settings.h"
#include "ssh_command.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace tmssh;

namespace {

Session base() {
    Session s;
    s.name = "r1";
    s.host = "10.0.0.1";
    s.username = "admin";
    return s;
}

bool has(const Command &c, const std::string &a) {
    return std::find(c.args.begin(), c.args.end(), a) != c.args.end();
}

// The value following flag, or "" if the flag is absent.
std::string after(const Command &c, const std::string &flag) {
    auto it = std::find(c.args.begin(), c.args.end(), flag);
    return (it == c.args.end() || it + 1 == c.args.end()) ? std::string{} : *(it + 1);
}

// Every -o value starting with prefix.
std::vector<std::string> options(const Command &c, const std::string &prefix) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i + 1 < c.args.size(); ++i)
        if (c.args[i] == "-o" && c.args[i + 1].rfind(prefix, 0) == 0) out.push_back(c.args[i + 1]);
    return out;
}

SshAlgorithms modern_client() {
    SshAlgorithms a;
    a.probed = true;
    a.kex = {"curve25519-sha256", "diffie-hellman-group14-sha1", "diffie-hellman-group1-sha1",
             "diffie-hellman-group-exchange-sha1", "diffie-hellman-group-exchange-sha256"};
    a.ciphers = {"aes128-ctr", "aes128-cbc", "aes256-cbc", "3des-cbc"};
    a.macs = {"hmac-sha2-256", "hmac-sha1", "hmac-md5"};
    a.host_keys = {"ssh-ed25519", "ssh-rsa"}; // no ssh-dss, as on OpenSSH 9.8+
    return a;
}

} // namespace

TEST_CASE("plain session: port, user, defaults, host last after --") {
    Command c = ssh_command(base(), {});
    CHECK(c.program == "ssh");
    CHECK(after(c, "-p") == "22");
    CHECK(after(c, "-l") == "admin");
    CHECK(options(c, "StrictHostKeyChecking=") == std::vector<std::string>{"StrictHostKeyChecking=accept-new"});
    CHECK(options(c, "ConnectTimeout=") == std::vector<std::string>{"ConnectTimeout=20"});
    REQUIRE(c.args.size() >= 2);
    CHECK(c.args[c.args.size() - 2] == "--");
    CHECK(c.args.back() == "10.0.0.1");
    CHECK(c.env == std::vector<std::string>{"TERM=xterm-256color"});
    // agent auth adds nothing
    CHECK(options(c, "PreferredAuthentications").empty());
    CHECK_FALSE(has(c, "-i"));
}

TEST_CASE("no username means no -l") {
    Session s = base();
    s.username.clear();
    CHECK_FALSE(has(ssh_command(s, {}), "-l"));
}

TEST_CASE("password auth") {
    Session s = base();
    s.auth_type = kAuthPassword;
    Command c = ssh_command(s, {});
    CHECK(options(c, "PreferredAuthentications=").front() ==
          "PreferredAuthentications=keyboard-interactive,password");
    CHECK(options(c, "PubkeyAuthentication=").front() == "PubkeyAuthentication=no");
}

TEST_CASE("publickey auth expands ~ and pins the identity") {
    const char *old = std::getenv("HOME");
    std::string saved = old ? old : "";
    ::setenv("HOME", "/home/tester", 1);
    Session s = base();
    s.auth_type = kAuthPublicKey;
    s.key_path = "~/.ssh/lab";
    Command c = ssh_command(s, {});
    CHECK(after(c, "-i") == "/home/tester/.ssh/lab");
    CHECK(options(c, "IdentitiesOnly=").front() == "IdentitiesOnly=yes");
    ::setenv("HOME", saved.c_str(), 1);
}

TEST_CASE("host key policies") {
    Session s = base();
    s.host_key_policy = kHostKeyStrict;
    s.known_hosts = "/tmp/kh";
    Command c = ssh_command(s, {});
    CHECK(options(c, "StrictHostKeyChecking=").front() == "StrictHostKeyChecking=yes");
    CHECK(options(c, "UserKnownHostsFile=").front() == "UserKnownHostsFile=/tmp/kh");

    s.host_key_policy = kHostKeyInsecure;
    c = ssh_command(s, {});
    CHECK(options(c, "StrictHostKeyChecking=").front() == "StrictHostKeyChecking=no");
    // insecure never writes to a known_hosts file, even a named one
    CHECK(options(c, "UserKnownHostsFile=") == std::vector<std::string>{"UserKnownHostsFile=/dev/null"});
}

TEST_CASE("jump host without a key uses -J") {
    Session s = base();
    s.jump.host = "bastion";
    s.jump.username = "j";
    CHECK(after(ssh_command(s, {}), "-J") == "j@bastion");

    s.jump.port = 2200;
    CHECK(after(ssh_command(s, {}), "-J") == "j@bastion:2200");

    s.jump.host = "2001:db8::5";
    CHECK(after(ssh_command(s, {}), "-J") == "j@[2001:db8::5]:2200");
}

TEST_CASE("jump host with a key uses a quoted ProxyCommand") {
    Session s = base();
    s.jump.host = "bastion";
    s.jump.username = "j";
    s.jump.key_path = "/keys/my key";
    Command c = ssh_command(s, {});
    CHECK_FALSE(has(c, "-J"));
    auto pc = options(c, "ProxyCommand=");
    REQUIRE(pc.size() == 1);
    CHECK(pc[0] == "ProxyCommand=ssh -i '/keys/my key' -o IdentitiesOnly=yes -p 22 -l j -W %h:%p -- bastion");
}

TEST_CASE("legacy algorithms are cut to what the client supports") {
    Session s = base();
    s.legacy_algorithms = true;
    Command c = ssh_command(s, modern_client());
    CHECK(options(c, "KexAlgorithms=").front() ==
          "KexAlgorithms=+diffie-hellman-group14-sha1,diffie-hellman-group1-sha1,"
          "diffie-hellman-group-exchange-sha256,diffie-hellman-group-exchange-sha1");
    CHECK(options(c, "Ciphers=").front() == "Ciphers=+aes128-cbc,aes256-cbc,3des-cbc");
    CHECK(options(c, "MACs=").front() == "MACs=+hmac-sha1,hmac-md5");
    CHECK(options(c, "HostKeyAlgorithms=").front() == "HostKeyAlgorithms=+ssh-rsa");
}

TEST_CASE("legacy is skipped when the client could not be probed") {
    Session s = base();
    s.legacy_algorithms = true;
    Command c = ssh_command(s, SshAlgorithms{});
    CHECK(options(c, "KexAlgorithms=").empty());
    CHECK(options(c, "Ciphers=").empty());
}

TEST_CASE("legacy off adds nothing even with a probed client") {
    CHECK(options(ssh_command(base(), modern_client()), "Ciphers=").empty());
}

TEST_CASE("an algorithm list with nothing supported is left out") {
    Session s = base();
    s.legacy_algorithms = true;
    SshAlgorithms a = modern_client();
    a.macs = {"hmac-sha2-256"};
    CHECK(options(ssh_command(s, a), "MACs=").empty());
}

TEST_CASE("term_type and port override") {
    Session s = base();
    s.port = 2222;
    s.term_type = "vt100";
    Command c = ssh_command(s, {});
    CHECK(after(c, "-p") == "2222");
    CHECK(c.env == std::vector<std::string>{"TERM=vt100"});
}

TEST_CASE("a host beginning with a dash stays a host") {
    Session s = base();
    s.host = "-oProxyCommand=evil";
    Command c = ssh_command(s, {});
    CHECK(c.args[c.args.size() - 2] == "--");
    CHECK(c.args.back() == "-oProxyCommand=evil");
}

TEST_CASE("shell_quote") {
    CHECK(shell_quote("plain-word_1.2") == "plain-word_1.2");
    CHECK(shell_quote("") == "''");
    CHECK(shell_quote("a b") == "'a b'");
    CHECK(shell_quote("it's") == "'it'\\''s'");
    CHECK(shell_quote("$(rm -rf /)") == "'$(rm -rf /)'");
}

TEST_CASE("display reads like a shell command") {
    Session s = base();
    s.name = "x";
    Command c = ssh_command(s, {});
    CHECK(c.display().rfind("TERM=xterm-256color ssh -p 22 -l admin ", 0) == 0);
}

TEST_CASE("probe against the real client, when there is one") {
    SshAlgorithms a = probe_ssh_algorithms();
    if (!a.probed) return; // no ssh installed on this machine
    CHECK(a.kex.count("curve25519-sha256") == 1);
    CHECK_FALSE(a.ciphers.empty());
}

TEST_CASE("probe of a missing program reports not probed") {
    SshAlgorithms a = probe_ssh_algorithms("/nonexistent/ssh");
    CHECK_FALSE(a.probed);
    CHECK(a.kex.empty());
}

// ─────────────────────────────────────────────────────────────────────
// Settings
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("settings defaults and round trip") {
    Settings s;
    s.app = "light";
    s.terminal_theme = "nord";
    s.font = "JetBrains Mono 13";
    s.scrollback_lines = 20000;
    s.sidebar_width = 300;
    auto r = parse_settings(emit_settings(s));
    CHECK(r.warnings.empty());
    CHECK(r.settings.app == "light");
    CHECK(r.settings.terminal_theme == "nord");
    CHECK(r.settings.font == "JetBrains Mono 13");
    CHECK(r.settings.scrollback_lines == 20000);
    CHECK(r.settings.sidebar_width == 300);
}

TEST_CASE("bad settings values are warned about and defaulted") {
    auto r = parse_settings("app: purple\nscrollback_lines: lots\nwindow_width: 5\nfont: \"\"\n");
    CHECK(r.settings.app == "dark");
    CHECK(r.settings.scrollback_lines == 5000);
    CHECK(r.settings.window_width == 1100);
    CHECK(r.settings.font == "Monospace 12");
    CHECK(r.warnings.size() == 4);
}

TEST_CASE("unreadable settings fall back to defaults") {
    auto r = parse_settings("app: [unclosed");
    CHECK(r.settings.app == "dark");
    CHECK(r.warnings.size() == 1);
    CHECK(parse_settings("").warnings.empty());
}

TEST_CASE("settings save and load") {
    std::string tmpl = (fs::temp_directory_path() / "tmssh-set-XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    REQUIRE(::mkdtemp(buf.data()) != nullptr);
    fs::path dir(buf.data());
    fs::path p = dir / "sub" / "settings.yaml";

    CHECK(load_settings(p.string()).warnings.empty()); // missing: defaults
    Settings s;
    s.terminal_theme = "doom";
    REQUIRE(save_settings(p.string(), s) == "");
    struct stat st {};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    CHECK(load_settings(p.string()).settings.terminal_theme == "doom");
    fs::remove_all(dir);
}
