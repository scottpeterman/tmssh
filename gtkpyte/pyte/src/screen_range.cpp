// src/screen_range.cpp
//
// Absolute line addressing and ranged text extraction.
//
// A separate translation unit rather than more of screen.cpp: this is read-only
// work with one caller in mind -- a selection in a renderer -- and none of it
// participates in the escape-sequence machinery that fills the rest of the
// core. Keeping it apart means the emulation can be read without wading
// through it, and it can be read without wading through the emulation.

#include "pyte/screen.h"

#include <algorithm>

namespace pyte {
namespace {

// UTF-8 encoding, again, rather than reaching for the one in screen.cpp: that
// one has internal linkage, and a shared header for eleven lines of bit
// shifting would be worse than the duplication.
void append_utf8(std::string &out, char32_t ch) {
    if (ch < 0x80) {
        out.push_back(static_cast<char>(ch));
    } else if (ch < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else if (ch < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (ch >> 18)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    }
}

void trim_trailing_spaces(std::string &text) {
    const std::size_t end = text.find_last_not_of(' ');
    if (end == std::string::npos) {
        text.clear();
    } else {
        text.erase(end + 1);
    }
}

}  // namespace

int Screen::first_line() const { return store_.origin(); }

int Screen::base_line() const { return store_.base(); }

int Screen::end_line() const { return store_.total(); }

bool Screen::line_resident(int line) const { return store_.resident(line); }

std::string Screen::line_at(int line) const {
    const Row *row = store_.at(line);
    return row == nullptr ? std::string() : row_text(*row);
}

std::vector<std::string> Screen::lines_in_range(int start, int end) const {
    std::vector<std::string> out;

    // Clamped rather than rejected. A selection dragged to the top of a long
    // history and left there while output arrives can outlive its own oldest
    // line; returning what survives is more useful than returning nothing.
    start = std::max(start, store_.origin());
    end = std::min(end, store_.total());
    if (start >= end) {
        return out;
    }

    out.reserve(static_cast<std::size_t>(end - start));
    for (int line = start; line < end; ++line) {
        out.push_back(line_at(line));
    }
    return out;
}

std::string Screen::text_in_range(int start_line, int start_col, int end_line,
                                  int end_col) const {
    if (end_line < start_line ||
        (end_line == start_line && end_col <= start_col)) {
        return std::string();
    }

    start_line = std::max(start_line, store_.origin());
    end_line = std::min(end_line, store_.total() - 1);
    if (start_line > end_line) {
        return std::string();
    }

    // Columns are grid columns, so the slice happens over cells. Slicing
    // row_text() instead would be wrong wherever a double-width glyph appears
    // before the cut: it occupies two columns and contributes one character,
    // so the character index and the column index diverge at that point.
    // A row that autowrapped continues onto the next one: no newline between
    // them, and no trimming at the seam, where a trailing space is content
    // ("foo bar" split at the space must not come back as "foobar").
    const auto wraps = [this](int line) {
        const Row *row = store_.at(line);
        return row != nullptr && !row->empty() && row->back().wraps;
    };

    const auto slice = [this](int line, int from, int to, bool trim) {
        std::string out;
        const Row *row = store_.at(line);
        if (row == nullptr) {
            return out;
        }
        const int width = static_cast<int>(row->size());
        from = std::max(0, from);
        to = std::min(to, width);
        for (int x = from; x < to; ++x) {
            const Cell &c = (*row)[static_cast<std::size_t>(x)];
            if (c.wide_continuation) {
                continue;  // emitted by its left half
            }
            append_utf8(out, c.ch == U'\0' ? U' ' : c.ch);
        }
        if (trim) {
            trim_trailing_spaces(out);
        }
        return out;
    };

    if (start_line == end_line) {
        return slice(start_line, start_col, end_col, true);
    }

    std::string out = slice(start_line, start_col, cols_, !wraps(start_line));
    for (int line = start_line + 1; line <= end_line; ++line) {
        if (!wraps(line - 1)) {
            out.push_back('\n');
        }
        const bool last = line == end_line;
        out += slice(line, 0, last ? end_col : cols_, last || !wraps(line));
    }
    return out;
}

}  // namespace pyte