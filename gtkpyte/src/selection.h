// src/selection.h
//
// A text selection in absolute line coordinates, plus the pieces of selection
// behaviour that do not need a widget: word boundaries and auto-scroll speed.
//
// Absolute lines, not viewport rows: a viewport row stops meaning the same
// text the moment output scrolls, and a highlight held in viewport rows crawls
// up the screen while the user holds still. pyte numbers every row it holds,
// live or scrolled off, on one axis; the selection lives on that axis and is
// converted to viewport rows only when painting.
#pragma once

#include "pyte/screen.h"

namespace gtkpyte {

struct Selection {
    int anchor_line = 0;  // where the drag started; does not move
    int anchor_col = 0;
    int focus_line = 0;   // follows the pointer
    int focus_col = 0;

    bool active = false;    // exists and should be painted
    bool dragging = false;  // primary button down, focus still moving

    void clear();

    // A plain click: nothing between the two ends.
    bool empty() const { return anchor_line == focus_line && anchor_col == focus_col; }

    // Start before end whichever way the drag went. End column is exclusive.
    void normalized(int &start_line, int &start_col, int &end_line, int &end_col) const;

    // The selected [from, to) columns on one absolute line; false when the
    // line is outside the selection.
    bool span_on_line(int line, int cols, int &from, int &to) const;
};

// Word around viewport cell (col, row) for double-click, as [start, end)
// columns. Wider than alphanumerics on purpose: in a terminal the thing worth
// selecting is a path, URL, address or interface name, and stopping at every
// dot or slash turns one double-click into six. Returns false when the cell is
// not a word character, in which case [col, col+1) is the selection.
bool word_bounds(const pyte::Screen &screen, int row, int col, int &start, int &end);

bool is_word_char(char32_t ch);

// Lines per auto-scroll tick from how far past the edge the pointer is: one
// line just over the edge for precise landing, doubling for each further row
// of overshoot, capped at a page.
int autoscroll_step(int overshoot_px, int cell_h, int page_lines);

}  // namespace gtkpyte
