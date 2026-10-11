// tests/test_themes.cpp -- colour parsing, theme files, the bundled pack and
// the user-override directory.
#include "themes.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace tmssh;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "tmssh-theme-XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        REQUIRE(::mkdtemp(buf.data()) != nullptr);
        path = buf.data();
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void write(const std::string &name, const std::string &text) const {
        std::ofstream(path / name) << text;
    }
};

Rgba rgb(int r, int g, int b, float a = 1.0f) {
    return Rgba{r / 255.0f, g / 255.0f, b / 255.0f, a};
}

std::string ansi_block(const std::string &value) {
    std::string s;
    for (const char *k : kAnsiKeys) s += std::string("  ") + k + ": \"" + value + "\"\n";
    return s;
}

// A complete theme with the given name, type and background.
std::string theme_yaml(const std::string &name, const std::string &type = "dark",
                       const std::string &bg = "#101010", const std::string &extra = "") {
    return "name: \"" + name + "\"\nlabel: \"" + name + " label\"\ntype: \"" + type +
           "\"\nterminal:\n  background: \"" + bg + "\"\n  foreground: \"#e0e0e0\"\n" +
           ansi_block("#808080") + extra;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Colours
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("hex colours") {
    CHECK(*parse_color("#ff8000") == rgb(255, 128, 0));
    CHECK(*parse_color("#FF8000") == rgb(255, 128, 0));
    CHECK(*parse_color("#f80") == rgb(255, 136, 0));
    CHECK(*parse_color(" #0078d433 ") == rgb(0, 120, 212, 0x33 / 255.0f));
}

TEST_CASE("rgb() and rgba()") {
    CHECK(*parse_color("rgb(0, 255, 0)") == rgb(0, 255, 0));
    CHECK(*parse_color("rgba(0, 255, 0, 0.3)") == rgb(0, 255, 0, 0.3f));
    CHECK(*parse_color("RGBA(0,255,0,1)") == rgb(0, 255, 0));
}

TEST_CASE("bad colours are rejected") {
    for (const char *bad : {"", "#", "#12", "#12345", "#1234567", "#gg0000", "ff0000",
                            "rgb(0,0)", "rgb(0,0,0,0)", "rgba(0,0,0)", "rgb(256,0,0)",
                            "rgba(0,0,0,1.5)", "rgb(a,0,0)", "rgb(0,0,0", "red"})
        CHECK_MESSAGE(!parse_color(bad).has_value(), bad);
}

// ─────────────────────────────────────────────────────────────────────
// Theme files
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("a complete theme parses") {
    auto r = parse_theme(theme_yaml("t1", "light", "#ffffff",
                                    "  cursor: \"#ff0000\"\n  selection: \"#0078d433\"\n"));
    REQUIRE(r.theme);
    const Theme &t = *r.theme;
    CHECK(t.name == "t1");
    CHECK(t.label == "t1 label");
    CHECK_FALSE(t.dark);
    CHECK(t.background == rgb(255, 255, 255));
    CHECK(t.cursor == rgb(255, 0, 0));
    // A selection that already has alpha keeps it.
    CHECK(t.selection == rgb(0, 120, 212, 0x33 / 255.0f));
    CHECK(t.ansi[15] == rgb(128, 128, 128));
}

TEST_CASE("opaque selection is given overlay alpha") {
    auto r = parse_theme(theme_yaml("t", "dark", "#000000", "  selection: \"#504945\"\n"));
    REQUIRE(r.theme);
    CHECK(r.theme->selection == rgb(0x50, 0x49, 0x45, kSelectionAlpha));
}

TEST_CASE("cursor and selection default from the foreground") {
    auto r = parse_theme(theme_yaml("t"));
    REQUIRE(r.theme);
    CHECK(r.theme->cursor == r.theme->foreground);
    Rgba sel = r.theme->foreground;
    sel.alpha = kSelectionAlpha;
    CHECK(r.theme->selection == sel);
}

TEST_CASE("label defaults to name, type defaults to dark") {
    auto r = parse_theme("name: x\nterminal:\n  background: '#000'\n  foreground: '#fff'\n" +
                         ansi_block("#888"));
    REQUIRE(r.theme);
    CHECK(r.theme->label == "x");
    CHECK(r.theme->dark);
}

TEST_CASE("a missing colour names every missing key") {
    auto r = parse_theme("name: t\nterminal:\n  background: '#000'\n  foreground: '#fff'\n");
    REQUIRE_FALSE(r.theme);
    CHECK(r.error.find("black") != std::string::npos);
    CHECK(r.error.find("bright_white") != std::string::npos);
}

TEST_CASE("a bad colour is named with its value") {
    std::string y = theme_yaml("t");
    y.replace(y.find("#101010"), 7, "#zzzzzz");
    auto r = parse_theme(y);
    REQUIRE_FALSE(r.theme);
    CHECK(r.error.find("background=\"#zzzzzz\"") != std::string::npos);
}

TEST_CASE("other theme errors") {
    CHECK_FALSE(parse_theme("").theme);
    CHECK_FALSE(parse_theme("- a list").theme);
    CHECK_FALSE(parse_theme("label: no name\n").theme);
    CHECK_FALSE(parse_theme("name: t\n").theme); // no terminal block
    CHECK_FALSE(parse_theme(theme_yaml("t", "sepia")).theme);
    CHECK_FALSE(parse_theme("name: [unclosed").theme);
}

TEST_CASE("a chrome block, which PathfinderSSH also ignores, is ignored") {
    auto r = parse_theme(theme_yaml("t", "dark", "#000000", "chrome:\n  window: \"#123456\"\n"));
    CHECK(r.theme.has_value());
}

// ─────────────────────────────────────────────────────────────────────
// The bundled pack
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("every bundled theme parses, with its file name as its name") {
    ThemeSet set;
    auto messages = set.load_dir(TMSSH_THEMES_DIR);
    for (const auto &m : messages) FAIL_CHECK(m);
    CHECK(set.size() == 30);

    for (const auto &e : fs::directory_iterator(TMSSH_THEMES_DIR)) {
        const std::string stem = e.path().stem().string();
        CHECK_MESSAGE(set.find(stem) != nullptr, stem);
    }

    const Theme *ice = set.find(kDefaultTheme);
    REQUIRE(ice != nullptr);
    CHECK_FALSE(ice->dark);

    // The one rgba() selection in the pack keeps its own alpha.
    const Theme *cc = set.find("cyber_custom");
    REQUIRE(cc != nullptr);
    CHECK(cc->selection.alpha < 1.0f);
}

TEST_CASE("bundled theme selections all carry alpha") {
    ThemeSet set;
    set.load_dir(TMSSH_THEMES_DIR);
    for (const Theme *t : set.sorted()) CHECK_MESSAGE(t->selection.alpha < 1.0f, t->name);
}

// ─────────────────────────────────────────────────────────────────────
// ThemeSet
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("a later theme with the same name replaces the earlier one") {
    ThemeSet set;
    set.add(*parse_theme(theme_yaml("ice", "light", "#ffffff")).theme);
    set.add(*parse_theme(theme_yaml("ice", "dark", "#000000")).theme);
    CHECK(set.size() == 1);
    CHECK(set.find("ice")->dark);
}

TEST_CASE("resolve falls back to the default, then to anything") {
    ThemeSet set;
    CHECK(set.resolve("x") == nullptr);
    set.add(*parse_theme(theme_yaml("zz")).theme);
    CHECK(set.resolve("missing")->name == "zz");
    set.add(*parse_theme(theme_yaml(kDefaultTheme, "light")).theme);
    CHECK(set.resolve("missing")->name == kDefaultTheme);
    CHECK(set.resolve("")->name == kDefaultTheme);
    CHECK(set.resolve("zz")->name == "zz");
}

TEST_CASE("sorted is by label, case-insensitive") {
    ThemeSet set;
    for (const char *n : {"b", "A", "c"}) set.add(*parse_theme(theme_yaml(n)).theme);
    auto s = set.sorted();
    REQUIRE(s.size() == 3);
    CHECK(s[0]->name == "A");
    CHECK(s[1]->name == "b");
    CHECK(s[2]->name == "c");
}

TEST_CASE("load_dir: user files override, bad files are reported and skipped") {
    ThemeSet set;
    set.add(*parse_theme(theme_yaml("ice", "light", "#ffffff")).theme);

    TempDir d;
    d.write("ice.yaml", theme_yaml("ice", "dark", "#000000"));
    d.write("mine.yml", theme_yaml("mine"));
    d.write("broken.yaml", "name: broken\n");
    d.write("notes.txt", "not a theme");

    auto messages = set.load_dir(d.path.string());
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].find("broken.yaml") != std::string::npos);
    CHECK(set.size() == 2);
    CHECK(set.find("ice")->dark);
    CHECK(set.find("mine") != nullptr);
}

TEST_CASE("load_dir on a missing directory is quiet") {
    ThemeSet set;
    CHECK(set.load_dir("/nonexistent/tmssh/themes").empty());
    CHECK(set.empty());
}

TEST_CASE("user themes dir honours XDG_CONFIG_HOME") {
    const char *old = std::getenv("XDG_CONFIG_HOME");
    std::string saved = old ? old : "";
    ::setenv("XDG_CONFIG_HOME", "/tmp/xdg-test", 1);
    CHECK(user_themes_dir() == "/tmp/xdg-test/tmssh/themes");
    if (old)
        ::setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
    else
        ::unsetenv("XDG_CONFIG_HOME");
}
