// src/palette.cpp
#include "palette.h"

#include "pyte/cell.h"

namespace gtkpyte {
namespace {

// xterm defaults, in pyte's index order: 0-7 normal, 8-15 bright.
constexpr unsigned kAnsiDefaults[16] = {
    0x000000, 0xcd0000, 0x00cd00, 0xcdcd00,
    0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
    0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
    0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
};

// xterm's 6x6x6 cube levels -- not an even 0-255 split.
constexpr int kCubeLevels[6] = {0, 95, 135, 175, 215, 255};

GdkRGBA rgb(int r, int g, int b) {
    return GdkRGBA{static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                   static_cast<float>(b) / 255.0f, 1.0f};
}

GdkRGBA hex(unsigned v) {
    return rgb((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

}  // namespace

Palette::Palette()
    : foreground_(hex(0xd0d0d0)), background_(hex(0x101010)), cursor_(hex(0xd0d0d0)) {
    for (int i = 0; i < 16; ++i) {
        ansi_[i] = hex(kAnsiDefaults[i]);
    }
}

void Palette::set_ansi(int index, const GdkRGBA &c) {
    if (index >= 0 && index < 16) {
        ansi_[index] = c;
    }
}

GdkRGBA Palette::color(int index, const GdkRGBA &fallback, bool bold) const {
    if (index == pyte::kDefaultColor) {
        return fallback;
    }
    if (pyte::is_rgb(index)) {
        return rgb(pyte::rgb_red(index), pyte::rgb_green(index), pyte::rgb_blue(index));
    }
    if (index >= 0 && index < 8) {
        return ansi_[bold ? index + 8 : index];
    }
    if (index >= 8 && index < 16) {
        return ansi_[index];
    }
    if (index >= 16 && index < 232) {
        const int n = index - 16;
        return rgb(kCubeLevels[(n / 36) % 6], kCubeLevels[(n / 6) % 6], kCubeLevels[n % 6]);
    }
    if (index >= 232 && index < 256) {
        const int level = 8 + (index - 232) * 10;
        return rgb(level, level, level);
    }
    return fallback;
}

}  // namespace gtkpyte
