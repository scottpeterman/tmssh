// tests/test_settings.cpp -- settings.yaml parsing and round trip.
#include "settings.h"

#include <doctest/doctest.h>

using namespace tmssh;

TEST_CASE("defaults for the logging and zoom keys") {
    const Settings s = parse_settings("").settings;
    CHECK(s.font_zoom == 0);
    CHECK(s.log_dir == "~/tmssh-logs");
    CHECK_FALSE(s.auto_log);
}

TEST_CASE("logging and zoom keys parse") {
    const SettingsLoad l = parse_settings("font_zoom: -3\nlog_dir: /srv/logs\nauto_log: true\n");
    CHECK(l.warnings.empty());
    CHECK(l.settings.font_zoom == -3);
    CHECK(l.settings.log_dir == "/srv/logs");
    CHECK(l.settings.auto_log);
}

TEST_CASE("bad values are ignored with a warning") {
    const SettingsLoad l = parse_settings("font_zoom: 500\nauto_log: maybe\n");
    CHECK(l.warnings.size() == 2);
    CHECK(l.settings.font_zoom == 0);
    CHECK_FALSE(l.settings.auto_log);
}

TEST_CASE("emit and parse round trip") {
    Settings s;
    s.font_zoom = 4;
    s.log_dir = "~/work logs";
    s.auto_log = true;
    const SettingsLoad back = parse_settings(emit_settings(s));
    CHECK(back.warnings.empty());
    CHECK(back.settings.font_zoom == 4);
    CHECK(back.settings.log_dir == "~/work logs");
    CHECK(back.settings.auto_log);
}
