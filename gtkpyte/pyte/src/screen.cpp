// src/screen.cpp
#include "pyte/screen.h"

#include <algorithm>
#include <utility>

#include "pyte/unicode.h"

namespace pyte {
namespace {

int clamp_int(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// Nothing visible on the row. Attributes are ignored: an erased row keeps
// whatever background was current, and that is still an empty row.
bool row_blank(const Row &row) {
    for (const Cell &cell : row) {
        if (cell.ch != U' ' && cell.ch != U'\0') {
            return false;
        }
    }
    return true;
}

}  // namespace

Screen::Screen(int cols, int rows, int max_scrollback)
    : cols_(std::max(cols, 1)),
      rows_(std::max(rows, 1)),
      store_(std::max(cols, 1), std::max(rows, 1), max_scrollback) {
    margin_top_ = 0;
    margin_bottom_ = rows_ - 1;
    mark_all_dirty();
}

Cell Screen::blank() const {
    Cell c;
    c.ch = U' ';
    c.attrs = attrs_;
    c.wide_continuation = false;
    return c;
}

Cell &Screen::cell(int x, int y) {
    // Live window only. Callers reach this after snap_to_live(), so the view
    // and the live screen are the same rows.
    return (*store_.line(y))[static_cast<std::size_t>(x)];
}

int Screen::view_top() const {
    const int top = store_.base() - view_pos_;
    return std::max(top, store_.origin());
}

const Cell &Screen::at(int x, int y) const {
    static const Cell kOutOfRange{};
    if (x < 0 || y < 0 || x >= cols_ || y >= rows_) {
        return kOutOfRange;
    }
    const Row *row = store_.at(view_top() + y);
    if (row == nullptr || x >= static_cast<int>(row->size())) {
        return kOutOfRange;
    }
    return (*row)[static_cast<std::size_t>(x)];
}

void Screen::mark_dirty(int y) {
    if (y >= 0 && y < rows_) {
        dirty_.insert(y);
    }
}

void Screen::mark_all_dirty() {
    for (int y = 0; y < rows_; ++y) {
        dirty_.insert(y);
    }
}

void Screen::mark_region_dirty() {
    for (int y = margin_top_; y <= margin_bottom_; ++y) {
        mark_dirty(y);
    }
}

void Screen::snap_to_live() {
    if (!viewing_) {
        return;
    }
    view_pos_ = 0;
    viewing_ = false;
    mark_all_dirty();
}

void Screen::resize(int cols, int rows) {
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);
    if (cols == cols_ && rows == rows_) {
        return;
    }
    snap_to_live();

    // Shrinking: blank rows below the cursor go first, the way xterm and VTE
    // do it. The store anchors the bottom, so without this a window that
    // shrinks with empty space under the prompt pushes the content ABOVE the
    // prompt into scrollback and leaves the empty rows on screen. Only blank
    // rows are dropped -- a row with anything on it is never lost, it goes
    // to scrollback as before.
    if (rows < rows_) {
        const int want = rows_ - rows;
        int blank_below = 0;
        for (int y = rows_ - 1; y > cursor_y_ && blank_below < want; --y) {
            if (!row_blank(*store_.line(y))) {
                break;
            }
            ++blank_below;
        }
        store_.drop_bottom(blank_below);
    }

    // The store anchors the BOTTOM of the live window across a line-count
    // change, so base moves and every resident row lands on a different screen
    // row. cursor_y_ is a screen row, so it has to move with them or it
    // silently comes to mean a different line of text -- and the next thing
    // written (a shell prompt) paints over content that is still on screen, at
    // exactly the row offset the window changed by.
    //
    // The delta is measured from the store rather than derived from
    // rows - rows_, because base clamps at origin when there is not enough
    // scrollback to expand into.
    const int old_base = store_.base();
    store_.resize(cols, rows, blank());
    cursor_y_ += old_base - store_.base();
    if (inactive_store_ != nullptr) {
        // The held buffer is not on show, but it has to come back at the
        // current geometry -- otherwise leaving a full-screen application
        // after a window resize restores a screen of the wrong shape.
        inactive_store_->resize(cols, rows, blank());
    }

    cols_ = cols;
    rows_ = rows;
    cursor_x_ = clamp_int(cursor_x_, 0, cols_ - 1);
    cursor_y_ = clamp_int(cursor_y_, 0, rows_ - 1);
    reset_margins();
    mark_all_dirty();
}

void Screen::reset() {
    attrs_ = Attrs{};
    saved_attrs_ = Attrs{};
    cursor_x_ = 0;
    cursor_y_ = 0;
    saved_x_ = 0;
    saved_y_ = 0;
    autowrap_ = true;
    view_pos_ = 0;
    viewing_ = false;
    if (alt_active_) {
        // RIS puts the terminal back on the primary buffer; an application
        // that reset and exited must not leave the alternate one on show.
        std::swap(store_, *inactive_store_);
        alt_active_ = false;
    }
    // Drop the alternate buffer entirely: RIS means nothing survives, so the
    // next application that switches gets a fresh one.
    inactive_store_.reset();
    store_.reset(blank());
    reset_margins();
    mark_all_dirty();
}

// --- writing ---------------------------------------------------------------

void Screen::draw(char32_t ch) {
    const int width = char_width(ch);

    if (width == 0) {
        // Combining mark: attach to the cell to the left if there is one.
        // Storing only the base code point is a known simplification of this
        // port; a full implementation keeps a combining sequence per cell.
        return;
    }

    snap_to_live();

    if (cursor_x_ + width > cols_) {
        if (!autowrap_) {
            cursor_x_ = cols_ - width;
            if (cursor_x_ < 0) {
                cursor_x_ = 0;
            }
        } else {
            // Mark the row being left before linefeed() moves off it (and,
            // at the bottom margin, scrolls it into history, flag intact).
            cell(cols_ - 1, cursor_y_).wraps = true;
            carriage_return();
            linefeed();
        }
    }

    Cell &target = cell(cursor_x_, cursor_y_);
    target.ch = ch;
    target.attrs = attrs_;
    target.wide_continuation = false;
    // An explicit write into the last column un-wraps the row until the next
    // character actually overflows it. This is what keeps readline redraws
    // honest: a row that is rewritten and then ends in CR LF is a hard break.
    target.wraps = false;

    if (width == 2 && cursor_x_ + 1 < cols_) {
        Cell &tail = cell(cursor_x_ + 1, cursor_y_);
        tail.ch = U'\0';
        tail.attrs = attrs_;
        tail.wide_continuation = true;
        tail.wraps = false;
    }

    mark_dirty(cursor_y_);
    cursor_x_ += width;
    if (cursor_x_ > cols_) {
        cursor_x_ = cols_;
    }
}

// --- C0 controls -----------------------------------------------------------

void Screen::carriage_return() { cursor_x_ = 0; }

void Screen::linefeed() { index(); }

void Screen::backspace() {
    if (cursor_x_ > 0) {
        --cursor_x_;
    }
}

void Screen::tab() {
    // Fixed 8-column tab stops. Real terminals keep a settable stop table
    // (HTS / TBC); this port has not needed one yet.
    const int next = ((cursor_x_ / 8) + 1) * 8;
    cursor_x_ = std::min(next, cols_ - 1);
}

// --- cursor movement -------------------------------------------------------

void Screen::cursor_up(int n) {
    cursor_y_ = std::max(cursor_y_ - std::max(n, 1), margin_top_);
}

void Screen::cursor_down(int n) {
    cursor_y_ = std::min(cursor_y_ + std::max(n, 1), margin_bottom_);
}

void Screen::cursor_forward(int n) {
    cursor_x_ = std::min(cursor_x_ + std::max(n, 1), cols_ - 1);
}

void Screen::cursor_back(int n) {
    cursor_x_ = std::max(cursor_x_ - std::max(n, 1), 0);
}

void Screen::cursor_position(int row, int col) {
    cursor_y_ = clamp_int(row - 1, 0, rows_ - 1);
    cursor_x_ = clamp_int(col - 1, 0, cols_ - 1);
}

void Screen::cursor_to_column(int col) {
    cursor_x_ = clamp_int(col - 1, 0, cols_ - 1);
}

void Screen::cursor_to_line(int row) {
    cursor_y_ = clamp_int(row - 1, 0, rows_ - 1);
}

void Screen::save_cursor() {
    saved_x_ = cursor_x_;
    saved_y_ = cursor_y_;
    saved_attrs_ = attrs_;
}

void Screen::restore_cursor() {
    cursor_x_ = clamp_int(saved_x_, 0, cols_ - 1);
    cursor_y_ = clamp_int(saved_y_, 0, rows_ - 1);
    attrs_ = saved_attrs_;
}

// --- erasing and editing ---------------------------------------------------

void Screen::erase_in_line(int mode) {
    snap_to_live();
    Row *line = store_.line(cursor_y_);
    if (line == nullptr) {
        return;
    }
    const Cell b = blank();
    int from = 0;
    int to = cols_;
    if (mode == 0) {
        from = cursor_x_;
    } else if (mode == 1) {
        to = std::min(cursor_x_ + 1, cols_);
    } else if (mode != 2) {
        return;
    }
    for (int x = from; x < to; ++x) {
        (*line)[static_cast<std::size_t>(x)] = b;
    }
    mark_dirty(cursor_y_);
}

void Screen::erase_in_display(int mode) {
    snap_to_live();
    const Cell b = blank();
    if (mode == 0) {
        erase_in_line(0);
        for (int y = cursor_y_ + 1; y < rows_; ++y) {
            if (Row *line = store_.line(y)) {
                std::fill(line->begin(), line->end(), b);
            }
            mark_dirty(y);
        }
    } else if (mode == 1) {
        erase_in_line(1);
        for (int y = 0; y < cursor_y_; ++y) {
            if (Row *line = store_.line(y)) {
                std::fill(line->begin(), line->end(), b);
            }
            mark_dirty(y);
        }
    } else if (mode == 2 || mode == 3) {
        for (int y = 0; y < rows_; ++y) {
            if (Row *line = store_.line(y)) {
                std::fill(line->begin(), line->end(), b);
            }
        }
        if (mode == 3) {
            // CSI 3 J is "erase saved lines" -- the scrollback, not just the
            // window. Clearing only the window would leave a user who pressed
            // clear still able to scroll back to what they cleared.
            store_.clear_scrollback();
        }
        mark_all_dirty();
    }
}

void Screen::insert_lines(int n) {
    snap_to_live();
    if (cursor_y_ < margin_top_ || cursor_y_ > margin_bottom_) {
        return;  // cursor outside the region: DECSTBM says do nothing
    }
    n = std::max(n, 1);
    n = std::min(n, margin_bottom_ - cursor_y_ + 1);
    const Cell b = blank();
    for (int i = 0; i < n; ++i) {
        // Lines pushed past the region bottom are lost -- they are NOT
        // scrollback, because the region bottom is not the bottom of the
        // screen. IL is an edit inside the region, not a scroll of the screen.
        store_.scroll_region_down(cursor_y_, margin_bottom_, b);
    }
    for (int y = cursor_y_; y <= margin_bottom_; ++y) {
        mark_dirty(y);
    }
    cursor_x_ = 0;
}

void Screen::delete_lines(int n) {
    snap_to_live();
    if (cursor_y_ < margin_top_ || cursor_y_ > margin_bottom_) {
        return;
    }
    n = std::max(n, 1);
    n = std::min(n, margin_bottom_ - cursor_y_ + 1);
    const Cell b = blank();
    for (int i = 0; i < n; ++i) {
        store_.scroll_region(cursor_y_, margin_bottom_, b);
    }
    for (int y = cursor_y_; y <= margin_bottom_; ++y) {
        mark_dirty(y);
    }
    cursor_x_ = 0;
}

void Screen::insert_characters(int n) {
    snap_to_live();
    Row *line = store_.line(cursor_y_);
    if (line == nullptr) {
        return;
    }
    n = std::max(n, 1);
    const Cell b = blank();
    for (int i = 0; i < n; ++i) {
        line->pop_back();
        line->insert(line->begin() + cursor_x_, b);
    }
    mark_dirty(cursor_y_);
}

void Screen::delete_characters(int n) {
    snap_to_live();
    Row *line = store_.line(cursor_y_);
    if (line == nullptr) {
        return;
    }
    n = std::max(n, 1);
    const Cell b = blank();
    for (int i = 0; i < n; ++i) {
        line->erase(line->begin() + cursor_x_);
        line->push_back(b);
    }
    mark_dirty(cursor_y_);
}

void Screen::erase_characters(int n) {
    snap_to_live();
    Row *line = store_.line(cursor_y_);
    if (line == nullptr) {
        return;
    }
    n = std::max(n, 1);
    const Cell b = blank();
    const int to = std::min(cursor_x_ + n, cols_);
    for (int x = cursor_x_; x < to; ++x) {
        (*line)[static_cast<std::size_t>(x)] = b;
    }
    mark_dirty(cursor_y_);
}

// --- scrolling -------------------------------------------------------------

void Screen::set_margins(int top, int bottom) {
    const int t = clamp_int(top - 1, 0, rows_ - 1);
    const int bttm = clamp_int(bottom - 1, 0, rows_ - 1);
    if (t >= bttm) {
        return;  // degenerate region: terminals ignore it
    }
    margin_top_ = t;
    margin_bottom_ = bttm;
    cursor_position(1, 1);
}

void Screen::reset_margins() {
    margin_top_ = 0;
    margin_bottom_ = rows_ - 1;
}

void Screen::scroll_up(int n) {
    snap_to_live();
    n = std::max(n, 1);
    const Cell b = blank();
    for (int i = 0; i < n; ++i) {
        if (margin_top_ == 0) {
            // The region includes the top of the screen, so the displaced row
            // is real scrollback.
            store_.advance(b);
            if (margin_bottom_ < rows_ - 1) {
                // advance() moved EVERY row up by one, including the rows
                // below the region, which must not have moved. Shifting
                // [margin_bottom_, rows_ - 1] back down restores them AND
                // leaves the region's own bottom row blank, which is what a
                // scroll owes it.
                //
                // Starting the shift at margin_bottom_ + 1 instead is off by
                // one: the row that had been at margin_bottom_ + 1 is sitting
                // at margin_bottom_ after the advance, so a shift from
                // margin_bottom_ + 1 leaves it there. For a full-screen editor
                // (everything except a status line) that is a one-row shift of
                // the last row, and the status line gets dragged up into the
                // text and stays there.
                store_.scroll_region_down(margin_bottom_, rows_ - 1, b);
            }
        } else {
            store_.scroll_region(margin_top_, margin_bottom_, b);
        }
    }
    mark_region_dirty();
}

void Screen::scroll_down(int n) {
    snap_to_live();
    n = std::max(n, 1);
    const Cell b = blank();
    for (int i = 0; i < n; ++i) {
        store_.scroll_region_down(margin_top_, margin_bottom_, b);
    }
    mark_region_dirty();
}

void Screen::index() {
    snap_to_live();
    if (cursor_y_ == margin_bottom_) {
        scroll_up(1);
    } else if (cursor_y_ < rows_ - 1) {
        ++cursor_y_;
    }
}

void Screen::reverse_index() {
    snap_to_live();
    if (cursor_y_ == margin_top_) {
        // Nothing is pulled back out of scrollback here: RI scrolls the
        // region, and the region's contents are the screen's, not history's.
        scroll_down(1);
    } else if (cursor_y_ > 0) {
        --cursor_y_;
    }
}

// --- attributes ------------------------------------------------------------

void Screen::select_graphic_rendition(const std::vector<int> &params) {
    if (params.empty()) {
        attrs_ = Attrs{};
        return;
    }

    for (std::size_t i = 0; i < params.size(); ++i) {
        const int p = params[i];
        switch (p) {
            case 0: attrs_ = Attrs{}; break;
            case 1: attrs_.bold = true; break;
            case 3: attrs_.italic = true; break;
            case 4: attrs_.underline = true; break;
            case 7: attrs_.reverse = true; break;
            case 9: attrs_.strikethrough = true; break;
            case 22: attrs_.bold = false; break;
            case 23: attrs_.italic = false; break;
            case 24: attrs_.underline = false; break;
            case 27: attrs_.reverse = false; break;
            case 29: attrs_.strikethrough = false; break;
            case 39: attrs_.fg = kDefaultColor; break;
            case 49: attrs_.bg = kDefaultColor; break;
            case 38:
            case 48: {
                // Extended colour: 5;<n> for a 256-colour index, 2;<r>;<g>;<b>
                // for truecolor. Stream normalises the colon sub-parameter
                // form to this shape before dispatching, so only one layout
                // has to be handled here.
                const bool is_fg = (p == 38);
                int &target = is_fg ? attrs_.fg : attrs_.bg;
                if (i + 1 < params.size() && params[i + 1] == 5) {
                    if (i + 2 < params.size()) {
                        target = params[i + 2];
                    }
                    i += 2;
                } else if (i + 1 < params.size() && params[i + 1] == 2) {
                    if (i + 4 < params.size()) {
                        target = pack_rgb(params[i + 2], params[i + 3],
                                          params[i + 4]);
                    }
                    i += 4;
                }
                break;
            }
            default:
                if (p >= 30 && p <= 37) {
                    attrs_.fg = p - 30;
                } else if (p >= 40 && p <= 47) {
                    attrs_.bg = p - 40;
                } else if (p >= 90 && p <= 97) {
                    attrs_.fg = p - 90 + 8;
                } else if (p >= 100 && p <= 107) {
                    attrs_.bg = p - 100 + 8;
                }
                break;
        }
    }
}

// --- alternate screen ------------------------------------------------------

RowStore &Screen::primary_store() {
    return alt_active_ ? *inactive_store_ : store_;
}

const RowStore &Screen::primary_store() const {
    return alt_active_ ? *inactive_store_ : store_;
}

void Screen::set_alternate_screen(bool on) {
    if (on == alt_active_) {
        return;  // DECSET is idempotent; re-entering must not stack buffers
    }
    snap_to_live();

    if (inactive_store_ == nullptr) {
        // First entry: build the alternate buffer. A budget of 0 is what makes
        // it the alternate screen rather than a second primary -- a
        // full-screen application redraws constantly, and every one of those
        // redraws would otherwise land in history.
        inactive_store_ = std::make_unique<RowStore>(cols_, rows_, 0);
    }

    // Both buffers are kept for the life of the Screen and swapped, never
    // rebuilt. The alternate screen's contents have to survive a switch away
    // and back -- that persistence is the whole reason DECSET 1047 has to
    // clear it on the way out, and why an application using 47 can leave and
    // return to the frame it drew.
    std::swap(store_, *inactive_store_);

    alt_active_ = on;
    mark_all_dirty();
}

// --- scrollback ------------------------------------------------------------

int Screen::max_scrollback() const { return primary_store().max_scrollback(); }

void Screen::set_max_scrollback(int lines) {
    primary_store().set_max_scrollback(lines);
    if (view_pos_ > store_.scrollback()) {
        view_pos_ = store_.scrollback();
        viewing_ = view_pos_ > 0;
        mark_all_dirty();
    }
}

void Screen::scroll_history_to(int position) {
    const int available = store_.scrollback();
    int pos = position;
    if (pos < 0) {
        pos = 0;
    }
    if (pos > available) {
        pos = available;
    }
    if (pos == view_pos_) {
        return;
    }
    if (pos == 0) {
        // Not just view_pos_ = 0: leaving history has to clear viewing_ too,
        // and a renderer reads that flag to bring the cursor back and re-arm
        // auto-scroll.
        snap_to_live();
        return;
    }
    view_pos_ = pos;
    viewing_ = true;
    mark_all_dirty();
}

void Screen::scroll_history_up(int n) {
    scroll_history_to(view_pos_ + std::max(n, 1));
}

void Screen::scroll_history_down(int n) {
    if (!viewing_) {
        return;
    }
    scroll_history_to(view_pos_ - std::max(n, 1));
}

void Screen::scroll_history_to_top() { scroll_history_to(store_.scrollback()); }

void Screen::scroll_history_to_bottom() { snap_to_live(); }

const Cell &Screen::history_at(int x, int index) const {
    static const Cell kOutOfRange{};
    if (x < 0 || index < 0 || index >= store_.scrollback()) {
        return kOutOfRange;
    }
    const Row *row = store_.at(store_.origin() + index);
    if (row == nullptr || x >= static_cast<int>(row->size())) {
        return kOutOfRange;
    }
    return (*row)[static_cast<std::size_t>(x)];
}

std::string Screen::history_line_text(int index) const {
    if (index < 0 || index >= store_.scrollback()) {
        return std::string();
    }
    const Row *row = store_.at(store_.origin() + index);
    return row == nullptr ? std::string() : row_text(*row);
}

std::vector<std::string> Screen::history_lines() const {
    const int n = store_.scrollback();
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        out.push_back(history_line_text(i));
    }
    return out;
}

// --- output ----------------------------------------------------------------

std::string Screen::row_text(const Row &row) {
    std::string out;
    for (const Cell &c : row) {
        if (c.wide_continuation) {
            continue;  // the wide glyph was emitted by its left half
        }
        append_utf8(out, c.ch == U'\0' ? U' ' : c.ch);
    }
    return out;
}

std::string Screen::line_text(int y) const {
    if (y < 0 || y >= rows_) {
        return std::string();
    }
    const Row *row = store_.at(view_top() + y);
    return row == nullptr ? std::string() : row_text(*row);
}

std::string Screen::dump() const {
    std::string out;
    for (int y = 0; y < rows_; ++y) {
        out += line_text(y);
        if (y + 1 < rows_) {
            out += '\n';
        }
    }
    return out;
}

}  // namespace pyte
