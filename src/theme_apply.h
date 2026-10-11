// src/theme_apply.h -- themes onto widgets, and the application dark/light.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtkpyte-terminal.h"
#include "themes.h"

namespace tmssh {

enum class AppVariant { Dark, Light };

// "dark" / "light"; nullopt for anything else.
std::optional<AppVariant> parse_app_variant(std::string_view s);
const char *to_string(AppVariant v);

// Sets the window chrome for the whole application. Independent of any
// terminal theme.
void set_app_variant(AppVariant v);

// Puts a theme's colours on one terminal.
void apply_terminal_theme(GtkpyteTerminal *terminal, const Theme &theme);

// The bundled theme pack (compiled in as a GResource), with the user's
// themes directory layered over it: a user file with a bundled name replaces
// that theme. Anything that failed to load is appended to messages.
ThemeSet load_themes(std::vector<std::string> &messages);

} // namespace tmssh
