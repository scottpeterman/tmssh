// src/unicode.cpp
#include "pyte/unicode.h"

#include <utf8proc.h>

namespace pyte {

int char_width(char32_t cp) {
    if (cp == 0) {
        return 0;
    }
    // C0/C1 controls occupy no cells. They should never reach a cell anyway,
    // but a stray one shouldn't shift the grid.
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
        return 0;
    }
    const utf8proc_int32_t width = utf8proc_charwidth(static_cast<utf8proc_int32_t>(cp));
    if (width < 0) {
        return 1;
    }
    return static_cast<int>(width);
}

void append_utf8(std::string &out, char32_t cp) {
    utf8proc_uint8_t buf[4];
    const utf8proc_ssize_t n =
        utf8proc_encode_char(static_cast<utf8proc_int32_t>(cp), buf);
    if (n <= 0) {
        return;
    }
    out.append(reinterpret_cast<const char *>(buf), static_cast<std::size_t>(n));
}

std::string to_utf8(char32_t cp) {
    std::string out;
    append_utf8(out, cp);
    return out;
}

}  // namespace pyte
