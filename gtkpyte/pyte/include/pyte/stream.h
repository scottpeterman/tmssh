// include/pyte/stream.h
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "pyte/screen.h"

namespace pyte {

// Byte-stream parser. Feed it whatever came off the wire, in whatever chunk
// sizes it arrived in; parser state persists across calls, so a sequence split
// across two reads is handled correctly.
//
// The state machine follows the usual VT500 shape: Ground consumes text and C0
// controls, ESC opens the escape states, CSI collects parameters until a final
// byte, and the string states (OSC/DCS) swallow everything until a terminator.
class Stream {
public:
    // Called when the host queries the terminal and expects a reply on the
    // input channel -- Device Status Report, Device Attributes, and friends.
    // The emulator never writes anywhere itself; the owner wires this to the
    // SSH channel (or logs it, or drops it).
    //
    // Without a responder the queries are still parsed and recognised, they
    // just go unanswered. Some applications wait or retry when that happens.
    using Responder = std::function<void(const std::string &)>;

    // Called for every mode set or reset the stream sees, whether or not the
    // screen acts on it: (mode number, set, was DEC-private).
    //
    // Most modes are not the buffer's business -- cursor visibility, mouse
    // reporting, bracketed paste, and cursor-key mode all belong to whatever
    // is drawing and feeding the terminal. But they are the emulator's to
    // *observe*, because it is the only thing parsing the byte stream. This
    // reports them without acting on them; the modes Screen does handle are
    // reported too, so a renderer can watch those as well.
    //
    // Cursor-key mode (DECCKM, ?1) in particular has to reach the input side:
    // get it wrong and arrow keys work at a shell prompt and fail inside a
    // full-screen application.
    using ModeHandler = std::function<void(int, bool, bool)>;

    explicit Stream(Screen &screen);

    void set_responder(Responder responder) { responder_ = std::move(responder); }
    void set_mode_handler(ModeHandler handler) { mode_handler_ = std::move(handler); }

    // How many replies this stream has generated. Zero after a session that
    // contained queries means the responder was never wired up.
    std::size_t responses_sent() const { return responses_sent_; }

    // Reported by primary Device Attributes (CSI c). Defaults to what xterm
    // announces, since that is what TERM is usually set to.
    void set_primary_da(std::string reply) { primary_da_ = std::move(reply); }
    void set_secondary_da(std::string reply) { secondary_da_ = std::move(reply); }

    void feed(const std::string &data);
    void feed(const char *data, std::size_t len);

    // Drop any partial sequence and return to Ground. Use after a connection
    // resets so a truncated sequence can't corrupt what follows.
    void reset();

    // Bytes the parser recognised but does not implement, for porting triage:
    // a count of unhandled final bytes seen. Useful when diffing against the
    // reference implementation — a mismatch plus a nonzero count usually means
    // a missing dispatch rather than a wrong one.
    std::size_t unhandled_count() const { return unhandled_; }

private:
    enum class State {
        Ground,
        Escape,
        EscapeIntermediate,  // charset selection, e.g. ESC ( B
        CsiParam,
        OscString,
        DcsString,
    };

    void consume(unsigned char byte);
    void ground(unsigned char byte);
    void execute_c0(unsigned char byte);
    void escape_dispatch(unsigned char byte);
    void csi_dispatch(unsigned char final_byte);
    void dec_private_mode(bool set);
    void report_mode(int mode, bool set, bool private_mode);
    std::vector<int> normalised_sgr_params() const;
    int param(std::size_t index, int fallback) const;
    void respond(const std::string &reply);
    void device_status_report();
    void device_attributes();

    Screen &screen_;
    State state_ = State::Ground;

    // CSI accumulation
    std::vector<int> params_;
    bool param_pending_ = false;
    char private_marker_ = '\0';
    // True when this CSI used ':' as a separator anywhere. The colon form of
    // extended colour (ISO 8613-6) carries an extra colour-space slot that
    // the semicolon form does not, and that slot is what distinguishes them.
    bool param_colon_ = false;

    // UTF-8 accumulation for multi-byte code points split across feeds
    unsigned char utf8_buf_[4] = {0, 0, 0, 0};
    int utf8_len_ = 0;
    int utf8_want_ = 0;

    // String-state terminator tracking (ESC \ is two bytes)
    bool string_esc_pending_ = false;

    Responder responder_;
    ModeHandler mode_handler_;
    std::size_t responses_sent_ = 0;
    std::string primary_da_ = "\033[?1;2c";     // xterm: VT100 with AVO
    std::string secondary_da_ = "\033[>0;10;0c";  // xterm-ish identity

    std::size_t unhandled_ = 0;
};

}  // namespace pyte