// include/pyte/cell.h
#pragma once

#include <cstdint>

namespace pyte {

// Colour index meaning "terminal default", distinct from any palette entry.
inline constexpr int kDefaultColor = -1;

// Attrs::fg and Attrs::bg hold either a palette index (0-255), kDefaultColor,
// or a packed 24-bit colour. Values at or above kRgbFlag are packed colours,
// which keeps Cell at its existing size and leaves Attrs comparison a plain
// integer compare -- both matter, because there is one Cell per screen
// position and attribute equality is what run-batching in a renderer keys on.
inline constexpr int kRgbFlag = 1 << 24;

constexpr int pack_rgb(int r, int g, int b) {
    return kRgbFlag | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}
constexpr bool is_rgb(int color) { return color >= kRgbFlag; }
constexpr int rgb_red(int color) { return (color >> 16) & 0xFF; }
constexpr int rgb_green(int color) { return (color >> 8) & 0xFF; }
constexpr int rgb_blue(int color) { return color & 0xFF; }

struct Attrs {
    int fg = kDefaultColor;
    int bg = kDefaultColor;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool reverse = false;
    bool strikethrough = false;

    bool operator==(const Attrs &other) const {
        return fg == other.fg && bg == other.bg && bold == other.bold &&
               italic == other.italic && underline == other.underline &&
               reverse == other.reverse && strikethrough == other.strikethrough;
    }
    bool operator!=(const Attrs &other) const { return !(*this == other); }
};

struct Cell {
    char32_t ch = U' ';
    Attrs attrs;
    // Right half of a double-width character. Its `ch` is U'\0' and it is never
    // drawn on its own; a renderer paints the wide glyph from the left cell.
    bool wide_continuation = false;
    // Set on a row's LAST cell when autowrap carried output onto the next
    // row. A soft wrap, not a line break: copy joins across it. Anything that
    // blanks the cell (erase, scroll-in) clears it by assigning blank().
    bool wraps = false;
};

}  // namespace pyte
