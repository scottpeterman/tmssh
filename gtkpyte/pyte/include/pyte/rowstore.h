// include/pyte/rowstore.h
#pragma once

#include <cstddef>
#include <deque>
#include <vector>

#include "pyte/cell.h"

namespace pyte {

// One line of terminal content.
using Row = std::vector<Cell>;

// A grow-only row list backing both scrollback and the live screen.
//
// There is one list. The live screen is a window into it, defined by base():
// screen row y is absolute row base() + y. A linefeed at the bottom is
// base()++ plus one appended row -- no copy between containers, no seam for a
// reader to re-fuse, and no saved screen to fall out of sync with the live
// one. Viewing scrollback is a read offset the caller keeps; the store never
// needs to know about it.
//
// Absolute indices are stable for the lifetime of a row. Trimming the head
// advances origin() rather than renumbering, so a caller holding an absolute
// index can always ask resident() whether it still exists.
//
// Invariant: total() == base() + lines(), i.e. the live window always sits at
// the very end of the list. advance() and resize() both maintain it.
class RowStore {
public:
    RowStore(int cols, int lines, int max_scrollback);

    int cols() const { return cols_; }
    int lines() const { return lines_; }

    // Absolute index of screen row 0.
    int base() const { return base_; }
    // Absolute index of the oldest row still held.
    int origin() const { return origin_; }
    // Resident rows: scrollback plus the live window.
    int total() const { return origin_ + static_cast<int>(rows_.size()); }
    // Rows above the live window.
    int scrollback() const { return base_ - origin_; }

    int max_scrollback() const { return max_; }
    void set_max_scrollback(int max);

    bool resident(int abs) const;
    Row *at(int abs);
    const Row *at(int abs) const;
    Row *line(int y);  // live screen row, 0-based
    const Row *line(int y) const;

    // Scroll the live window down one row: the top screen row becomes the
    // newest scrollback row and a fresh row of `fill` appears at the bottom.
    void advance(const Cell &fill);

    // Scroll rows [top, bottom] of the live window up (respectively down) by
    // one without touching scrollback. Both ends are inclusive and top ==
    // bottom is a legitimate one-row region: the row is simply cleared.
    //
    // When top is 0, prefer advance() -- these two discard the displaced row.
    void scroll_region(int top, int bottom, const Cell &fill);
    void scroll_region_down(int top, int bottom, const Cell &fill);

    // Change geometry. The bottom of the live window stays anchored, so
    // shrinking pushes rows into scrollback rather than dropping them and
    // growing pulls them back. base() moves; a caller tracking a screen row
    // (a cursor) must shift it by the delta.
    void resize(int cols, int lines, const Cell &fill);

    // Remove the bottom n rows of the live window outright -- not into
    // scrollback -- and shrink the window by n. base() does not move. For a
    // resize that should lose blank rows under the cursor before it pushes
    // content above it into history. At least one row always remains.
    void drop_bottom(int n);

    // Drop scrollback, keeping the live window. This is what CSI 3 J asks for.
    void clear_scrollback();

    // Drop everything, including scrollback, and start on a fresh window.
    void reset(const Cell &fill);

private:
    void trim();
    Row make_row(const Cell &fill) const;
    std::size_t index_of(int abs) const;

    std::deque<Row> rows_;
    int origin_ = 0;
    int base_ = 0;
    int cols_;
    int lines_;
    int max_;  // rows retained above base; 0 = no scrollback
};

}  // namespace pyte
