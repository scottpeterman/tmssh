/* src/gtkpyte-terminal.h
 *
 * GtkpyteTerminal: a GTK4 terminal widget over the pyte emulation core.
 *
 * The widget owns a screen and nothing else -- no pty, no shell, no SSH.
 * Bytes come in through gtkpyte_terminal_feed() and go out through the
 * "commit" signal; where they come from is the caller's business. That is the
 * same split qtpyte makes, and it is what lets one widget sit on a local pty,
 * an SSH channel or a recorded capture without knowing which.
 *
 * The API is plain C so the header stays usable from C and from GObject
 * Introspection later; the implementation is C++ because pyte is.
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GTKPYTE_TYPE_TERMINAL (gtkpyte_terminal_get_type())
G_DECLARE_FINAL_TYPE(GtkpyteTerminal, gtkpyte_terminal, GTKPYTE, TERMINAL, GtkWidget)

GtkWidget *gtkpyte_terminal_new(void);

/* Push received bytes into the emulator and schedule a redraw. */
void gtkpyte_terminal_feed(GtkpyteTerminal *self, const char *data, gsize len);

/* Send text as if pasted: newline conversion and bracketed paste applied,
 * emitted through "commit". */
void gtkpyte_terminal_paste(GtkpyteTerminal *self, const char *utf8);

/* Paste the clipboard contents (what Ctrl+Shift+V does). */
void gtkpyte_terminal_paste_clipboard(GtkpyteTerminal *self);

/* Font, as a Pango description string ("Monospace 11") or a description.
 * Changing it re-derives the cell size and so the grid, and tells the child
 * through "resized". get_font returns a newly allocated string. Also the
 * "font" property. */
void gtkpyte_terminal_set_font(GtkpyteTerminal *self, const char *font_desc);
void gtkpyte_terminal_set_font_desc(GtkpyteTerminal *self, const PangoFontDescription *desc);
char *gtkpyte_terminal_get_font(GtkpyteTerminal *self);

/* Rows of history kept; 0 disables scrollback. The "scrollback-lines" property. */
void gtkpyte_terminal_set_scrollback(GtkpyteTerminal *self, int lines);

/* Colours.
 *
 * set_colors sets the default foreground and background and the 16 ANSI
 * entries in one call (palette_size 0, 8 or 16). A NULL colour, and any
 * palette entry not supplied, goes back to its default. The 256-colour cube
 * and greyscale ramp above the 16 are xterm's and fixed; truecolour is drawn
 * as sent.
 *
 * The single-colour setters take NULL for "default" as well. Each is also a
 * GdkRGBA property: "foreground-color", "background-color", "cursor-color",
 * "selection-color" (painted over text, so give it alpha). */
void gtkpyte_terminal_set_colors(GtkpyteTerminal *self, const GdkRGBA *foreground,
                                 const GdkRGBA *background, const GdkRGBA *palette,
                                 gsize palette_size);
void gtkpyte_terminal_set_color_foreground(GtkpyteTerminal *self, const GdkRGBA *color);
void gtkpyte_terminal_set_color_background(GtkpyteTerminal *self, const GdkRGBA *color);
void gtkpyte_terminal_set_color_cursor(GtkpyteTerminal *self, const GdkRGBA *color);
void gtkpyte_terminal_set_color_selection(GtkpyteTerminal *self, const GdkRGBA *color);
void gtkpyte_terminal_get_color_foreground(GtkpyteTerminal *self, GdkRGBA *color);
void gtkpyte_terminal_get_color_background(GtkpyteTerminal *self, GdkRGBA *color);
void gtkpyte_terminal_get_color_cursor(GtkpyteTerminal *self, GdkRGBA *color);
void gtkpyte_terminal_get_color_selection(GtkpyteTerminal *self, GdkRGBA *color);
/* Fills palette[0..15]. */
void gtkpyte_terminal_get_palette(GtkpyteTerminal *self, GdkRGBA *palette);

/* Built-in schemes: foreground, background, cursor and the 16 ANSI colours.
 * set returns FALSE for an unknown name and changes nothing. get returns the
 * scheme last applied, or NULL once any colour has been set individually.
 * The "color-scheme" property. list returns a NULL-terminated array to free
 * with g_strfreev. */
gboolean gtkpyte_terminal_set_color_scheme(GtkpyteTerminal *self, const char *name);
const char *gtkpyte_terminal_get_color_scheme(GtkpyteTerminal *self);
char **gtkpyte_terminal_list_color_schemes(void);

int gtkpyte_terminal_get_columns(GtkpyteTerminal *self);
int gtkpyte_terminal_get_rows(GtkpyteTerminal *self);
int gtkpyte_terminal_get_cell_width(GtkpyteTerminal *self);
int gtkpyte_terminal_get_cell_height(GtkpyteTerminal *self);

/* The visible screen as text, one line per row, trailing spaces kept. */
char *gtkpyte_terminal_get_screen_text(GtkpyteTerminal *self);

/* Selection. Copying is never automatic: selecting puts the text on the
 * primary selection (middle-click paste, X11/Wayland convention) but leaves
 * the clipboard alone until copy_clipboard() -- Ctrl+Shift+C -- is called. */
gboolean gtkpyte_terminal_has_selection(GtkpyteTerminal *self);
char *gtkpyte_terminal_get_selected_text(GtkpyteTerminal *self);
void gtkpyte_terminal_copy_clipboard(GtkpyteTerminal *self);
void gtkpyte_terminal_select_all(GtkpyteTerminal *self);
void gtkpyte_terminal_clear_selection(GtkpyteTerminal *self);

/* Scrollback is exposed through GtkScrollable (in lines, not pixels): put the
 * terminal in a GtkScrolledWindow for a scrollbar.
 *
 * Signals
 *
 *   "commit"  (GtkpyteTerminal *, GBytes *data)
 *       Bytes to send upstream: keystrokes, pastes, and replies the emulator
 *       owes to Device Status Report and friends. Drop it and applications
 *       that query the terminal will hang waiting.
 *
 *   "resized" (GtkpyteTerminal *, int columns, int rows)
 *       The grid changed shape. Whatever is producing the bytes needs to know
 *       (TIOCSWINSZ for a pty, window-change for SSH).
 *
 *   "selection-changed" (GtkpyteTerminal *)
 *       A selection appeared or went away -- for enabling a Copy menu item.
 */

G_END_DECLS
