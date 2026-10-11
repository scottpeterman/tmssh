// src/themes.cpp -- see themes.h.
#include "themes.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace tmssh {

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

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Strictly a number and nothing else, after trimming.
std::optional<double> number(std::string_view s) {
    std::string t = trim(s);
    if (t.empty()) return std::nullopt;
    char *end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::optional<Rgba> parse_hex(std::string_view h) {
    // h excludes the '#'
    std::vector<int> d;
    for (char c : h) {
        int v = hex_digit(c);
        if (v < 0) return std::nullopt;
        d.push_back(v);
    }
    auto byte = [](int hi, int lo) { return static_cast<float>(hi * 16 + lo) / 255.0f; };
    Rgba c;
    switch (d.size()) {
    case 3:
        c.red = byte(d[0], d[0]);
        c.green = byte(d[1], d[1]);
        c.blue = byte(d[2], d[2]);
        return c;
    case 6:
    case 8:
        c.red = byte(d[0], d[1]);
        c.green = byte(d[2], d[3]);
        c.blue = byte(d[4], d[5]);
        if (d.size() == 8) c.alpha = byte(d[6], d[7]);
        return c;
    default:
        return std::nullopt;
    }
}

// rgb(r, g, b) / rgba(r, g, b, a): r, g, b in 0..255, a in 0..1.
std::optional<Rgba> parse_func(std::string_view s) {
    std::string t = lower(s);
    bool has_alpha;
    std::string_view body;
    if (t.rfind("rgba(", 0) == 0) {
        has_alpha = true;
        body = std::string_view(t).substr(5);
    } else if (t.rfind("rgb(", 0) == 0) {
        has_alpha = false;
        body = std::string_view(t).substr(4);
    } else {
        return std::nullopt;
    }
    if (body.empty() || body.back() != ')') return std::nullopt;
    body.remove_suffix(1);

    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= body.size(); ++i) {
        if (i == body.size() || body[i] == ',') {
            parts.push_back(body.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() != (has_alpha ? 4u : 3u)) return std::nullopt;

    float ch[3];
    for (int i = 0; i < 3; ++i) {
        auto v = number(parts[i]);
        if (!v || *v < 0 || *v > 255) return std::nullopt;
        ch[i] = static_cast<float>(*v / 255.0);
    }
    Rgba c{ch[0], ch[1], ch[2], 1.0f};
    if (has_alpha) {
        auto a = number(parts[3]);
        if (!a || *a < 0 || *a > 1) return std::nullopt;
        c.alpha = static_cast<float>(*a);
    }
    return c;
}

} // namespace

bool operator==(const Rgba &a, const Rgba &b) {
    auto eq = [](float x, float y) { return std::fabs(x - y) < 1e-6f; };
    return eq(a.red, b.red) && eq(a.green, b.green) && eq(a.blue, b.blue) && eq(a.alpha, b.alpha);
}

std::optional<Rgba> parse_color(std::string_view text) {
    std::string t = trim(text);
    if (t.empty()) return std::nullopt;
    if (t[0] == '#') return parse_hex(std::string_view(t).substr(1));
    return parse_func(t);
}

const std::array<const char *, 16> kAnsiKeys = {
    "black",         "red",         "green",          "yellow",       "blue",
    "magenta",       "cyan",        "white",          "bright_black", "bright_red",
    "bright_green",  "bright_yellow", "bright_blue",  "bright_magenta", "bright_cyan",
    "bright_white",
};

ThemeParse parse_theme(std::string_view yaml) {
    ThemeParse out;
    YAML::Node root;
    try {
        root = YAML::Load(std::string(yaml));
    } catch (const YAML::Exception &e) {
        out.error = std::string("parse theme: ") + e.what();
        return out;
    }
    if (!root.IsMap()) {
        out.error = "parse theme: not a mapping";
        return out;
    }

    auto scalar = [](const YAML::Node &m, const char *key) -> std::string {
        YAML::Node v = m[key];
        return (v && v.IsScalar()) ? trim(v.Scalar()) : std::string{};
    };

    Theme t;
    t.name = scalar(root, "name");
    if (t.name.empty()) {
        out.error = "theme has no name";
        return out;
    }
    t.label = scalar(root, "label");
    if (t.label.empty()) t.label = t.name;

    std::string type = lower(scalar(root, "type"));
    if (type.empty() || type == "dark")
        t.dark = true;
    else if (type == "light")
        t.dark = false;
    else {
        out.error = t.name + ": type must be dark or light, not \"" + type + "\"";
        return out;
    }

    YAML::Node term = root["terminal"];
    if (!term || !term.IsMap()) {
        out.error = t.name + ": no terminal block";
        return out;
    }

    std::string missing;
    std::string bad;
    auto color = [&](const char *key, Rgba &dst, bool required) {
        std::string raw = scalar(term, key);
        if (raw.empty()) {
            if (required) missing += std::string(missing.empty() ? "" : ", ") + key;
            return false;
        }
        auto c = parse_color(raw);
        if (!c) {
            bad += std::string(bad.empty() ? "" : ", ") + key + "=\"" + raw + "\"";
            return false;
        }
        dst = *c;
        return true;
    };

    color("background", t.background, true);
    color("foreground", t.foreground, true);
    for (std::size_t i = 0; i < kAnsiKeys.size(); ++i) color(kAnsiKeys[i], t.ansi[i], true);
    const bool have_cursor = color("cursor", t.cursor, false);
    const bool have_selection = color("selection", t.selection, false);

    if (!missing.empty() || !bad.empty()) {
        out.error = t.name + ":";
        if (!missing.empty()) out.error += " missing " + missing;
        if (!missing.empty() && !bad.empty()) out.error += ";";
        if (!bad.empty()) out.error += " bad colour " + bad;
        return out;
    }

    if (!have_cursor) t.cursor = t.foreground;
    if (!have_selection) {
        t.selection = t.foreground;
        t.selection.alpha = kSelectionAlpha;
    } else if (t.selection.alpha >= 1.0f) {
        t.selection.alpha = kSelectionAlpha;
    }

    out.theme = std::move(t);
    return out;
}

void ThemeSet::add(Theme theme) {
    for (auto &t : themes_) {
        if (t.name == theme.name) {
            t = std::move(theme);
            return;
        }
    }
    themes_.push_back(std::move(theme));
}

const Theme *ThemeSet::find(std::string_view name) const {
    for (const auto &t : themes_)
        if (t.name == name) return &t;
    return nullptr;
}

const Theme *ThemeSet::resolve(std::string_view name) const {
    if (!name.empty())
        if (const Theme *t = find(name)) return t;
    if (const Theme *t = find(kDefaultTheme)) return t;
    return themes_.empty() ? nullptr : &themes_.front();
}

std::vector<const Theme *> ThemeSet::sorted() const {
    std::vector<const Theme *> out;
    for (const auto &t : themes_) out.push_back(&t);
    std::stable_sort(out.begin(), out.end(), [](const Theme *a, const Theme *b) {
        return lower(a->label) < lower(b->label);
    });
    return out;
}

std::vector<std::string> ThemeSet::load_dir(const std::string &dir) {
    std::vector<std::string> messages;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return messages;

    std::vector<fs::path> files;
    for (const auto &e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string ext = lower(e.path().extension().string());
        if (ext == ".yaml" || ext == ".yml") files.push_back(e.path());
    }
    // Directory order is arbitrary; sorting makes "last one wins" for two
    // files declaring the same name predictable.
    std::sort(files.begin(), files.end());

    for (const auto &p : files) {
        std::ifstream in(p, std::ios::binary);
        if (!in) {
            messages.push_back(p.string() + ": cannot read");
            continue;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        ThemeParse r = parse_theme(ss.str());
        if (!r.theme) {
            messages.push_back(p.filename().string() + ": " + r.error);
            continue;
        }
        add(std::move(*r.theme));
    }
    return messages;
}

std::string user_themes_dir() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg == '/') return (fs::path(xdg) / "tmssh" / "themes").string();
    const char *home = std::getenv("HOME");
    fs::path base = (home && *home) ? fs::path(home) : fs::path(".");
    return (base / ".config" / "tmssh" / "themes").string();
}

} // namespace tmssh
