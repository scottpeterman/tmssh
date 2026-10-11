// include/pyte/unicode.h
#pragma once

#include <cstdint>
#include <string>

namespace pyte {

// Display width of a code point in terminal cells: 0 (combining), 1, or 2 (wide).
//
// Every width decision in the emulator goes through this one function. Width
// tables disagree at the edges (emoji, ZWJ sequences, ambiguous-width CJK), so
// when this port disagrees with the reference implementation, this is the
// single place to reconcile it.
int char_width(char32_t cp);

// Append `cp` to `out` as UTF-8.
void append_utf8(std::string &out, char32_t cp);

// Encode a single code point as a UTF-8 string.
std::string to_utf8(char32_t cp);

}  // namespace pyte
