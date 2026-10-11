// src/keymap.h
#pragma once

#include <gdk/gdk.h>

#include <string>

namespace gtkpyte {

// Cursor keys send different sequences depending on modes the application
// sets; the widget tracks them and passes them in.
struct KeyModes {
    // DECCKM (CSI ?1h): cursor keys send SS3 O rather than CSI [. Any
    // full-screen application reading arrows sets it -- get it wrong and
    // arrows work at the prompt and fail inside vim.
    bool application_cursor = false;
};

// Translate a GDK key press into the bytes a terminal sends.
//
// `keyval` is what GDK reports, so Shift is already applied ('A', not 'a';
// ISO_Left_Tab for Shift-Tab). Returns empty for a bare modifier or a key with
// no mapping, which callers must check rather than assume output.
std::string encode_key(guint keyval, GdkModifierType state, const KeyModes &modes);

// Bytes for pasted text: newlines become CR, the way Return is sent, and the
// whole thing is wrapped in bracketed-paste markers when the application asked
// for them (?2004) so an editor can tell paste from typing.
std::string encode_text(const std::string &utf8, bool bracketed_paste);

}  // namespace gtkpyte
