// src/stream.cpp
#include "pyte/stream.h"

#include <utf8proc.h>

namespace pyte {
namespace {

constexpr unsigned char kBEL = 0x07;
constexpr unsigned char kBS = 0x08;
constexpr unsigned char kHT = 0x09;
constexpr unsigned char kLF = 0x0A;
constexpr unsigned char kVT = 0x0B;
constexpr unsigned char kFF = 0x0C;
constexpr unsigned char kCR = 0x0D;
constexpr unsigned char kESC = 0x1B;
constexpr unsigned char kCAN = 0x18;
constexpr unsigned char kSUB = 0x1A;

// Expected total byte count for a UTF-8 sequence with this lead byte, or 0 if
// the byte cannot start one.
int utf8_sequence_length(unsigned char lead) {
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0;
}

}  // namespace

Stream::Stream(Screen &screen) : screen_(screen) {}

void Stream::reset() {
    // Parser state only. The responder and the reported identity are
    // configuration, not stream state, and survive a reset.
    state_ = State::Ground;
    params_.clear();
    param_pending_ = false;
    param_colon_ = false;
    private_marker_ = '\0';
    utf8_len_ = 0;
    utf8_want_ = 0;
    string_esc_pending_ = false;
}

void Stream::feed(const std::string &data) { feed(data.data(), data.size()); }

void Stream::feed(const char *data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
        consume(static_cast<unsigned char>(data[i]));
    }
}

int Stream::param(std::size_t index, int fallback) const {
    if (index >= params_.size()) {
        return fallback;
    }
    const int v = params_[index];
    return v == 0 ? fallback : v;
}

void Stream::respond(const std::string &reply) {
    if (!responder_) {
        return;  // recognised, but nobody is listening
    }
    ++responses_sent_;
    responder_(reply);
}

void Stream::device_status_report() {
    const int code = params_.empty() ? 0 : params_[0];
    if (code == 5) {
        // "Are you there?" -- 0n means ready, no malfunction.
        respond("\033[0n");
    } else if (code == 6) {
        // Cursor Position Report. Rows and columns on the wire are 1-based.
        std::string reply = "\033[";
        if (private_marker_ == '?') {
            reply = "\033[?";  // DECXCPR
        }
        reply += std::to_string(screen_.cursor_y() + 1);
        reply += ';';
        reply += std::to_string(screen_.cursor_x() + 1);
        reply += 'R';
        respond(reply);
    } else {
        ++unhandled_;
    }
}

void Stream::device_attributes() {
    if (private_marker_ == '>') {
        respond(secondary_da_);
    } else if (private_marker_ == '\0' || private_marker_ == '?') {
        respond(primary_da_);
    } else {
        ++unhandled_;
    }
}

void Stream::consume(unsigned char byte) {
    switch (state_) {
        case State::Ground:
            ground(byte);
            break;

        case State::Escape:
            if (byte == '[') {
                params_.clear();
                param_pending_ = false;
                private_marker_ = '\0';
                state_ = State::CsiParam;
            } else if (byte == ']') {
                string_esc_pending_ = false;
                state_ = State::OscString;
            } else if (byte == 'P' || byte == 'X' || byte == '^' || byte == '_') {
                string_esc_pending_ = false;
                state_ = State::DcsString;
            } else if (byte == '(' || byte == ')' || byte == '*' || byte == '+' ||
                       byte == '#' || byte == '%') {
                state_ = State::EscapeIntermediate;
            } else {
                escape_dispatch(byte);
                state_ = State::Ground;
            }
            break;

        case State::EscapeIntermediate:
            // Charset designation and similar: the payload byte is consumed and
            // ignored. This port assumes UTF-8 throughout.
            state_ = State::Ground;
            break;

        case State::CsiParam:
            if (byte >= '0' && byte <= '9') {
                if (!param_pending_) {
                    params_.push_back(0);
                    param_pending_ = true;
                }
                params_.back() = params_.back() * 10 + (byte - '0');
            } else if (byte == ';' || byte == ':') {
                // ':' introduces a sub-parameter. Without this branch it
                // matched nothing and was dropped, so "38:2::255:0:0" ran its
                // digits together into one enormous parameter rather than
                // producing a colour.
                if (byte == ':') {
                    param_colon_ = true;
                }
                if (!param_pending_) {
                    params_.push_back(0);
                }
                param_pending_ = false;
            } else if (byte == '?' || byte == '<' || byte == '=' || byte == '>') {
                private_marker_ = static_cast<char>(byte);
            } else if (byte >= 0x20 && byte <= 0x2F) {
                // Intermediate bytes; not used by anything dispatched here.
            } else if (byte >= 0x40 && byte <= 0x7E) {
                csi_dispatch(byte);
                state_ = State::Ground;
            } else if (byte == kCAN || byte == kSUB) {
                state_ = State::Ground;
            } else if (byte < 0x20) {
                execute_c0(byte);  // C0 controls act immediately, even mid-CSI
            }
            break;

        case State::OscString:
        case State::DcsString:
            if (string_esc_pending_) {
                string_esc_pending_ = false;
                state_ = State::Ground;  // ESC \ terminated the string
                if (byte != '\\') {
                    consume(byte);
                }
            } else if (byte == kBEL) {
                state_ = State::Ground;
            } else if (byte == kESC) {
                string_esc_pending_ = true;
            } else if (byte == kCAN || byte == kSUB) {
                state_ = State::Ground;
            }
            // Payload is discarded. Window titles and clipboard sequences do
            // not affect the screen buffer.
            break;
    }
}

void Stream::ground(unsigned char byte) {
    if (utf8_want_ > 0) {
        if ((byte & 0xC0) == 0x80) {
            utf8_buf_[utf8_len_++] = byte;
            if (utf8_len_ == utf8_want_) {
                utf8proc_int32_t cp = 0;
                const utf8proc_ssize_t n = utf8proc_iterate(
                    reinterpret_cast<const utf8proc_uint8_t *>(utf8_buf_), utf8_len_, &cp);
                utf8_len_ = 0;
                utf8_want_ = 0;
                if (n > 0 && cp >= 0) {
                    screen_.draw(static_cast<char32_t>(cp));
                } else {
                    screen_.draw(U'\uFFFD');
                }
            }
            return;
        }
        // Invalid continuation: emit a replacement and reprocess this byte.
        utf8_len_ = 0;
        utf8_want_ = 0;
        screen_.draw(U'\uFFFD');
    }

    if (byte == kESC) {
        state_ = State::Escape;
        return;
    }
    if (byte < 0x20) {
        execute_c0(byte);
        return;
    }
    if (byte < 0x7F) {
        screen_.draw(static_cast<char32_t>(byte));
        return;
    }
    if (byte == 0x7F) {
        return;  // DEL is ignored
    }

    const int want = utf8_sequence_length(byte);
    if (want == 0) {
        screen_.draw(U'\uFFFD');
        return;
    }
    utf8_buf_[0] = byte;
    utf8_len_ = 1;
    utf8_want_ = want;
}

void Stream::execute_c0(unsigned char byte) {
    switch (byte) {
        case kBS: screen_.backspace(); break;
        case kHT: screen_.tab(); break;
        case kLF:
        case kVT:
        case kFF: screen_.linefeed(); break;
        case kCR: screen_.carriage_return(); break;
        case kBEL: break;  // a renderer may want a signal here
        default: break;
    }
}

void Stream::escape_dispatch(unsigned char byte) {
    switch (byte) {
        case 'D': screen_.index(); break;
        case 'M': screen_.reverse_index(); break;
        case 'E':
            screen_.carriage_return();
            screen_.index();
            break;
        case '7': screen_.save_cursor(); break;
        case '8': screen_.restore_cursor(); break;
        case 'c': screen_.reset(); break;
        case 'Z': respond(primary_da_); break;  // DECID, obsolete form of CSI c
        case '=':
        case '>': break;  // keypad mode: no screen effect
        default: ++unhandled_; break;
    }
}

void Stream::csi_dispatch(unsigned char final_byte) {
    // A private marker makes it a different sequence that happens to share the
    // final byte. Vim sends CSI > 4;2 m (XTMODKEYS) on entry and CSI > 4;m on
    // exit -- after ?1049l -- which run as SGR would turn underline on for the
    // shell. Same trap for kitty keyboard (CSI > 1 u, CSI ? u, CSI < u),
    // XTSAVE/XTRESTORE (CSI ? s, CSI ? r) and XTQMODKEYS (CSI ? 4 m). None of
    // them touch the buffer, so they are consumed without effect.
    if (private_marker_ != '\0') {
        switch (final_byte) {
            case 'm':
            case 'r':
            case 's':
            case 'u':
                return;
            default:
                break;
        }
    }

    switch (final_byte) {
        case 'A': screen_.cursor_up(param(0, 1)); break;
        case 'B': screen_.cursor_down(param(0, 1)); break;
        case 'C': screen_.cursor_forward(param(0, 1)); break;
        case 'D': screen_.cursor_back(param(0, 1)); break;
        case 'E':
            screen_.cursor_down(param(0, 1));
            screen_.carriage_return();
            break;
        case 'F':
            screen_.cursor_up(param(0, 1));
            screen_.carriage_return();
            break;
        case 'G':
        case '`': screen_.cursor_to_column(param(0, 1)); break;
        case 'd': screen_.cursor_to_line(param(0, 1)); break;
        case 'H':
        case 'f': screen_.cursor_position(param(0, 1), param(1, 1)); break;
        case 'J': screen_.erase_in_display(params_.empty() ? 0 : params_[0]); break;
        case 'K': screen_.erase_in_line(params_.empty() ? 0 : params_[0]); break;
        case 'L': screen_.insert_lines(param(0, 1)); break;
        case 'M': screen_.delete_lines(param(0, 1)); break;
        case 'P': screen_.delete_characters(param(0, 1)); break;
        case 'X': screen_.erase_characters(param(0, 1)); break;
        case '@': screen_.insert_characters(param(0, 1)); break;
        case 'S': screen_.scroll_up(param(0, 1)); break;  // SU
        case 'T':
            // CSI Ps T is SD. The five-parameter form is xterm's highlight
            // mouse tracking, which shares the final byte and has nothing to
            // do with scrolling -- scrolling on it would shred the screen.
            if (params_.size() >= 5) {
                ++unhandled_;
            } else {
                screen_.scroll_down(param(0, 1));
            }
            break;
        case 'm':
            screen_.select_graphic_rendition(normalised_sgr_params());
            break;
        case 'n': device_status_report(); break;
        case 'c': device_attributes(); break;
        case 'r':
            if (params_.size() >= 2) {
                screen_.set_margins(param(0, 1), param(1, screen_.rows()));
            } else {
                screen_.reset_margins();
            }
            break;
        case 's': screen_.save_cursor(); break;
        case 'u': screen_.restore_cursor(); break;
        case 'h':
        case 'l':
            // ANSI (unprefixed) modes are recognised and ignored; none of the
            // ones applications actually send changes the buffer.
            if (private_marker_ == '?') {
                dec_private_mode(final_byte == 'h');
            } else {
                for (const int mode : params_) {
                    report_mode(mode, final_byte == 'h', false);
                }
            }
            break;
        default: ++unhandled_; break;
    }
}

std::vector<int> Stream::normalised_sgr_params() const {
    if (!param_colon_) {
        return params_;
    }
    // The colon form of direct colour is "38:2:<colour space>:r:g:b" -- one
    // slot longer than the semicolon form Screen expects. Drop that slot here
    // so the screen sees a single layout and never has to know which
    // separator the host chose.
    std::vector<int> out;
    out.reserve(params_.size());
    for (std::size_t i = 0; i < params_.size(); ++i) {
        const int p = params_[i];
        out.push_back(p);
        if ((p == 38 || p == 48) && i + 2 < params_.size() &&
            params_[i + 1] == 2 && params_.size() - i >= 6) {
            out.push_back(params_[i + 1]);  // the 2
            i += 2;                          // skip the colour-space slot
        }
    }
    return out;
}

void Stream::report_mode(int mode, bool set, bool private_mode) {
    if (mode_handler_) {
        mode_handler_(mode, set, private_mode);
    }
}

void Stream::dec_private_mode(bool set) {
    // Modes arrive in lists -- CSI ?1049;1000;2004h is one sequence, not
    // three -- so every parameter is dispatched, not just the first.
    for (const int mode : params_) {
        // Report first, unconditionally. A renderer watching cursor-key mode
        // or bracketed paste must see it whether or not the buffer cares.
        report_mode(mode, set, true);

        switch (mode) {
            case 7:  // DECAWM
                screen_.set_autowrap(set);
                break;

            case 47:
            case 1047:
                // Bare buffer switch. 1047 clears the alternate screen on the
                // way out, so a full-screen application does not leave its
                // last frame behind for the next one to inherit.
                if (!set && mode == 1047) {
                    screen_.erase_in_display(2);
                }
                screen_.set_alternate_screen(set);
                break;

            case 1048:
                // Save/restore cursor, explicitly defined as DECSC's slot --
                // so this shares storage with ESC 7 and CSI s, as on a real
                // terminal.
                if (set) {
                    screen_.save_cursor();
                } else {
                    screen_.restore_cursor();
                }
                break;

            case 1049:
                // The combination applications actually send: save the
                // cursor, switch, and start on a clean alternate screen;
                // on the way out, switch back and put the cursor where the
                // shell left it.
                if (set) {
                    screen_.save_cursor();
                    screen_.set_alternate_screen(true);
                    screen_.erase_in_display(2);
                } else {
                    screen_.set_alternate_screen(false);
                    screen_.restore_cursor();
                }
                break;

            default:
                // Cursor visibility, bracketed paste, mouse reporting, origin
                // mode and the rest: recognised, and deliberately not counted
                // as unhandled. They belong to a layer above the buffer.
                break;
        }
    }
}

}  // namespace pyte