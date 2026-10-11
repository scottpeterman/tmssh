// src/selection.cpp
#include "selection.h"

#include <algorithm>

namespace gtkpyte {

void Selection::clear() {
    active = false;
    dragging = false;
    anchor_line = anchor_col = focus_line = focus_col = 0;
}

void Selection::normalized(int &start_line, int &start_col, int &end_line,
                           int &end_col) const {
    const bool forward = anchor_line < focus_line ||
                         (anchor_line == focus_line && anchor_col <= focus_col);
    if (forward) {
        start_line = anchor_line;
        start_col = anchor_col;
        end_line = focus_line;
        end_col = focus_col;
    } else {
        start_line = focus_line;
        start_col = focus_col;
        end_line = anchor_line;
        end_col = anchor_col;
    }
}

bool Selection::span_on_line(int line, int cols, int &from, int &to) const {
    if (!active) {
        return false;
    }
    int start_line = 0;
    int start_col = 0;
    int end_line = 0;
    int end_col = 0;
    normalized(start_line, start_col, end_line, end_col);
    if (line < start_line || line > end_line) {
        return false;
    }
    from = (line == start_line) ? start_col : 0;
    to = (line == end_line) ? end_col : cols;
    from = std::clamp(from, 0, cols);
    to = std::clamp(to, 0, cols);
    return to > from;
}

bool is_word_char(char32_t ch) {
    if (ch < 128) {
        const char c = static_cast<char>(ch);
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               c == '_' || c == '-' || c == '.' || c == '/' || c == ':' || c == '~' ||
               c == '@' || c == '+' || c == '=' || c == '%' || c == '#';
    }
    // Non-ASCII counts as word: the alternative is a class table per script,
    // and erring this way only selects slightly too much.
    return ch != U' ';
}

bool word_bounds(const pyte::Screen &screen, int row, int col, int &start, int &end) {
    const int cols = screen.cols();
    // Grid cells, not the row's text: a click is a grid column, and indexing
    // text instead goes out of step after the first double-width glyph.
    const auto char_at = [&screen, row](int x) {
        const pyte::Cell &cell = screen.at(x, row);
        return cell.wide_continuation ? U'\0' : cell.ch;
    };
    // A click on the right half of a wide glyph means the glyph.
    while (col > 0 && screen.at(col, row).wide_continuation) {
        --col;
    }
    if (!is_word_char(char_at(col))) {
        start = col;
        end = col + 1;
        return false;
    }
    // Continuation cells ('\0') travel with the glyph to their left.
    const auto in_word = [&](int x) {
        return screen.at(x, row).wide_continuation || is_word_char(char_at(x));
    };
    start = col;
    while (start > 0 && in_word(start - 1)) {
        --start;
    }
    end = col + 1;
    while (end < cols && in_word(end)) {
        ++end;
    }
    return true;
}

int autoscroll_step(int overshoot_px, int cell_h, int page_lines) {
    const int rows_out = std::max(0, overshoot_px) / std::max(1, cell_h);
    const int step = 1 << std::min(rows_out, 10);
    return std::clamp(step, 1, std::max(1, page_lines));
}

}  // namespace gtkpyte
