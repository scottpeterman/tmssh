// src/keymap.cpp
#include "keymap.h"

namespace gtkpyte {
namespace {

// xterm modifier parameter: 1 + bitmask. Shift 1, Alt 2, Control 4, Meta 8.
// 1 means unmodified, and the parameter is then left off.
int modifier_param(GdkModifierType state) {
    int mask = 0;
    if (state & GDK_SHIFT_MASK) mask |= 1;
    if (state & GDK_ALT_MASK) mask |= 2;
    if (state & GDK_CONTROL_MASK) mask |= 4;
    if (state & GDK_META_MASK) mask |= 8;
    return mask + 1;
}

// Arrows, Home, End: CSI or SS3 with a letter final.
std::string csi_letter(char final_byte, int mod, bool application_cursor) {
    if (mod > 1) {
        // SS3 carries no parameters, so a modified cursor key is always CSI.
        return std::string("\033[1;") + std::to_string(mod) + final_byte;
    }
    if (application_cursor) {
        return std::string("\033O") + final_byte;
    }
    return std::string("\033[") + final_byte;
}

// Insert, Delete, PageUp/Down, F5 and up: CSI number ~.
std::string csi_tilde(int number, int mod) {
    std::string out = "\033[" + std::to_string(number);
    if (mod > 1) {
        out += ';';
        out += std::to_string(mod);
    }
    out += '~';
    return out;
}

// Control characters from a Ctrl combination. Ctrl-[ is ESC, which vim users
// press constantly, so the punctuation cases matter as much as the letters.
std::string control_char(guint keyval) {
    if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_z) {
        return std::string(1, static_cast<char>(keyval - GDK_KEY_a + 1));
    }
    if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z) {
        return std::string(1, static_cast<char>(keyval - GDK_KEY_A + 1));
    }
    switch (keyval) {
        case GDK_KEY_space:
        case GDK_KEY_2:
        case GDK_KEY_at:           return std::string(1, '\0');
        case GDK_KEY_bracketleft:  return "\033";
        case GDK_KEY_backslash:    return std::string(1, '\034');
        case GDK_KEY_bracketright: return std::string(1, '\035');
        case GDK_KEY_asciicircum:
        case GDK_KEY_6:            return std::string(1, '\036');
        case GDK_KEY_underscore:
        case GDK_KEY_minus:        return std::string(1, '\037');
        case GDK_KEY_question:     return std::string(1, '\177');
        default:                   return std::string();
    }
}

bool is_modifier_key(guint keyval) {
    switch (keyval) {
        case GDK_KEY_Shift_L:
        case GDK_KEY_Shift_R:
        case GDK_KEY_Control_L:
        case GDK_KEY_Control_R:
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R:
        case GDK_KEY_Meta_L:
        case GDK_KEY_Meta_R:
        case GDK_KEY_Super_L:
        case GDK_KEY_Super_R:
        case GDK_KEY_Hyper_L:
        case GDK_KEY_Hyper_R:
        case GDK_KEY_ISO_Level3_Shift:
        case GDK_KEY_ISO_Level5_Shift:
        case GDK_KEY_Caps_Lock:
        case GDK_KEY_Num_Lock:
        case GDK_KEY_Scroll_Lock:
            return true;
        default:
            return false;
    }
}

}  // namespace

std::string encode_key(guint keyval, GdkModifierType state, const KeyModes &modes) {
    if (is_modifier_key(keyval)) {
        return std::string();
    }

    const int mod = modifier_param(state);
    const bool app = modes.application_cursor;

    switch (keyval) {
        case GDK_KEY_Up:
        case GDK_KEY_KP_Up:       return csi_letter('A', mod, app);
        case GDK_KEY_Down:
        case GDK_KEY_KP_Down:     return csi_letter('B', mod, app);
        case GDK_KEY_Right:
        case GDK_KEY_KP_Right:    return csi_letter('C', mod, app);
        case GDK_KEY_Left:
        case GDK_KEY_KP_Left:     return csi_letter('D', mod, app);
        case GDK_KEY_Home:
        case GDK_KEY_KP_Home:     return csi_letter('H', mod, app);
        case GDK_KEY_End:
        case GDK_KEY_KP_End:      return csi_letter('F', mod, app);

        case GDK_KEY_Insert:
        case GDK_KEY_KP_Insert:   return csi_tilde(2, mod);
        case GDK_KEY_Delete:
        case GDK_KEY_KP_Delete:   return csi_tilde(3, mod);
        case GDK_KEY_Page_Up:
        case GDK_KEY_KP_Page_Up:  return csi_tilde(5, mod);
        case GDK_KEY_Page_Down:
        case GDK_KEY_KP_Page_Down: return csi_tilde(6, mod);

        case GDK_KEY_F1:
        case GDK_KEY_F2:
        case GDK_KEY_F3:
        case GDK_KEY_F4: {
            const char final_byte = static_cast<char>('P' + (keyval - GDK_KEY_F1));
            return (mod > 1) ? std::string("\033[1;") + std::to_string(mod) + final_byte
                             : std::string("\033O") + final_byte;
        }
        case GDK_KEY_F5:  return csi_tilde(15, mod);
        case GDK_KEY_F6:  return csi_tilde(17, mod);
        case GDK_KEY_F7:  return csi_tilde(18, mod);
        case GDK_KEY_F8:  return csi_tilde(19, mod);
        case GDK_KEY_F9:  return csi_tilde(20, mod);
        case GDK_KEY_F10: return csi_tilde(21, mod);
        case GDK_KEY_F11: return csi_tilde(23, mod);
        case GDK_KEY_F12: return csi_tilde(24, mod);

        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_ISO_Enter:
            // CR: the line discipline turns it into a newline.
            return (state & GDK_ALT_MASK) ? "\033\r" : "\r";

        case GDK_KEY_BackSpace:
            // DEL, matching VERASE on the pty; BS is the "prints ^H" bug.
            // Ctrl-Backspace sends BS, which readline binds to word-rubout.
            if (state & GDK_CONTROL_MASK) return std::string(1, '\010');
            return (state & GDK_ALT_MASK) ? "\033\177" : "\177";

        case GDK_KEY_Tab:
        case GDK_KEY_KP_Tab:
            return (state & GDK_SHIFT_MASK) ? "\033[Z" : "\t";
        case GDK_KEY_ISO_Left_Tab:
            return "\033[Z";

        case GDK_KEY_Escape:
            return "\033";

        default:
            break;
    }

    std::string out;
    if (state & GDK_ALT_MASK) {
        // Alt-x is ESC x -- what readline, vim and tmux expect.
        out += '\033';
    }

    if (state & GDK_CONTROL_MASK) {
        const std::string ctrl = control_char(keyval);
        if (!ctrl.empty()) {
            return out + ctrl;
        }
    }

    const gunichar uc = gdk_keyval_to_unicode(keyval);
    if (uc == 0) {
        return std::string();
    }
    char buf[6];
    const int n = g_unichar_to_utf8(uc, buf);
    out.append(buf, static_cast<std::size_t>(n));
    return out;
}

std::string encode_text(const std::string &utf8, bool bracketed_paste) {
    std::string body;
    body.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const char c = utf8[i];
        if (c == '\r') {
            if (i + 1 < utf8.size() && utf8[i + 1] == '\n') {
                ++i;
            }
            body += '\r';
        } else if (c == '\n') {
            body += '\r';
        } else {
            body += c;
        }
    }
    if (!bracketed_paste) {
        return body;
    }
    return "\033[200~" + body + "\033[201~";
}

}  // namespace gtkpyte
