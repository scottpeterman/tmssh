// src/mouse.h
//
// xterm mouse reporting: what an application asked for, and the bytes that
// tell it about a press, release, drag or wheel step.
#pragma once

#include <string>

namespace gtkpyte {

// What the application has requested with DECSET. Tracking is the highest of
// 9/1000/1002/1003 currently set; the encoding is orthogonal to it.
struct MouseModes {
    enum Tracking { kOff = 0, kX10 = 9, kNormal = 1000, kButtonMotion = 1002, kAnyMotion = 1003 };
    Tracking tracking = kOff;
    bool sgr = false;  // ?1006: CSI < b ; x ; y M/m, no coordinate limit
    // ?1007: on the alternate screen with tracking off, the wheel sends
    // cursor keys. On by default, as in xterm's alternateScroll and VTE, so
    // less, man and a mouse-less vim scroll at all.
    bool alternate_scroll = true;

    bool reporting() const { return tracking != kOff; }

    // Apply one DECSET/DECRST. Returns true when the mode was a mouse mode.
    bool apply(int mode, bool set);
};

// Button numbers as xterm encodes them.
enum MouseButton { kMouseLeft = 0, kMouseMiddle = 1, kMouseRight = 2, kWheelUp = 64, kWheelDown = 65 };

enum class MouseAction { Press, Release, Motion };

struct MouseMods {
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
};

// Bytes for one event at a 0-based grid cell, or empty when nothing should be
// sent: the mode does not report this kind of event, or the classic encoding
// cannot represent the position (column or row past 222).
//
// For Motion, `button` is the held button, or -1 for a motion with nothing
// held (only reported in any-motion mode).
std::string encode_mouse(const MouseModes &modes, MouseAction action, int button, int col,
                         int row, MouseMods mods);

}  // namespace gtkpyte
