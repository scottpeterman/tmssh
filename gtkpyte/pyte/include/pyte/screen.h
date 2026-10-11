// include/pyte/screen.h
#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "pyte/cell.h"
#include "pyte/rowstore.h"

namespace pyte {

// Rows of scrollback a Screen retains unless told otherwise.
inline constexpr int kDefaultScrollback = 1000;

// The screen buffer and every operation that mutates it.
//
// Screen knows nothing about escape sequences -- Stream parses those and calls
// these methods. Coordinates on the public API are 0-based (x = column,
// y = row) except cursor_position(), which takes the 1-based row/column that
// CSI sequences carry.
//
// The cells live in a RowStore; the screen is a window into it. Rows that
// scroll off the top are retained as scrollback, and the read API (at(),
// line_text(), dump()) shows whatever the window is currently pointed at.
// Anything that writes cells snaps the window back to the live screen first,
// so an application can never paint into history.
class Screen {
public:
    explicit Screen(int cols, int rows, int max_scrollback = kDefaultScrollback);

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int cursor_x() const { return cursor_x_; }
    int cursor_y() const { return cursor_y_; }

    const Cell &at(int x, int y) const;

    void resize(int cols, int rows);
    void reset();

    // --- writing -----------------------------------------------------------
    void draw(char32_t ch);

    // --- C0 controls -------------------------------------------------------
    void carriage_return();
    void linefeed();
    void backspace();
    void tab();

    // --- cursor movement ---------------------------------------------------
    void cursor_up(int n = 1);
    void cursor_down(int n = 1);
    void cursor_forward(int n = 1);
    void cursor_back(int n = 1);
    void cursor_position(int row = 1, int col = 1);  // 1-based, as CSI H sends
    void cursor_to_column(int col);                  // 1-based
    void cursor_to_line(int row);                    // 1-based
    void save_cursor();
    void restore_cursor();

    // --- erasing and editing ----------------------------------------------
    void erase_in_display(int mode);  // 0 = to end, 1 = to start, 2 = all,
                                      // 3 = all plus scrollback
    void erase_in_line(int mode);     // 0 = to end, 1 = to start, 2 = all
    void insert_lines(int n = 1);
    void delete_lines(int n = 1);
    void insert_characters(int n = 1);
    void delete_characters(int n = 1);
    void erase_characters(int n = 1);

    // --- scrolling ---------------------------------------------------------
    void set_margins(int top, int bottom);  // 1-based, inclusive
    void reset_margins();
    void index();          // move down, scrolling at the bottom margin
    void reverse_index();  // move up, scrolling at the top margin

    // Scroll the contents of the current region, leaving the cursor where it
    // is: SU (CSI S) and SD (CSI T). Not to be confused with the
    // scroll_history_* calls below, which move the viewport and touch no
    // cells. Rows leaving the top become scrollback on the same terms as any
    // other scroll -- only when the region includes the top of the screen.
    void scroll_up(int n = 1);
    void scroll_down(int n = 1);

    // --- attributes and modes ---------------------------------------------
    void select_graphic_rendition(const std::vector<int> &params);
    const Attrs &current_attrs() const { return attrs_; }
    void set_autowrap(bool on) { autowrap_ = on; }
    bool autowrap() const { return autowrap_; }

    // --- alternate screen --------------------------------------------------
    // Swap in a second buffer for full-screen applications: it has no
    // scrollback of its own, and the primary buffer -- including its history
    // -- is held untouched until the application leaves.
    //
    // This is the buffer swap only. Whether the cursor is saved and whether
    // either buffer is cleared differs between DECSET 47, 1047 and 1049, so
    // Stream applies that policy; Screen owns the buffers.
    void set_alternate_screen(bool on);
    bool alternate_screen() const { return alt_active_; }

    // --- scrollback --------------------------------------------------------
    // Retention budget in rows. Lowering it trims immediately; 0 disables
    // scrollback. Always applies to the primary buffer, so setting it while a
    // full-screen application is running does what the caller meant.
    void set_max_scrollback(int lines);
    int max_scrollback() const;

    // Rows currently held above the live screen.
    int history_size() const { return store_.scrollback(); }
    // Rows above the live screen the window is scrolled back by; 0 = live.
    int history_position() const { return view_pos_; }
    bool viewing_history() const { return viewing_; }

    // Move the window through scrollback. Writing to the screen -- or calling
    // scroll_history_to_bottom() -- returns it to the live output.
    void scroll_history_up(int n = 1);
    void scroll_history_down(int n = 1);
    void scroll_history_to_top();
    void scroll_history_to_bottom();

    // Absolute positioning, for a scrollbar: 0 is live output, history_size()
    // is the oldest retained row. Out-of-range values clamp rather than fail,
    // because a scrollbar's range and the history it describes can only be
    // resynchronised after the fact -- history trims as output arrives.
    void scroll_history_to(int position);

    // Random access to retained rows, independent of where the window sits:
    // --- absolute line addressing -----------------------------------------
    //
    // The row store already numbers every row it holds, live or scrolled off,
    // on one axis. These expose that axis, because it is the only coordinate
    // a selection can be anchored to: viewport coordinates move under a
    // selection when output scrolls, and history indices move when scrollback
    // trims. An absolute line number means the same text until that text is
    // discarded, and line_resident() answers whether it has been.
    //
    // first_line() is the oldest row still held, base_line() the first row of
    // the live screen, end_line() one past the newest. On the alternate
    // screen there is no scrollback, so first_line() == base_line() and the
    // range is just the visible rows.
    int first_line() const;
    int base_line() const;
    int end_line() const;
    bool line_resident(int line) const;

    // The absolute line currently shown at the top of the viewport. Adding a
    // viewport row to this converts a click into an absolute line.
    int view_top() const;

    // Text by absolute line, spanning history and live screen alike, whatever
    // the viewport happens to be pointed at. Out-of-range lines come back
    // empty rather than throwing: a selection can outlive the text it covers.
    std::string line_at(int line) const;
    std::vector<std::string> lines_in_range(int start, int end) const;

    // A rectangle of text: from (start_line, start_col) to (end_line,
    // end_col), end column exclusive, joined with newlines. Columns are grid
    // columns, so this slices cells rather than the rendered string -- a
    // double-width glyph occupies two columns but contributes one character,
    // and slicing text by column would cut in the wrong place after the first
    // one. Trailing spaces are trimmed per row, since a terminal row is
    // padded to the full width and nobody wants the padding.
    std::string text_in_range(int start_line, int start_col, int end_line,
                              int end_col) const;

    // index 0 is the oldest row still held, history_size() - 1 the newest.
    // This is what a scrollbar reads; paging is what a keyboard drives.
    const Cell &history_at(int x, int index) const;
    std::string history_line_text(int index) const;
    std::vector<std::string> history_lines() const;

    // --- output ------------------------------------------------------------
    // Full-width rows, no trailing-space trimming, so two dumps are directly
    // comparable byte for byte. Trim at the caller if you want readability.
    std::string dump() const;
    std::string line_text(int y) const;

    // Row indices touched since the last clear_dirty(). A renderer repaints
    // these; nothing in the emulator depends on it.
    const std::set<int> &dirty() const { return dirty_; }
    void clear_dirty() { dirty_.clear(); }

private:
    Cell &cell(int x, int y);  // live window, not the scrolled-back view
    void mark_dirty(int y);
    void mark_all_dirty();
    void mark_region_dirty();
    Cell blank() const;

    // The buffer that owns the scrollback, whichever one is on show.
    RowStore &primary_store();
    const RowStore &primary_store() const;

    // Leave scrollback. Every path that writes cells calls this first.
    void snap_to_live();
    static std::string row_text(const Row &row);

    int cols_;
    int rows_;
    int cursor_x_ = 0;
    int cursor_y_ = 0;
    int saved_x_ = 0;
    int saved_y_ = 0;
    int margin_top_ = 0;     // 0-based, inclusive
    int margin_bottom_ = 0;  // 0-based, inclusive
    bool autowrap_ = true;
    Attrs attrs_;
    Attrs saved_attrs_;
    RowStore store_;  // the buffer currently on show
    // The primary buffer while the alternate screen is on show, else null.
    // Held rather than copied, so the primary's history survives untouched.
    std::unique_ptr<RowStore> inactive_store_;
    bool alt_active_ = false;
    int view_pos_ = 0;      // rows above base currently displayed; 0 = live
    bool viewing_ = false;  // true when view_pos_ > 0
    std::set<int> dirty_;
};

}  // namespace pyte