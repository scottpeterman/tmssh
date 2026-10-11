// src/settings.h -- application settings, as data.
//
// ~/.config/tmssh/settings.yaml:
//
//   app: dark                 # window chrome: dark or light
//   terminal_theme: ice       # default palette; a session's own theme wins
//   font: Monospace 12        # Pango font description
//   scrollback_lines: 5000
//   window_width: 1100
//   window_height: 700
//   sidebar_width: 260
//
// Every key is optional; a missing or unusable value takes its default, so a
// hand-edited file can never stop the application starting.
#pragma once

#include <string>
#include <vector>

namespace tmssh {

struct Settings {
    std::string app = "dark";
    std::string terminal_theme = "ice";
    std::string font = "Monospace 12";
    int scrollback_lines = 5000;
    int window_width = 1100;
    int window_height = 700;
    int sidebar_width = 260;
};

struct SettingsLoad {
    Settings settings;
    std::vector<std::string> warnings; // values that were ignored, and why
};

SettingsLoad parse_settings(const std::string &yaml);
SettingsLoad load_settings(const std::string &path); // missing file: defaults, no warnings

std::string emit_settings(const Settings &s);
[[nodiscard]] std::string save_settings(const std::string &path, const Settings &s);

// $XDG_CONFIG_HOME/tmssh/settings.yaml, else ~/.config/tmssh/settings.yaml.
std::string default_settings_path();

} // namespace tmssh
