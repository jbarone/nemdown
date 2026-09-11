/* wl_cursor.h — pointer shape via cursor-shape-v1.
 *
 * Using the protocol rather than loading a wl_cursor theme saves the whole
 * theme/size/hotspot/animation path for the three shapes we actually need.
 */

#ifndef NEMDOWN_WL_CURSOR_H
#define NEMDOWN_WL_CURSOR_H

#include <stdint.h>

struct nd_app;

typedef enum {
  ND_CURSOR_DEFAULT,
  ND_CURSOR_POINTER,
  ND_CURSOR_COL_RESIZE,
  ND_CURSOR_TEXT,
} nd_cursor;

/* No-op when the shape is already set: issuing a request per motion event
 * would be a round trip for nothing. */
void nd_cursor_set(struct nd_app *app, nd_cursor shape);

#endif /* NEMDOWN_WL_CURSOR_H */
