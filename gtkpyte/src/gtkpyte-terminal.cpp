// src/gtkpyte-terminal.cpp
//
// GtkpyteTerminal implementation. pyte owns what the screen contains; this
// file owns how it looks, what the keyboard sends, and turning pointer
// positions back into cells for selection. Nothing here parses an escape
// sequence, and nothing in pyte knows about GTK.

#include "gtkpyte-terminal.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "keymap.h"
#include "mouse.h"
#include "palette.h"
#include "pyte/screen.h"
#include "pyte/stream.h"
#include "schemes.h"
#include "selection.h"

namespace {

constexpr int kDefaultScrollback = 5000;
constexpr const char *kDefaultFont = "Monospace 11";

// DEC private modes this widget acts on. Everything else is observed only.
constexpr int kModeCursorKeys = 1;       // DECCKM
constexpr int kModeCursorVisible = 25;   // DECTCEM
constexpr int kModeBracketedPaste = 2004;

// Mouse-wheel notches scroll this many lines; touchpads scroll by pixels.
constexpr double kLinesPerWheelStep = 3.0;

// Auto-scroll tick while dragging a selection past the top or bottom edge.
// Speed comes from lines per tick (see autoscroll_step), not the interval.
constexpr guint kAutoScrollIntervalMs = 30;

// Default selection highlight. Painted over the glyphs with alpha rather than
// as a background, so it works whatever colours the application chose -- for
// btop that is most of what is on screen.
constexpr GdkRGBA kSelectionColor = {80.0f / 255.0f, 120.0f / 255.0f, 200.0f / 255.0f,
                                     110.0f / 255.0f};

enum { SIGNAL_COMMIT, SIGNAL_RESIZED, SIGNAL_SELECTION_CHANGED, N_SIGNALS };
guint g_signals[N_SIGNALS];

// GtkScrollable's four properties, overridden from the interface.
enum {
    PROP_0,
    PROP_HADJUSTMENT,
    PROP_VADJUSTMENT,
    PROP_HSCROLL_POLICY,
    PROP_VSCROLL_POLICY,
    // Our own, after the four GtkScrollable overrides.
    PROP_FONT,
    PROP_SCROLLBACK_LINES,
    PROP_COLOR_SCHEME,
    PROP_FOREGROUND_COLOR,
    PROP_BACKGROUND_COLOR,
    PROP_CURSOR_COLOR,
    PROP_SELECTION_COLOR,
    N_PROPS
};
GParamSpec *g_props[N_PROPS];

// Everything with a C++ constructor lives here. GObject allocates instance
// structs with g_malloc0 and never runs constructors, so C++ members cannot sit
// in the instance struct directly; one owned pointer keeps that problem in one
// place (new in init, delete in finalize).
struct State {
    pyte::Screen screen{80, 24, kDefaultScrollback};
    pyte::Stream stream{screen};
    gtkpyte::Palette palette;
    gtkpyte::KeyModes key_modes;
    gtkpyte::Selection selection;
    GdkRGBA selection_color = kSelectionColor;
    // Name of the built-in scheme last applied; empty once any single colour
    // is set by hand, because the result is no longer that scheme.
    std::string scheme = "xterm";

    PangoFontDescription *font = nullptr;
    int cell_w = 8;
    int cell_h = 16;
    // False when glyph advances in the primary font are not all the same
    // whole number of pixels. Runs then cannot be laid out as one string and
    // still land on the grid, so every cell is positioned on its own.
    bool uniform_advance = true;

    int cols = 80;
    int rows = 24;

    bool cursor_visible = true;
    bool bracketed_paste = false;
    bool focused = false;

    double scroll_accum = 0.0;  // wheel steps (touchpad: cell heights) not yet acted on

    // Mouse reporting. mouse_button is the button whose press was sent to the
    // application, so its release goes there too even if Shift is pressed in
    // between; -1 when none. The last cell sent dedupes motion reports.
    gtkpyte::MouseModes mouse;
    int mouse_button = -1;
    int mouse_last_col = -1;
    int mouse_last_row = -1;
    // Pointer position in widget coordinates, kept by a motion controller:
    // scroll events carry no widget-relative position of their own.
    double pointer_x = 0.0;
    double pointer_y = 0.0;

    // Selection drag: where the pointer is (widget coordinates), and the
    // auto-scroll timer running while it is outside the widget vertically.
    double drag_x = 0.0;
    double drag_y = 0.0;
    guint autoscroll_id = 0;
    int autoscroll_dir = 0;

    // GtkScrollable. Units are lines: value 0 is the oldest retained row and
    // upper - page_size is the live screen.
    GtkAdjustment *hadjustment = nullptr;
    GtkAdjustment *vadjustment = nullptr;
    gulong vadjustment_handler = 0;
    GtkScrollablePolicy hscroll_policy = GTK_SCROLL_MINIMUM;
    GtkScrollablePolicy vscroll_policy = GTK_SCROLL_MINIMUM;
    bool syncing_adjustment = false;

    ~State() {
        if (font != nullptr) {
            pango_font_description_free(font);
        }
    }
};

}  // namespace

struct _GtkpyteTerminal {
    GtkWidget parent_instance;
    State *st;
};

G_DEFINE_FINAL_TYPE_WITH_CODE(GtkpyteTerminal, gtkpyte_terminal, GTK_TYPE_WIDGET,
                              G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, nullptr))

namespace {

// --- outbound bytes ---------------------------------------------------------

void emit_commit(GtkpyteTerminal *self, const std::string &bytes) {
    if (bytes.empty()) {
        return;
    }
    GBytes *data = g_bytes_new(bytes.data(), bytes.size());
    g_signal_emit(self, g_signals[SIGNAL_COMMIT], 0, data);
    g_bytes_unref(data);
}

// --- scrollback and the adjustment -----------------------------------------

// Push the emulator's scrollback state into the vertical adjustment. Guarded,
// because setting the value fires value-changed, which would otherwise come
// straight back and move the history position being read.
void sync_adjustment(GtkpyteTerminal *self) {
    State *st = self->st;
    if (st->vadjustment == nullptr) {
        return;
    }
    const int history = st->screen.history_size();
    const double page = static_cast<double>(std::max(1, st->rows));
    st->syncing_adjustment = true;
    gtk_adjustment_configure(st->vadjustment,
                             static_cast<double>(history - st->screen.history_position()),
                             0.0, static_cast<double>(history) + page, 1.0, page, page);
    st->syncing_adjustment = false;
}

void on_vadjustment_changed(GtkAdjustment *adjustment, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    State *st = self->st;
    if (st->syncing_adjustment) {
        return;
    }
    // The bar counts down from the oldest row; the emulator counts up from the
    // live screen. Bottom of the bar is live, which is where a terminal sits.
    const int value = static_cast<int>(std::lround(gtk_adjustment_get_value(adjustment)));
    st->screen.scroll_history_to(st->screen.history_size() - value);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void set_hadjustment(GtkpyteTerminal *self, GtkAdjustment *adjustment) {
    State *st = self->st;
    if (adjustment == st->hadjustment) {
        return;
    }
    if (st->hadjustment != nullptr) {
        g_object_unref(st->hadjustment);
    }
    // No horizontal scrolling: the grid always fits the width. Hold a zero
    // adjustment anyway, because GtkScrolledWindow expects one.
    if (adjustment == nullptr) {
        adjustment = gtk_adjustment_new(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    }
    st->hadjustment = GTK_ADJUSTMENT(g_object_ref_sink(adjustment));
    g_object_notify(G_OBJECT(self), "hadjustment");
}

void set_vadjustment(GtkpyteTerminal *self, GtkAdjustment *adjustment) {
    State *st = self->st;
    if (adjustment == st->vadjustment && adjustment != nullptr) {
        return;
    }
    if (st->vadjustment != nullptr) {
        g_signal_handler_disconnect(st->vadjustment, st->vadjustment_handler);
        g_object_unref(st->vadjustment);
    }
    if (adjustment == nullptr) {
        adjustment = gtk_adjustment_new(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    }
    st->vadjustment = GTK_ADJUSTMENT(g_object_ref_sink(adjustment));
    st->vadjustment_handler = g_signal_connect(st->vadjustment, "value-changed",
                                               G_CALLBACK(on_vadjustment_changed), self);
    sync_adjustment(self);
    g_object_notify(G_OBJECT(self), "vadjustment");
}

void scroll_history(GtkpyteTerminal *self, int lines) {
    State *st = self->st;
    if (lines < 0) {
        st->screen.scroll_history_up(-lines);
    } else if (lines > 0) {
        st->screen.scroll_history_down(lines);
    }
    sync_adjustment(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void scroll_to_live(GtkpyteTerminal *self) {
    State *st = self->st;
    if (st->screen.viewing_history()) {
        st->screen.scroll_history_to_bottom();
        sync_adjustment(self);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

// --- font metrics -----------------------------------------------------------

// Every layout carries this. A terminal draws one character per cell, and a
// programming font's ligatures (JetBrains Mono, Fira Code, Cascadia) would
// otherwise fuse "==" or "!=" into one glyph spanning two cells -- the text
// stops matching the grid, and the source on screen stops matching the file.
// VTE turns the same features off.
PangoAttrList *base_attrs() {
    PangoAttrList *list = pango_attr_list_new();
    pango_attr_list_insert(list, pango_attr_font_features_new("liga 0, clig 0, calt 0, dlig 0"));
    return list;
}

void reset_attrs(PangoLayout *layout) {
    PangoAttrList *list = base_attrs();
    pango_layout_set_attributes(layout, list);
    pango_attr_list_unref(list);
}

PangoLayout *make_layout(GtkpyteTerminal *self) {
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), nullptr);
    pango_layout_set_font_description(layout, self->st->font);
    reset_attrs(layout);
    return layout;
}

// Cell size comes from laying out real text with the widget's own Pango
// context, rather than from font metrics: that way the number reflects the
// hinting and glyph-position rounding GTK will actually render with.
void measure_cells(GtkpyteTerminal *self) {
    State *st = self->st;
    PangoLayout *layout = make_layout(self);

    int w_wide = 0;
    int h = 0;
    pango_layout_set_text(layout, "MMMMMMMMMM", -1);
    pango_layout_get_pixel_size(layout, &w_wide, &h);

    int w_narrow = 0;
    pango_layout_set_text(layout, "iiiiiiiiii", -1);
    pango_layout_get_pixel_size(layout, &w_narrow, nullptr);

    int w_mixed = 0;
    pango_layout_set_text(layout, "W.i W.i W.i W.i W.", -1);  // 18 glyphs
    pango_layout_get_pixel_size(layout, &w_mixed, nullptr);

    g_object_unref(layout);

    st->cell_w = std::max(1, static_cast<int>(std::lround(w_wide / 10.0)));
    st->cell_h = std::max(1, h);
    st->uniform_advance = (w_wide == st->cell_w * 10) && (w_narrow == w_wide) &&
                          (w_mixed == st->cell_w * 18);
}

// --- grid -------------------------------------------------------------------

void update_grid(GtkpyteTerminal *self, int width, int height) {
    State *st = self->st;
    const int cols = std::max(1, width / st->cell_w);
    const int rows = std::max(1, height / st->cell_h);
    if (cols == st->cols && rows == st->rows) {
        return;
    }
    st->cols = cols;
    st->rows = rows;
    // Emulator first, so it lays out what it holds to the new shape; then the
    // producer, so the application redraws. Skipping the second half is the
    // usual reason a program does not repaint after a window resize.
    st->screen.resize(cols, rows);
    sync_adjustment(self);
    g_signal_emit(self, g_signals[SIGNAL_RESIZED], 0, cols, rows);
}

void handle_mode(GtkpyteTerminal *self, int mode, bool set, bool private_mode) {
    if (!private_mode) {
        return;
    }
    State *st = self->st;
    if (st->mouse.apply(mode, set)) {
        if (!st->mouse.reporting()) {
            st->mouse_button = -1;
        }
        // An arrow while the application has the mouse, the I-beam while
        // the mouse selects text.
        gtk_widget_set_cursor_from_name(GTK_WIDGET(self),
                                        st->mouse.reporting() ? "default" : "text");
        return;
    }
    switch (mode) {
        case kModeCursorKeys:
            st->key_modes.application_cursor = set;
            break;
        case kModeCursorVisible:
            st->cursor_visible = set;
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case kModeBracketedPaste:
            st->bracketed_paste = set;
            break;
        default:
            break;
    }
}

// --- painting ---------------------------------------------------------------

void append_utf8(std::string &out, char32_t cp) {
    char buf[6];
    const int n = g_unichar_to_utf8(static_cast<gunichar>(cp), buf);
    out.append(buf, static_cast<std::size_t>(n));
}

PangoAttrList *attrs_for(const pyte::Attrs &a) {
    PangoAttrList *list = base_attrs();
    if (a.bold) {
        pango_attr_list_insert(list, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    }
    if (a.italic) {
        pango_attr_list_insert(list, pango_attr_style_new(PANGO_STYLE_ITALIC));
    }
    if (a.underline) {
        pango_attr_list_insert(list, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
    }
    if (a.strikethrough) {
        pango_attr_list_insert(list, pango_attr_strikethrough_new(TRUE));
    }
    return list;
}

void draw_text_at(GtkSnapshot *snapshot, PangoLayout *layout, const std::string &text,
                  float x, float y, const GdkRGBA &fg) {
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
    gtk_snapshot_save(snapshot);
    graphene_point_t origin = GRAPHENE_POINT_INIT(x, y);
    gtk_snapshot_translate(snapshot, &origin);
    gtk_snapshot_append_layout(snapshot, layout, &fg);
    gtk_snapshot_restore(snapshot);
}

void paint_row(GtkpyteTerminal *self, GtkSnapshot *snapshot, PangoLayout *layout, int y) {
    State *st = self->st;
    const int cols = std::min(st->cols, st->screen.cols());
    const float cw = static_cast<float>(st->cell_w);
    const float ch = static_cast<float>(st->cell_h);
    const float top = static_cast<float>(y) * ch;

    int x = 0;
    while (x < cols) {
        if (st->screen.at(x, y).wide_continuation) {
            ++x;
            continue;
        }

        // A run of cells sharing attributes: one background rectangle, and
        // one layout when the run is safe to draw as a single string.
        const pyte::Attrs attrs = st->screen.at(x, y).attrs;
        const int run_start = x;
        bool plain_ascii = true;
        bool blank = true;
        while (x < cols) {
            const pyte::Cell &cell = st->screen.at(x, y);
            if (!cell.wide_continuation && cell.attrs != attrs) {
                break;
            }
            if (cell.wide_continuation || cell.ch < 0x20 || cell.ch > 0x7E) {
                plain_ascii = false;
            }
            if (!cell.wide_continuation && cell.ch != U' ' && cell.ch != U'\0') {
                blank = false;
            }
            ++x;
        }
        const int span = x - run_start;

        GdkRGBA fg = st->palette.color(attrs.fg, st->palette.foreground(), attrs.bold);
        GdkRGBA bg = st->palette.color(attrs.bg, st->palette.background());
        if (attrs.reverse) {
            std::swap(fg, bg);
        }

        if (!gdk_rgba_equal(&bg, &st->palette.background())) {
            graphene_rect_t r = GRAPHENE_RECT_INIT(static_cast<float>(run_start) * cw, top,
                                                   static_cast<float>(span) * cw, ch);
            gtk_snapshot_append_color(snapshot, &bg, &r);
        }

        if (blank && !attrs.underline && !attrs.strikethrough) {
            continue;
        }

        PangoAttrList *list = attrs_for(attrs);
        pango_layout_set_attributes(layout, list);
        pango_attr_list_unref(list);

        if (st->uniform_advance && plain_ascii) {
            // The common case: narrow glyphs the primary font certainly has,
            // with whole-pixel advances equal to the cell width.
            std::string text;
            text.reserve(static_cast<std::size_t>(span));
            for (int i = run_start; i < x; ++i) {
                const char32_t cp = st->screen.at(i, y).ch;
                text += (cp == U'\0') ? ' ' : static_cast<char>(cp);
            }
            draw_text_at(snapshot, layout, text, static_cast<float>(run_start) * cw, top, fg);
            continue;
        }

        // Fallback glyphs (box drawing, braille, CJK) come from substitute
        // fonts whose advances differ from the primary's, so a run drawn as
        // one string drifts off the grid. Place each cell on its own column;
        // a double-width glyph is drawn once from its left cell.
        for (int i = run_start; i < x; ++i) {
            const pyte::Cell &cell = st->screen.at(i, y);
            if (cell.wide_continuation) {
                continue;
            }
            const char32_t cp = (cell.ch == U'\0') ? U' ' : cell.ch;
            if (cp == U' ' && !attrs.underline && !attrs.strikethrough) {
                continue;
            }
            std::string glyph;
            append_utf8(glyph, cp);
            draw_text_at(snapshot, layout, glyph, static_cast<float>(i) * cw, top, fg);
        }
    }
    reset_attrs(layout);
}

void paint_selection(GtkpyteTerminal *self, GtkSnapshot *snapshot) {
    State *st = self->st;
    if (!st->selection.active || st->selection.empty()) {
        return;
    }
    const int top = st->screen.view_top();
    const int rows = std::min(st->rows, st->screen.rows());
    for (int y = 0; y < rows; ++y) {
        int from = 0;
        int to = 0;
        if (!st->selection.span_on_line(top + y, st->cols, from, to)) {
            continue;
        }
        const graphene_rect_t r = GRAPHENE_RECT_INIT(
            static_cast<float>(from * st->cell_w), static_cast<float>(y * st->cell_h),
            static_cast<float>((to - from) * st->cell_w), static_cast<float>(st->cell_h));
        gtk_snapshot_append_color(snapshot, &st->selection_color, &r);
    }
}

void paint_cursor(GtkpyteTerminal *self, GtkSnapshot *snapshot, PangoLayout *layout) {
    State *st = self->st;
    // No cursor while scrolled back: it marks where output lands, and that is
    // not on screen.
    if (!st->cursor_visible || st->screen.viewing_history()) {
        return;
    }
    const int cx = st->screen.cursor_x();
    const int cy = st->screen.cursor_y();
    if (cx < 0 || cx >= st->screen.cols() || cy < 0 || cy >= st->screen.rows()) {
        return;
    }
    const int span =
        (cx + 1 < st->screen.cols() && st->screen.at(cx + 1, cy).wide_continuation) ? 2 : 1;
    const graphene_rect_t r = GRAPHENE_RECT_INIT(
        static_cast<float>(cx * st->cell_w), static_cast<float>(cy * st->cell_h),
        static_cast<float>(span * st->cell_w), static_cast<float>(st->cell_h));

    if (!st->focused) {
        // Outline when unfocused: the cheapest signal that keystrokes are
        // going somewhere else.
        GskRoundedRect rr;
        gsk_rounded_rect_init_from_rect(&rr, &r, 0.0f);
        const float widths[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const GdkRGBA colors[4] = {st->palette.cursor(), st->palette.cursor(),
                                   st->palette.cursor(), st->palette.cursor()};
        gtk_snapshot_append_border(snapshot, &rr, widths, colors);
        return;
    }

    gtk_snapshot_append_color(snapshot, &st->palette.cursor(), &r);
    const pyte::Cell &cell = st->screen.at(cx, cy);
    if (cell.ch != U'\0' && cell.ch != U' ') {
        PangoAttrList *list = attrs_for(cell.attrs);
        pango_layout_set_attributes(layout, list);
        pango_attr_list_unref(list);
        std::string glyph;
        append_utf8(glyph, cell.ch);
        draw_text_at(snapshot, layout, glyph, r.origin.x, r.origin.y, st->palette.background());
        reset_attrs(layout);
    }
}

// --- selection --------------------------------------------------------------

void notify_selection_changed(GtkpyteTerminal *self) {
    g_signal_emit(self, g_signals[SIGNAL_SELECTION_CHANGED], 0);
}

// Pixel to absolute line and grid column. Columns clamp to cols, not cols - 1:
// the end is exclusive, so "past the last character" has to be expressible,
// or the last character on a row can never be included.
void point_to_cell(GtkpyteTerminal *self, double px, double py, int &line, int &col) {
    State *st = self->st;
    const int rows = std::min(st->rows, st->screen.rows());
    const int row = std::clamp(static_cast<int>(std::floor(py / st->cell_h)), 0, rows - 1);
    col = std::clamp(static_cast<int>(std::lround(px / st->cell_w)), 0, st->cols);
    line = st->screen.view_top() + row;
}

// The anchor uses floor rather than rounding: a press inside a cell anchors at
// that cell's left edge, so a drag to the right from anywhere in it includes it.
void point_to_anchor(GtkpyteTerminal *self, double px, double py, int &line, int &col) {
    State *st = self->st;
    point_to_cell(self, px, py, line, col);
    col = std::clamp(static_cast<int>(std::floor(px / st->cell_w)), 0, st->cols);
}

void extend_focus_to(GtkpyteTerminal *self, double px, double py) {
    State *st = self->st;
    point_to_cell(self, px, py, st->selection.focus_line, st->selection.focus_col);
    st->selection.active = true;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void set_selection(GtkpyteTerminal *self, int start_line, int start_col, int end_line,
                   int end_col) {
    State *st = self->st;
    st->selection.anchor_line = start_line;
    st->selection.anchor_col = start_col;
    st->selection.focus_line = end_line;
    st->selection.focus_col = end_col;
    st->selection.active = true;
    st->selection.dragging = false;
    notify_selection_changed(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

std::string selected_text(GtkpyteTerminal *self) {
    State *st = self->st;
    if (!st->selection.active || st->selection.empty()) {
        return std::string();
    }
    int start_line = 0;
    int start_col = 0;
    int end_line = 0;
    int end_col = 0;
    st->selection.normalized(start_line, start_col, end_line, end_col);
    return st->screen.text_in_range(start_line, start_col, end_line, end_col);
}

// On X11 and Wayland the primary selection follows the highlight. That is the
// platform's own convention and costs nothing, unlike the clipboard, which is
// only written by an explicit copy.
void publish_primary(GtkpyteTerminal *self) {
    const std::string text = selected_text(self);
    if (!text.empty()) {
        gdk_clipboard_set_text(gtk_widget_get_primary_clipboard(GTK_WIDGET(self)), text.c_str());
    }
}

void stop_autoscroll(GtkpyteTerminal *self) {
    State *st = self->st;
    st->autoscroll_dir = 0;
    if (st->autoscroll_id != 0) {
        g_source_remove(st->autoscroll_id);
        st->autoscroll_id = 0;
    }
}

gboolean on_autoscroll_tick(gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    State *st = self->st;
    const double height = static_cast<double>(gtk_widget_get_height(GTK_WIDGET(self)));
    const int overshoot = static_cast<int>(st->autoscroll_dir < 0 ? -st->drag_y
                                                                  : st->drag_y - height);
    const int step = gtkpyte::autoscroll_step(overshoot, st->cell_h, st->rows);

    const int before = st->screen.view_top();
    scroll_history(self, st->autoscroll_dir * step);

    // Extend to the row just revealed. Without this the selection stops
    // growing the moment the pointer leaves the widget, which reads as a
    // dropped drag.
    const double edge_y = st->autoscroll_dir < 0 ? 0.0 : height - 1.0;
    extend_focus_to(self, st->drag_x, edge_y);

    if (st->screen.view_top() == before) {
        // Pinned at the top of history or at the live screen.
        st->autoscroll_id = 0;
        st->autoscroll_dir = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void update_autoscroll(GtkpyteTerminal *self) {
    State *st = self->st;
    const double height = static_cast<double>(gtk_widget_get_height(GTK_WIDGET(self)));
    const int dir = (st->drag_y < 0.0) ? -1 : (st->drag_y > height ? 1 : 0);
    if (dir == 0) {
        stop_autoscroll(self);
        return;
    }
    st->autoscroll_dir = dir;
    if (st->autoscroll_id == 0) {
        st->autoscroll_id = g_timeout_add(kAutoScrollIntervalMs, on_autoscroll_tick, self);
    }
}

void select_word_at(GtkpyteTerminal *self, int line, int col) {
    State *st = self->st;
    const int row = line - st->screen.view_top();
    if (row < 0 || row >= st->screen.rows()) {
        return;
    }
    col = std::clamp(col, 0, st->screen.cols() - 1);
    int start = col;
    int end = col + 1;
    gtkpyte::word_bounds(st->screen, row, col, start, end);
    set_selection(self, line, start, line, end);
}

// Primary-button presses: GtkGestureClick counts the clicks for us, so single,
// double and triple are just n_press.
void on_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    State *st = self->st;
    gtk_widget_grab_focus(GTK_WIDGET(self));

    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_PRIMARY) {
        return;
    }

    int line = 0;
    int col = 0;

    if (n_press >= 3) {
        point_to_cell(self, x, y, line, col);
        set_selection(self, line, 0, line, st->cols);
        publish_primary(self);
        return;
    }
    if (n_press == 2) {
        point_to_anchor(self, x, y, line, col);
        select_word_at(self, line, col);
        publish_primary(self);
        return;
    }

    const GdkModifierType mods =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    st->drag_x = x;
    st->drag_y = y;

    if ((mods & GDK_SHIFT_MASK) && st->selection.active) {
        // Shift-click extends the existing selection, as on any text surface.
        extend_focus_to(self, x, y);
        st->selection.dragging = true;
        return;
    }

    const bool had_selection = st->selection.active && !st->selection.empty();
    point_to_anchor(self, x, y, line, col);
    st->selection.clear();
    st->selection.anchor_line = st->selection.focus_line = line;
    st->selection.anchor_col = st->selection.focus_col = col;
    st->selection.dragging = true;
    if (had_selection) {
        notify_selection_changed(self);
    }
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void on_click_released(GtkGestureClick *gesture, int, double, double, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_MIDDLE) {
        return;
    }
    // Middle click pastes the primary selection.
    GdkClipboard *primary = gtk_widget_get_primary_clipboard(GTK_WIDGET(self));
    gdk_clipboard_read_text_async(
        primary, nullptr,
        [](GObject *source, GAsyncResult *result, gpointer user_data) {
            auto *term = GTKPYTE_TERMINAL(user_data);
            char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, nullptr);
            if (text != nullptr) {
                gtkpyte_terminal_paste(term, text);
                g_free(text);
            }
            g_object_unref(term);
        },
        g_object_ref(self));
}

void on_drag_update(GtkGestureDrag *gesture, double dx, double dy, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    State *st = self->st;
    if (!st->selection.dragging) {
        return;
    }
    double sx = 0.0;
    double sy = 0.0;
    gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
    st->drag_x = sx + dx;
    st->drag_y = sy + dy;
    extend_focus_to(self, st->drag_x, st->drag_y);
    update_autoscroll(self);
}

void on_drag_end(GtkGestureDrag *, double, double, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    State *st = self->st;
    if (!st->selection.dragging) {
        return;
    }
    st->selection.dragging = false;
    stop_autoscroll(self);

    // A click that never moved is not a selection; it is how the last one
    // gets dismissed.
    if (st->selection.empty()) {
        st->selection.clear();
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return;
    }
    st->selection.active = true;
    publish_primary(self);
    notify_selection_changed(self);
}

// --- scroll and keyboard ----------------------------------------------------

gtkpyte::MouseMods mouse_mods(GdkModifierType state) {
    gtkpyte::MouseMods m;
    m.shift = (state & GDK_SHIFT_MASK) != 0;
    m.alt = (state & GDK_ALT_MASK) != 0;
    m.ctrl = (state & GDK_CONTROL_MASK) != 0;
    return m;
}

// Pixel to the grid cell a report names. Unlike selection, the column clamps
// to the last cell: a report has no "past the end" position.
void pixel_to_report_cell(GtkpyteTerminal *self, double x, double y, int &col, int &row) {
    State *st = self->st;
    col = std::clamp(static_cast<int>(std::floor(x / st->cell_w)), 0, std::max(0, st->cols - 1));
    row = std::clamp(static_cast<int>(std::floor(y / st->cell_h)), 0,
                     std::max(0, std::min(st->rows, st->screen.rows()) - 1));
}

void send_mouse(GtkpyteTerminal *self, gtkpyte::MouseAction action, int button, int col,
                int row, GdkModifierType state) {
    State *st = self->st;
    emit_commit(self, gtkpyte::encode_mouse(st->mouse, action, button, col, row,
                                            mouse_mods(state)));
}

// --- mouse reporting --------------------------------------------------------
//
// A capture-phase drag gesture on every button sees presses before the
// selection gestures do. When the application has asked for the mouse and
// Shift is not held, it claims the sequence -- press, drag and release all go
// to the application and selection never sees them. Otherwise it denies the
// sequence and the event carries on to selection untouched. Shift as the
// override is xterm's convention, and what every full-screen app expects.

int gdk_to_xterm_button(guint button) {
    switch (button) {
        case GDK_BUTTON_PRIMARY:   return gtkpyte::kMouseLeft;
        case GDK_BUTTON_MIDDLE:    return gtkpyte::kMouseMiddle;
        case GDK_BUTTON_SECONDARY: return gtkpyte::kMouseRight;
        default:                   return -1;
    }
}

void on_report_begin(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    State *st = self->st;
    const GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    const int button =
        gdk_to_xterm_button(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)));

    if (!st->mouse.reporting() || (state & GDK_SHIFT_MASK) || button < 0) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_widget_grab_focus(GTK_WIDGET(self));

    int col = 0;
    int row = 0;
    pixel_to_report_cell(self, x, y, col, row);
    st->mouse_button = button;
    st->mouse_last_col = col;
    st->mouse_last_row = row;
    send_mouse(self, gtkpyte::MouseAction::Press, button, col, row, state);
}

void on_report_update(GtkGestureDrag *gesture, double dx, double dy, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    State *st = self->st;
    if (st->mouse_button < 0) {
        return;
    }
    double sx = 0.0;
    double sy = 0.0;
    gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
    st->pointer_x = sx + dx;
    st->pointer_y = sy + dy;
    int col = 0;
    int row = 0;
    pixel_to_report_cell(self, sx + dx, sy + dy, col, row);
    if (col == st->mouse_last_col && row == st->mouse_last_row) {
        return;  // one report per cell crossed, not per pixel
    }
    st->mouse_last_col = col;
    st->mouse_last_row = row;
    send_mouse(self, gtkpyte::MouseAction::Motion, st->mouse_button, col, row,
               gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)));
}

void on_report_end(GtkGestureDrag *gesture, double dx, double dy, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    State *st = self->st;
    if (st->mouse_button < 0) {
        return;
    }
    double sx = 0.0;
    double sy = 0.0;
    gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
    int col = 0;
    int row = 0;
    pixel_to_report_cell(self, sx + dx, sy + dy, col, row);
    const int button = st->mouse_button;
    st->mouse_button = -1;
    send_mouse(self, gtkpyte::MouseAction::Release, button, col, row,
               gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)));
}

// Pointer tracking for the wheel, plus motion with no button held, which only
// any-motion mode (1003) reports.
void on_pointer_motion(GtkEventControllerMotion *controller, double x, double y, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    State *st = self->st;
    st->pointer_x = x;
    st->pointer_y = y;
    if (st->mouse.tracking != gtkpyte::MouseModes::kAnyMotion || st->mouse_button >= 0) {
        return;  // a held button's motion is reported by the drag gesture
    }
    int col = 0;
    int row = 0;
    pixel_to_report_cell(self, x, y, col, row);
    if (col == st->mouse_last_col && row == st->mouse_last_row) {
        return;
    }
    st->mouse_last_col = col;
    st->mouse_last_row = row;
    send_mouse(self, gtkpyte::MouseAction::Motion, -1, col, row,
               gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller)));
}

gboolean on_scroll(GtkEventControllerScroll *controller, double, double dy, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    State *st = self->st;
    const GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    const bool shift = (state & GDK_SHIFT_MASK) != 0;
    const bool reporting = st->mouse.reporting() && !shift;
    const bool alt_scroll = !st->mouse.reporting() && st->screen.alternate_screen() &&
                            st->mouse.alternate_scroll;

    // Full-screen applications have no scrollback, and with neither mouse
    // reporting nor alternate-scroll there is nothing for the wheel to do.
    if (!reporting && !alt_scroll && st->screen.alternate_screen()) {
        return FALSE;
    }

    // One step per wheel notch; a touchpad makes a step per cell height of
    // travel. Fractions carry over so slow, smooth scrolling still lands.
    const bool wheel = gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_WHEEL;
    st->scroll_accum += wheel ? dy : dy / static_cast<double>(st->cell_h);
    const int steps = static_cast<int>(st->scroll_accum);  // truncates toward zero
    if (steps == 0) {
        return TRUE;
    }
    st->scroll_accum -= steps;
    const int count = std::abs(steps);

    if (reporting) {
        // xterm reports one wheel "press" per notch, at the pointer's cell.
        int col = 0;
        int row = 0;
        pixel_to_report_cell(self, st->pointer_x, st->pointer_y, col, row);
        const int button = steps < 0 ? gtkpyte::kWheelUp : gtkpyte::kWheelDown;
        for (int i = 0; i < count; ++i) {
            send_mouse(self, gtkpyte::MouseAction::Press, button, col, row, state);
        }
        return TRUE;
    }

    const int lines = wheel ? count * static_cast<int>(kLinesPerWheelStep) : count;
    if (alt_scroll) {
        // Cursor keys, honouring DECCKM, which is what a pager or editor on
        // the alternate screen reads.
        const guint key = steps < 0 ? GDK_KEY_Up : GDK_KEY_Down;
        const std::string one =
            gtkpyte::encode_key(key, static_cast<GdkModifierType>(0), st->key_modes);
        std::string bytes;
        for (int i = 0; i < lines; ++i) {
            bytes += one;
        }
        emit_commit(self, bytes);
        return TRUE;
    }

    scroll_history(self, steps < 0 ? -lines : lines);
    return TRUE;
}

gboolean on_key_pressed(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state,
                        gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    State *st = self->st;
    const GdkModifierType mods =
        static_cast<GdkModifierType>(state & gtk_accelerator_get_default_mod_mask());
    const bool ctrl_shift = (mods & GDK_CONTROL_MASK) && (mods & GDK_SHIFT_MASK) &&
                            !(mods & GDK_ALT_MASK);

    // Terminal-owned shortcuts. Ctrl+Shift so the plain Ctrl combinations stay
    // with the application: Ctrl+C is SIGINT and Ctrl+V a literal-next, and a
    // terminal that takes either for the clipboard has broken its own job.
    if (ctrl_shift) {
        switch (keyval) {
            case GDK_KEY_V:
            case GDK_KEY_v:
                gtkpyte_terminal_paste_clipboard(self);
                return TRUE;
            case GDK_KEY_C:
            case GDK_KEY_c:
                // Swallowed even with nothing selected, or it falls through
                // as ^C to the shell.
                gtkpyte_terminal_copy_clipboard(self);
                return TRUE;
            case GDK_KEY_A:
            case GDK_KEY_a:
                gtkpyte_terminal_select_all(self);
                return TRUE;
            default:
                break;
        }
    }

    // Scrollback paging belongs to the widget, but only with Shift held and
    // only off the alternate screen: full-screen apps want the plain keys.
    if ((mods & GDK_SHIFT_MASK) && !st->screen.alternate_screen()) {
        if (keyval == GDK_KEY_Page_Up) {
            scroll_history(self, -std::max(1, st->rows / 2));
            return TRUE;
        }
        if (keyval == GDK_KEY_Page_Down) {
            scroll_history(self, std::max(1, st->rows / 2));
            return TRUE;
        }
    }

    const std::string bytes = gtkpyte::encode_key(keyval, mods, st->key_modes);
    if (bytes.empty()) {
        return FALSE;
    }

    // Typing returns to the live screen, as output does.
    scroll_to_live(self);
    emit_commit(self, bytes);
    return TRUE;
}

void on_focus_enter(GtkEventControllerFocus *, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    self->st->focused = true;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void on_focus_leave(GtkEventControllerFocus *, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    self->st->focused = false;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void on_realize(GtkWidget *widget, gpointer) {
    // The widget's Pango context only carries the display's font options once
    // it is on a display, so metrics taken in init can be off by a pixel.
    measure_cells(GTKPYTE_TERMINAL(widget));
    gtk_widget_queue_resize(widget);
}

void on_clipboard_text(GObject *source, GAsyncResult *result, gpointer data) {
    auto *self = GTKPYTE_TERMINAL(data);
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, nullptr);
    if (text != nullptr) {
        gtkpyte_terminal_paste(self, text);
        g_free(text);
    }
    g_object_unref(self);
}

// --- GtkWidget vfuncs -------------------------------------------------------

void gtkpyte_terminal_measure(GtkWidget *widget, GtkOrientation orientation, int,
                              int *minimum, int *natural, int *minimum_baseline,
                              int *natural_baseline) {
    State *st = GTKPYTE_TERMINAL(widget)->st;
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        *minimum = st->cell_w * 10;
        *natural = st->cell_w * 80;
    } else {
        *minimum = st->cell_h * 3;
        *natural = st->cell_h * 24;
    }
    *minimum_baseline = -1;
    *natural_baseline = -1;
}

void gtkpyte_terminal_size_allocate(GtkWidget *widget, int width, int height, int) {
    update_grid(GTKPYTE_TERMINAL(widget), width, height);
}

void gtkpyte_terminal_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    auto *self = GTKPYTE_TERMINAL(widget);
    State *st = self->st;

    const graphene_rect_t all = GRAPHENE_RECT_INIT(
        0.0f, 0.0f, static_cast<float>(gtk_widget_get_width(widget)),
        static_cast<float>(gtk_widget_get_height(widget)));
    gtk_snapshot_append_color(snapshot, &st->palette.background(), &all);

    PangoLayout *layout = make_layout(self);
    const int rows = std::min(st->rows, st->screen.rows());
    for (int y = 0; y < rows; ++y) {
        paint_row(self, snapshot, layout, y);
    }
    paint_selection(self, snapshot);
    paint_cursor(self, snapshot, layout);
    g_object_unref(layout);

    st->screen.clear_dirty();
}

// --- GObject ----------------------------------------------------------------

void gtkpyte_terminal_get_property(GObject *object, guint prop_id, GValue *value,
                                   GParamSpec *pspec) {
    State *st = GTKPYTE_TERMINAL(object)->st;
    switch (prop_id) {
        case PROP_HADJUSTMENT:
            g_value_set_object(value, st->hadjustment);
            break;
        case PROP_VADJUSTMENT:
            g_value_set_object(value, st->vadjustment);
            break;
        case PROP_HSCROLL_POLICY:
            g_value_set_enum(value, st->hscroll_policy);
            break;
        case PROP_VSCROLL_POLICY:
            g_value_set_enum(value, st->vscroll_policy);
            break;
        case PROP_FONT:
            g_value_take_string(value, pango_font_description_to_string(st->font));
            break;
        case PROP_SCROLLBACK_LINES:
            g_value_set_int(value, st->screen.max_scrollback());
            break;
        case PROP_COLOR_SCHEME:
            g_value_set_string(value, st->scheme.empty() ? nullptr : st->scheme.c_str());
            break;
        case PROP_FOREGROUND_COLOR:
            g_value_set_boxed(value, &st->palette.foreground());
            break;
        case PROP_BACKGROUND_COLOR:
            g_value_set_boxed(value, &st->palette.background());
            break;
        case PROP_CURSOR_COLOR:
            g_value_set_boxed(value, &st->palette.cursor());
            break;
        case PROP_SELECTION_COLOR:
            g_value_set_boxed(value, &st->selection_color);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

void gtkpyte_terminal_set_property(GObject *object, guint prop_id, const GValue *value,
                                   GParamSpec *pspec) {
    auto *self = GTKPYTE_TERMINAL(object);
    State *st = self->st;
    switch (prop_id) {
        case PROP_HADJUSTMENT:
            set_hadjustment(self, GTK_ADJUSTMENT(g_value_get_object(value)));
            break;
        case PROP_VADJUSTMENT:
            set_vadjustment(self, GTK_ADJUSTMENT(g_value_get_object(value)));
            break;
        case PROP_HSCROLL_POLICY:
            st->hscroll_policy = static_cast<GtkScrollablePolicy>(g_value_get_enum(value));
            break;
        case PROP_VSCROLL_POLICY:
            st->vscroll_policy = static_cast<GtkScrollablePolicy>(g_value_get_enum(value));
            break;
        case PROP_FONT:
            gtkpyte_terminal_set_font(self, g_value_get_string(value));
            break;
        case PROP_SCROLLBACK_LINES:
            gtkpyte_terminal_set_scrollback(self, g_value_get_int(value));
            break;
        case PROP_COLOR_SCHEME:
            gtkpyte_terminal_set_color_scheme(self, g_value_get_string(value));
            break;
        case PROP_FOREGROUND_COLOR:
            gtkpyte_terminal_set_color_foreground(
                self, static_cast<const GdkRGBA *>(g_value_get_boxed(value)));
            break;
        case PROP_BACKGROUND_COLOR:
            gtkpyte_terminal_set_color_background(
                self, static_cast<const GdkRGBA *>(g_value_get_boxed(value)));
            break;
        case PROP_CURSOR_COLOR:
            gtkpyte_terminal_set_color_cursor(
                self, static_cast<const GdkRGBA *>(g_value_get_boxed(value)));
            break;
        case PROP_SELECTION_COLOR:
            gtkpyte_terminal_set_color_selection(
                self, static_cast<const GdkRGBA *>(g_value_get_boxed(value)));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

void gtkpyte_terminal_dispose(GObject *object) {
    auto *self = GTKPYTE_TERMINAL(object);
    State *st = self->st;
    stop_autoscroll(self);
    if (st->vadjustment != nullptr) {
        g_signal_handler_disconnect(st->vadjustment, st->vadjustment_handler);
        g_clear_object(&st->vadjustment);
    }
    g_clear_object(&st->hadjustment);
    G_OBJECT_CLASS(gtkpyte_terminal_parent_class)->dispose(object);
}

void gtkpyte_terminal_finalize(GObject *object) {
    auto *self = GTKPYTE_TERMINAL(object);
    delete self->st;
    self->st = nullptr;
    G_OBJECT_CLASS(gtkpyte_terminal_parent_class)->finalize(object);
}

}  // namespace

static void gtkpyte_terminal_class_init(GtkpyteTerminalClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->get_property = gtkpyte_terminal_get_property;
    object_class->set_property = gtkpyte_terminal_set_property;
    object_class->dispose = gtkpyte_terminal_dispose;
    object_class->finalize = gtkpyte_terminal_finalize;
    widget_class->measure = gtkpyte_terminal_measure;
    widget_class->size_allocate = gtkpyte_terminal_size_allocate;
    widget_class->snapshot = gtkpyte_terminal_snapshot;

    g_object_class_override_property(object_class, PROP_HADJUSTMENT, "hadjustment");
    g_object_class_override_property(object_class, PROP_VADJUSTMENT, "vadjustment");
    g_object_class_override_property(object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
    g_object_class_override_property(object_class, PROP_VSCROLL_POLICY, "vscroll-policy");

    constexpr auto kRW = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                                  G_PARAM_EXPLICIT_NOTIFY);
    g_props[PROP_FONT] = g_param_spec_string(
        "font", nullptr, "Font as a Pango description string, e.g. \"Monospace 11\"",
        kDefaultFont, kRW);
    g_props[PROP_SCROLLBACK_LINES] = g_param_spec_int(
        "scrollback-lines", nullptr, "Rows of history retained; 0 disables scrollback", 0,
        G_MAXINT, kDefaultScrollback, kRW);
    g_props[PROP_COLOR_SCHEME] = g_param_spec_string(
        "color-scheme", nullptr,
        "Built-in scheme last applied; NULL after colours are set individually", "xterm", kRW);
    g_props[PROP_FOREGROUND_COLOR] = g_param_spec_boxed(
        "foreground-color", nullptr, "Default text colour", GDK_TYPE_RGBA, kRW);
    g_props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed(
        "background-color", nullptr, "Default background colour", GDK_TYPE_RGBA, kRW);
    g_props[PROP_CURSOR_COLOR] = g_param_spec_boxed(
        "cursor-color", nullptr, "Cursor colour", GDK_TYPE_RGBA, kRW);
    g_props[PROP_SELECTION_COLOR] = g_param_spec_boxed(
        "selection-color", nullptr, "Selection highlight, painted over text; alpha expected",
        GDK_TYPE_RGBA, kRW);
    for (int i = PROP_FONT; i < N_PROPS; ++i) {
        g_object_class_install_property(object_class, static_cast<guint>(i), g_props[i]);
    }

    g_signals[SIGNAL_COMMIT] =
        g_signal_new("commit", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                     nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_BYTES);
    g_signals[SIGNAL_RESIZED] =
        g_signal_new("resized", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                     nullptr, nullptr, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
    g_signals[SIGNAL_SELECTION_CHANGED] =
        g_signal_new("selection-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                     nullptr, nullptr, nullptr, G_TYPE_NONE, 0);

    gtk_widget_class_set_css_name(widget_class, "gtkpyte-terminal");
}

static void gtkpyte_terminal_init(GtkpyteTerminal *self) {
    self->st = new State();
    State *st = self->st;
    st->font = pango_font_description_from_string(kDefaultFont);

    GtkWidget *widget = GTK_WIDGET(self);
    gtk_widget_set_focusable(widget, TRUE);
    gtk_widget_set_cursor_from_name(widget, "text");
    gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN);

    st->stream.set_responder([self](const std::string &reply) { emit_commit(self, reply); });
    st->stream.set_mode_handler(
        [self](int mode, bool set, bool private_mode) { handle_mode(self, mode, set, private_mode); });

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(widget, keys);

    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(on_focus_enter), self);
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(widget, focus);

    GtkEventController *scroll =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), self);
    gtk_widget_add_controller(widget, scroll);

    // Mouse reporting first, in the capture phase, so it can take a press
    // away from selection when the application has asked for the mouse.
    GtkGesture *report = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(report), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(report), GTK_PHASE_CAPTURE);
    g_signal_connect(report, "drag-begin", G_CALLBACK(on_report_begin), self);
    g_signal_connect(report, "drag-update", G_CALLBACK(on_report_update), self);
    g_signal_connect(report, "drag-end", G_CALLBACK(on_report_end), self);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(report));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_pointer_motion), self);
    gtk_widget_add_controller(widget, motion);

    // Any button: primary selects, middle pastes the primary selection.
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), self);
    g_signal_connect(click, "released", G_CALLBACK(on_click_released), self);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), self);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drag));

    g_signal_connect(widget, "realize", G_CALLBACK(on_realize), nullptr);

    set_hadjustment(self, nullptr);
    set_vadjustment(self, nullptr);
    measure_cells(self);
}

// --- public API -------------------------------------------------------------

GtkWidget *gtkpyte_terminal_new(void) {
    return GTK_WIDGET(g_object_new(GTKPYTE_TYPE_TERMINAL, nullptr));
}

void gtkpyte_terminal_feed(GtkpyteTerminal *self, const char *data, gsize len) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    if (data == nullptr || len == 0) {
        return;
    }
    self->st->stream.feed(data, len);
    sync_adjustment(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void gtkpyte_terminal_paste(GtkpyteTerminal *self, const char *utf8) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    if (utf8 == nullptr || *utf8 == '\0') {
        return;
    }
    scroll_to_live(self);
    emit_commit(self, gtkpyte::encode_text(utf8, self->st->bracketed_paste));
}

void gtkpyte_terminal_paste_clipboard(GtkpyteTerminal *self) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    gdk_clipboard_read_text_async(clipboard, nullptr, on_clipboard_text, g_object_ref(self));
}

void gtkpyte_terminal_set_font(GtkpyteTerminal *self, const char *font_desc) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    g_return_if_fail(font_desc != nullptr);
    State *st = self->st;
    if (st->font != nullptr) {
        pango_font_description_free(st->font);
    }
    st->font = pango_font_description_from_string(font_desc);
    measure_cells(self);
    gtk_widget_queue_resize(GTK_WIDGET(self));
    gtk_widget_queue_draw(GTK_WIDGET(self));
    g_object_notify_by_pspec(G_OBJECT(self), g_props[PROP_FONT]);
}

void gtkpyte_terminal_set_font_desc(GtkpyteTerminal *self, const PangoFontDescription *desc) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    g_return_if_fail(desc != nullptr);
    char *str = pango_font_description_to_string(desc);
    gtkpyte_terminal_set_font(self, str);
    g_free(str);
}

char *gtkpyte_terminal_get_font(GtkpyteTerminal *self) {
    g_return_val_if_fail(GTKPYTE_IS_TERMINAL(self), nullptr);
    return pango_font_description_to_string(self->st->font);
}

void gtkpyte_terminal_set_scrollback(GtkpyteTerminal *self, int lines) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    self->st->screen.set_max_scrollback(std::max(0, lines));
    sync_adjustment(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    g_object_notify_by_pspec(G_OBJECT(self), g_props[PROP_SCROLLBACK_LINES]);
}

int gtkpyte_terminal_get_columns(GtkpyteTerminal *self) {
    g_return_val_if_fail(GTKPYTE_IS_TERMINAL(self), 0);
    return self->st->cols;
}

int gtkpyte_terminal_get_rows(GtkpyteTerminal *self) {
    g_return_val_if_fail(GTKPYTE_IS_TERMINAL(self), 0);
    return self->st->rows;
}

int gtkpyte_terminal_get_cell_width(GtkpyteTerminal *self) {
    g_return_val_if_fail(GTKPYTE_IS_TERMINAL(self), 0);
    return self->st->cell_w;
}

int gtkpyte_terminal_get_cell_height(GtkpyteTerminal *self) {
    g_return_val_if_fail(GTKPYTE_IS_TERMINAL(self), 0);
    return self->st->cell_h;
}

char *gtkpyte_terminal_get_screen_text(GtkpyteTerminal *self) {
    g_return_val_if_fail(GTKPYTE_IS_TERMINAL(self), nullptr);
    std::string out;
    const pyte::Screen &screen = self->st->screen;
    for (int y = 0; y < screen.rows(); ++y) {
        out += screen.line_text(y);
        out += '\n';
    }
    return g_strdup(out.c_str());
}

gboolean gtkpyte_terminal_has_selection(GtkpyteTerminal *self) {
    g_return_val_if_fail(GTKPYTE_IS_TERMINAL(self), FALSE);
    return self->st->selection.active && !self->st->selection.empty();
}

char *gtkpyte_terminal_get_selected_text(GtkpyteTerminal *self) {
    g_return_val_if_fail(GTKPYTE_IS_TERMINAL(self), nullptr);
    return g_strdup(selected_text(self).c_str());
}

void gtkpyte_terminal_copy_clipboard(GtkpyteTerminal *self) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    const std::string text = selected_text(self);
    if (!text.empty()) {
        gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), text.c_str());
    }
}

void gtkpyte_terminal_select_all(GtkpyteTerminal *self) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    const pyte::Screen &screen = self->st->screen;
    set_selection(self, screen.first_line(), 0, screen.end_line() - 1, self->st->cols);
    publish_primary(self);
}

void gtkpyte_terminal_clear_selection(GtkpyteTerminal *self) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    State *st = self->st;
    if (!st->selection.active) {
        return;
    }
    st->selection.clear();
    notify_selection_changed(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

// --- colours ----------------------------------------------------------------

namespace {

GdkRGBA rgba_from_hex(unsigned v) {
    return GdkRGBA{static_cast<float>((v >> 16) & 0xFF) / 255.0f,
                   static_cast<float>((v >> 8) & 0xFF) / 255.0f,
                   static_cast<float>(v & 0xFF) / 255.0f, 1.0f};
}

// Hand-set colours mean the terminal no longer shows a named scheme.
void forget_scheme(GtkpyteTerminal *self) {
    if (!self->st->scheme.empty()) {
        self->st->scheme.clear();
        g_object_notify_by_pspec(G_OBJECT(self), g_props[PROP_COLOR_SCHEME]);
    }
}

}  // namespace

void gtkpyte_terminal_set_colors(GtkpyteTerminal *self, const GdkRGBA *foreground,
                                 const GdkRGBA *background, const GdkRGBA *palette,
                                 gsize palette_size) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    g_return_if_fail(palette_size == 0 || palette_size == 8 || palette_size == 16);
    g_return_if_fail(palette_size == 0 || palette != nullptr);
    State *st = self->st;
    const gtkpyte::Palette defaults;

    st->palette.set_foreground(foreground != nullptr ? *foreground : defaults.foreground());
    st->palette.set_background(background != nullptr ? *background : defaults.background());
    // Entries not supplied go back to their default, so an 8-colour palette
    // leaves the bright half at xterm's values.
    for (gsize i = 0; i < 16; ++i) {
        st->palette.set_ansi(static_cast<int>(i),
                             i < palette_size ? palette[i] : defaults.ansi(static_cast<int>(i)));
    }

    GObject *object = G_OBJECT(self);
    g_object_freeze_notify(object);
    forget_scheme(self);
    g_object_notify_by_pspec(object, g_props[PROP_FOREGROUND_COLOR]);
    g_object_notify_by_pspec(object, g_props[PROP_BACKGROUND_COLOR]);
    g_object_thaw_notify(object);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void gtkpyte_terminal_set_color_foreground(GtkpyteTerminal *self, const GdkRGBA *color) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    self->st->palette.set_foreground(color != nullptr ? *color : gtkpyte::Palette().foreground());
    forget_scheme(self);
    g_object_notify_by_pspec(G_OBJECT(self), g_props[PROP_FOREGROUND_COLOR]);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void gtkpyte_terminal_set_color_background(GtkpyteTerminal *self, const GdkRGBA *color) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    self->st->palette.set_background(color != nullptr ? *color : gtkpyte::Palette().background());
    forget_scheme(self);
    g_object_notify_by_pspec(G_OBJECT(self), g_props[PROP_BACKGROUND_COLOR]);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void gtkpyte_terminal_set_color_cursor(GtkpyteTerminal *self, const GdkRGBA *color) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    self->st->palette.set_cursor(color != nullptr ? *color : gtkpyte::Palette().cursor());
    forget_scheme(self);
    g_object_notify_by_pspec(G_OBJECT(self), g_props[PROP_CURSOR_COLOR]);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void gtkpyte_terminal_set_color_selection(GtkpyteTerminal *self, const GdkRGBA *color) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self));
    // Not part of a scheme, so setting it leaves the scheme name alone.
    self->st->selection_color = color != nullptr ? *color : kSelectionColor;
    g_object_notify_by_pspec(G_OBJECT(self), g_props[PROP_SELECTION_COLOR]);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void gtkpyte_terminal_get_color_foreground(GtkpyteTerminal *self, GdkRGBA *color) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self) && color != nullptr);
    *color = self->st->palette.foreground();
}

void gtkpyte_terminal_get_color_background(GtkpyteTerminal *self, GdkRGBA *color) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self) && color != nullptr);
    *color = self->st->palette.background();
}

void gtkpyte_terminal_get_color_cursor(GtkpyteTerminal *self, GdkRGBA *color) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self) && color != nullptr);
    *color = self->st->palette.cursor();
}

void gtkpyte_terminal_get_color_selection(GtkpyteTerminal *self, GdkRGBA *color) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self) && color != nullptr);
    *color = self->st->selection_color;
}

void gtkpyte_terminal_get_palette(GtkpyteTerminal *self, GdkRGBA *palette) {
    g_return_if_fail(GTKPYTE_IS_TERMINAL(self) && palette != nullptr);
    for (int i = 0; i < 16; ++i) {
        palette[i] = self->st->palette.ansi(i);
    }
}

gboolean gtkpyte_terminal_set_color_scheme(GtkpyteTerminal *self, const char *name) {
    g_return_val_if_fail(GTKPYTE_IS_TERMINAL(self), FALSE);
    const gtkpyte::ColorScheme *scheme = gtkpyte::find_scheme(name);
    if (scheme == nullptr) {
        return FALSE;
    }
    State *st = self->st;
    st->palette.set_foreground(rgba_from_hex(scheme->foreground));
    st->palette.set_background(rgba_from_hex(scheme->background));
    st->palette.set_cursor(rgba_from_hex(scheme->cursor));
    for (int i = 0; i < 16; ++i) {
        st->palette.set_ansi(i, rgba_from_hex(scheme->ansi[i]));
    }
    st->scheme = scheme->name;

    GObject *object = G_OBJECT(self);
    g_object_freeze_notify(object);
    g_object_notify_by_pspec(object, g_props[PROP_COLOR_SCHEME]);
    g_object_notify_by_pspec(object, g_props[PROP_FOREGROUND_COLOR]);
    g_object_notify_by_pspec(object, g_props[PROP_BACKGROUND_COLOR]);
    g_object_notify_by_pspec(object, g_props[PROP_CURSOR_COLOR]);
    g_object_thaw_notify(object);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    return TRUE;
}

const char *gtkpyte_terminal_get_color_scheme(GtkpyteTerminal *self) {
    g_return_val_if_fail(GTKPYTE_IS_TERMINAL(self), nullptr);
    return self->st->scheme.empty() ? nullptr : self->st->scheme.c_str();
}

char **gtkpyte_terminal_list_color_schemes(void) {
    std::size_t count = 0;
    const gtkpyte::ColorScheme *all = gtkpyte::schemes(count);
    char **names = g_new0(char *, count + 1);
    for (std::size_t i = 0; i < count; ++i) {
        names[i] = g_strdup(all[i].name);
    }
    return names;
}
