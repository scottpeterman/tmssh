// src/schemes.h
//
// Built-in colour schemes: foreground, background, cursor and the 16 ANSI
// entries, as 0xRRGGBB. Data only -- the widget turns a scheme into GdkRGBA.
#pragma once

#include <cstddef>

namespace gtkpyte {

struct ColorScheme {
    const char *name;
    unsigned foreground;
    unsigned background;
    unsigned cursor;
    unsigned ansi[16];  // 0-7 normal, 8-15 bright
};

// The scheme with this name, or nullptr. Names are lower-case-hyphenated.
const ColorScheme *find_scheme(const char *name);

// All built-in schemes, in display order.
const ColorScheme *schemes(std::size_t &count);

}  // namespace gtkpyte
