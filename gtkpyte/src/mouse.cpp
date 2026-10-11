// src/mouse.cpp
#include "mouse.h"

namespace gtkpyte {
namespace {

// Classic encoding packs each value as a single byte offset by 32, so a
// coordinate (1-based) above 223 has no representation. xterm drops those
// events rather than sending a wrapped value, and so does this.
constexpr int kClassicMaxCoord = 223;

int modifier_bits(MouseMods mods) {
    int bits = 0;
    if (mods.shift) bits |= 4;
    if (mods.alt) bits |= 8;
    if (mods.ctrl) bits |= 16;
    return bits;
}

bool is_wheel(int button) { return button == kWheelUp || button == kWheelDown; }

}  // namespace

bool MouseModes::apply(int mode, bool set) {
    switch (mode) {
        case kX10:
        case kNormal:
        case kButtonMotion:
        case kAnyMotion:
            if (set) {
                tracking = static_cast<Tracking>(mode);
            } else if (tracking == mode) {
                tracking = kOff;
            }
            return true;
        case 1006:
            sgr = set;
            return true;
        case 1007:
            alternate_scroll = set;
            return true;
        default:
            return false;
    }
}

std::string encode_mouse(const MouseModes &modes, MouseAction action, int button, int col,
                         int row, MouseMods mods) {
    if (!modes.reporting() || col < 0 || row < 0) {
        return std::string();
    }

    // Which events this tracking mode reports at all.
    switch (action) {
        case MouseAction::Press:
            break;
        case MouseAction::Release:
            // X10 reports presses only, and a wheel step has no release.
            if (modes.tracking == MouseModes::kX10 || is_wheel(button)) {
                return std::string();
            }
            break;
        case MouseAction::Motion:
            if (modes.tracking == MouseModes::kAnyMotion) {
                break;
            }
            if (modes.tracking == MouseModes::kButtonMotion && button >= 0) {
                break;
            }
            return std::string();
    }

    int code = 0;
    if (action == MouseAction::Motion) {
        code = (button >= 0 ? button : 3) + 32;
    } else if (action == MouseAction::Release && !modes.sgr) {
        code = 3;  // classic encoding cannot say which button was released
    } else {
        code = button;
    }
    // X10 mode carries no modifiers.
    if (modes.tracking != MouseModes::kX10) {
        code |= modifier_bits(mods);
    }

    const int x = col + 1;
    const int y = row + 1;

    if (modes.sgr) {
        return "\033[<" + std::to_string(code) + ";" + std::to_string(x) + ";" +
               std::to_string(y) + (action == MouseAction::Release ? "m" : "M");
    }

    if (x > kClassicMaxCoord || y > kClassicMaxCoord) {
        return std::string();
    }
    std::string out = "\033[M";
    out += static_cast<char>(32 + code);
    out += static_cast<char>(32 + x);
    out += static_cast<char>(32 + y);
    return out;
}

}  // namespace gtkpyte
