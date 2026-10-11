// src/rowstore.cpp
#include "pyte/rowstore.h"

#include <algorithm>

namespace pyte {

RowStore::RowStore(int cols, int lines, int max_scrollback)
    : cols_(std::max(cols, 1)),
      lines_(std::max(lines, 1)),
      max_(std::max(max_scrollback, 0)) {
    for (int i = 0; i < lines_; ++i) {
        rows_.push_back(make_row(Cell{}));
    }
}

Row RowStore::make_row(const Cell &fill) const {
    return Row(static_cast<std::size_t>(cols_), fill);
}

std::size_t RowStore::index_of(int abs) const {
    return static_cast<std::size_t>(abs - origin_);
}

bool RowStore::resident(int abs) const {
    return abs >= origin_ && abs < total();
}

Row *RowStore::at(int abs) {
    if (!resident(abs)) {
        return nullptr;
    }
    return &rows_[index_of(abs)];
}

const Row *RowStore::at(int abs) const {
    if (!resident(abs)) {
        return nullptr;
    }
    return &rows_[index_of(abs)];
}

Row *RowStore::line(int y) {
    if (y < 0 || y >= lines_) {
        return nullptr;
    }
    return at(base_ + y);
}

const Row *RowStore::line(int y) const {
    if (y < 0 || y >= lines_) {
        return nullptr;
    }
    return at(base_ + y);
}

void RowStore::set_max_scrollback(int max) {
    max_ = std::max(max, 0);
    trim();
}

void RowStore::advance(const Cell &fill) {
    ++base_;
    rows_.push_back(make_row(fill));
    trim();
}

void RowStore::trim() {
    const int keep = base_ - max_;
    if (keep <= origin_) {
        return;
    }
    int drop = keep - origin_;
    const int held = static_cast<int>(rows_.size());
    if (drop >= held) {
        drop = held - 1;
    }
    if (drop <= 0) {
        return;
    }
    rows_.erase(rows_.begin(), rows_.begin() + drop);
    origin_ += drop;
}

void RowStore::scroll_region(int top, int bottom, const Cell &fill) {
    // top == bottom is a one-row region: the loop does not run, the row is
    // cleared, and that is the correct result. Rejecting it would silently
    // turn a one-row scroll into a no-op, which is how a status line survives
    // a scroll it was supposed to be excluded from.
    if (top < 0 || bottom >= lines_ || top > bottom) {
        return;
    }
    const auto first = rows_.begin() + static_cast<std::ptrdiff_t>(index_of(base_ + top));
    const auto last = first + (bottom - top) + 1;
    std::rotate(first, first + 1, last);
    std::fill((last - 1)->begin(), (last - 1)->end(), fill);
}

void RowStore::scroll_region_down(int top, int bottom, const Cell &fill) {
    if (top < 0 || bottom >= lines_ || top > bottom) {
        return;
    }
    const auto first = rows_.begin() + static_cast<std::ptrdiff_t>(index_of(base_ + top));
    const auto last = first + (bottom - top) + 1;
    std::rotate(first, last - 1, last);
    std::fill(first->begin(), first->end(), fill);
}

void RowStore::resize(int cols, int lines, const Cell &fill) {
    cols = std::max(cols, 1);
    lines = std::max(lines, 1);

    if (cols != cols_) {
        for (Row &r : rows_) {
            r.resize(static_cast<std::size_t>(cols), fill);
        }
        cols_ = cols;
    }

    if (lines != lines_) {
        // Anchor the bottom: the absolute index one past the last live row is
        // the fixed point, so visible content does not jump and rows leaving
        // the top become scrollback instead of disappearing.
        const int bottom = base_ + lines_;
        lines_ = lines;
        base_ = bottom - lines_;
        if (base_ < origin_) {
            base_ = origin_;  // not enough scrollback to expand into
        }
        const int need = base_ + lines_ - total();
        for (int i = 0; i < need; ++i) {
            rows_.push_back(make_row(fill));
        }
        trim();
    }
}

void RowStore::drop_bottom(int n) {
    n = std::min(std::max(n, 0), lines_ - 1);
    for (int i = 0; i < n; ++i) {
        rows_.pop_back();
    }
    lines_ -= n;
}

void RowStore::clear_scrollback() {
    const int drop = base_ - origin_;
    if (drop <= 0) {
        return;
    }
    rows_.erase(rows_.begin(), rows_.begin() + drop);
    origin_ = base_;
}

void RowStore::reset(const Cell &fill) {
    rows_.clear();
    for (int i = 0; i < lines_; ++i) {
        rows_.push_back(make_row(fill));
    }
    origin_ = 0;
    base_ = 0;
}

}  // namespace pyte
