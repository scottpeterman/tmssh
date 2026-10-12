// tests/test_session_log.cpp -- the byte-stream-to-text cleaner, log files and names.
#include "session_log.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace tmssh;

namespace {

std::string clean(const std::string &raw) {
    LogCleaner c;
    return c.feed(raw.data(), raw.size());
}

// Feed one byte at a time: every sequence split at every point.
std::string clean_bytewise(const std::string &raw) {
    LogCleaner c;
    std::string out;
    for (char ch : raw) out += c.feed(&ch, 1);
    return out;
}

std::string slurp(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "tmssh-log-XXXXXX").string();
        path = ::mkdtemp(tmpl.data());
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("plain lines pass through, CRLF becomes LF") {
    CHECK(clean("hello\r\nworld\r\n") == "hello\nworld\n");
}

TEST_CASE("colours, titles, modes and charset switches leave no trace") {
    CHECK(clean("\x1b[1;32mOK\x1b[0m done\r\n") == "OK done\n");
    CHECK(clean("\x1b]0;user@host: ~\x07$ ls\r\n") == "$ ls\n");
    CHECK(clean("\x1b]2;title\x1b\\x\r\n") == "x\n");
    CHECK(clean("\x1b[?2004h\x1b(Bprompt#\x1b[?2004l\r\n") == "prompt#\n");
    CHECK(clean("\x1bPq#0;2;0;0;0\x1b\\after\r\n") == "after\n");
}

TEST_CASE("trailing spaces are trimmed") {
    CHECK(clean("interface Gi0/1     \r\n") == "interface Gi0/1\n");
}

TEST_CASE("IOS --More-- erased with backspaces and spaces") {
    const std::string prompt = " --More-- ";
    const std::string bs(prompt.size(), '\b');
    const std::string sp(prompt.size(), ' ');
    const std::string raw = "line1\r\n" + prompt + bs + sp + bs + "line2\r\n";
    CHECK(clean(raw) == "line1\nline2\n");
}

TEST_CASE("Junos ---(more)--- erased with CR and erase-line") {
    CHECK(clean("---(more 35%)---\r\x1b[Knext line\r\n") == "next line\n");
    CHECK(clean("---(more)---\r                    \rnext\r\n") == "next\n");
}

TEST_CASE("readline edits are applied") {
    // Typed "shox", rubbed out the x, typed "w ip".
    CHECK(clean("router#shox\b \bw ip\r\n") == "router#show ip\n");
    CHECK(clean("router#shox\b\x1b[Kw ip\r\n") == "router#show ip\n");
    // Cursor left three, then overtype.
    CHECK(clean("abcdef\x1b[3Dxyz\r\n") == "abcxyz\n");
    // Delete one character mid-line.
    CHECK(clean("abXcd\x1b[3D\x1b[P\r\n") == "abcd\n");
    // Insert a blank mid-line, then fill it.
    CHECK(clean("abd\x1b[1D\x1b[@c\r\n") == "abcd\n");
    // Cursor to column, erase to start of line.
    CHECK(clean("xxxxx\x1b[3G\x1b[1Kok\r\n") == "  okx\n");
}

TEST_CASE("CR overwrite keeps the last value of a counter") {
    CHECK(clean("10%\r20%\r100%\r\n") == "100%\n");
}

TEST_CASE("tabs expand to the next multiple of eight") {
    CHECK(clean("a\tb\r\n") == "a       b\n");
}

TEST_CASE("clear screen ends the line being written") {
    CHECK(clean("before\x1b[H\x1b[2Jafter\r\n") == "before\nafter\n");
}

TEST_CASE("sequences and UTF-8 split across feeds") {
    const std::string raw = "caf\xc3\xa9 \x1b[1;31mred\x1b[0m\r\nok\r\n";
    CHECK(clean_bytewise(raw) == clean(raw));
    CHECK(clean(raw) == "caf\xc3\xa9 red\nok\n");
}

TEST_CASE("broken UTF-8 becomes U+FFFD, not a lost line") {
    CHECK(clean("a\xc3z\r\n") == "a\xef\xbf\xbdz\n");
}

TEST_CASE("an unfinished line is held until flushed") {
    LogCleaner c;
    const std::string prompt = "router#";
    CHECK(c.feed(prompt.data(), prompt.size()).empty());
    CHECK(c.flush() == "router#");
    CHECK(c.flush().empty());
}

TEST_CASE("log names") {
    CHECK(log_name_part("spine-1 (lab)") == "spine-1_lab");
    CHECK(log_name_part("a/b") == "a_b");
    CHECK(log_name_part("..hidden") == "hidden");
    CHECK(log_name_part("") == "session");
    CHECK(log_name_part("///") == "session");
    const std::string name = log_file_name("edge1", 0);
    CHECK(name.rfind("edge1_", 0) == 0);
    CHECK(name.size() == std::string("edge1_YYYYmmdd-HHMMSS.log").size());
}

TEST_CASE("expand_log_dir") {
    const char *home = std::getenv("HOME");
    REQUIRE(home != nullptr);
    CHECK(expand_log_dir("~/logs") == (fs::path(home) / "logs").string());
    CHECK(expand_log_dir("") == (fs::path(home) / "tmssh-logs").string());
    CHECK(expand_log_dir("/var/tmp/x") == "/var/tmp/x");
}

TEST_CASE("a session log: header, cleaned output, notes, footer, mode 0600") {
    TempDir tmp;
    const fs::path dir = tmp.path / "logs";
    std::string path;
    {
        SessionLog log;
        REQUIRE(log.create(dir.string(), "edge1", "admin@10.0.0.1", 0));
        path = log.path();
        const std::string out = "\x1b[1mshow ver\x1b[0m\r\nJunos: 23.4R1\r\nadmin@edge1> ";
        CHECK(log.write(out.data(), out.size()));
        CHECK(log.note("reconnected"));
    } // destructor closes

    const std::string text = slurp(path);
    CHECK(text.find("# tmssh session log: edge1\n") == 0);
    CHECK(text.find("# target: admin@10.0.0.1\n") != std::string::npos);
    CHECK(text.find("show ver\nJunos: 23.4R1\nadmin@edge1>\n# tmssh: reconnected\n") !=
          std::string::npos);
    CHECK(text.find("# tmssh: log closed ") != std::string::npos);

    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    REQUIRE(::stat(dir.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
}

TEST_CASE("two logs in the same second never share a file") {
    TempDir tmp;
    SessionLog a, b;
    REQUIRE(a.create(tmp.path.string(), "edge1", "", 0));
    REQUIRE(b.create(tmp.path.string(), "edge1", "", 0));
    CHECK(a.path() != b.path());
    CHECK(b.path().find("-2.log") != std::string::npos);
}

TEST_CASE("an uncreatable directory is an error, not a crash") {
    SessionLog log;
    CHECK_FALSE(log.create("/proc/no-such/dir", "x", "", 0));
    CHECK_FALSE(log.error().empty());
    CHECK_FALSE(log.is_open());
    CHECK_FALSE(log.write("x", 1));
}
