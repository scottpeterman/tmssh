// src/palette.h
#pragma once

#include <gdk/gdk.h>

namespace gtkpyte {

// Maps the colour values pyte::Attrs carries onto GdkRGBA.
//
// pyte::kDefaultColor means "the terminal's default", which only the widget
// knows, so it resolves against a fallback passed in rather than a table entry.
class Palette {
public:
    Palette();

    // index is a pyte::Attrs fg/bg value: 0-15 ANSI, 16-255 the xterm cube
    // and greyscale ramp, a packed 24-bit colour, or pyte::kDefaultColor.
    // `bold` brightens 0-7, which is what applications still assume.
    GdkRGBA color(int index, const GdkRGBA &fallback, bool bold = false) const;

    const GdkRGBA &foreground() const { return foreground_; }
    const GdkRGBA &background() const { return background_; }
    const GdkRGBA &cursor() const { return cursor_; }
    void set_foreground(const GdkRGBA &c) { foreground_ = c; }
    void set_background(const GdkRGBA &c) { background_ = c; }
    void set_cursor(const GdkRGBA &c) { cursor_ = c; }

    // One of the 16 ANSI entries. The cube and ramp above them are xterm's.
    void set_ansi(int index, const GdkRGBA &c);
    const GdkRGBA &ansi(int index) const { return ansi_[index & 15]; }

private:
    GdkRGBA ansi_[16];
    GdkRGBA foreground_;
    GdkRGBA background_;
    GdkRGBA cursor_;
};

}  // namespace gtkpyte
