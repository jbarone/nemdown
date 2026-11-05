/* keys.h — the keybinding list, drawn by `?` and printed by --help.
 *
 * One table feeds both, so the two cannot drift apart. The bindings
 * themselves live in nd_app_key; adding one there means adding a row here. */

#ifndef NEMDOWN_KEYS_H
#define NEMDOWN_KEYS_H

#include <cairo.h>
#include <stdio.h>

/* Prints the table as indented two-column text, for the usage message. */
void nd_keys_print(FILE *out);

/* Dims the window and draws the table in a centred panel, scaled down to fit
 * when the window is too short for it. w and h are logical pixels. */
void nd_keys_paint(cairo_t *cr, double w, double h, double scale);

#endif /* NEMDOWN_KEYS_H */
