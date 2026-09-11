/* images.h — decode and cache images referenced by the document.
 *
 * Decoded surfaces deliberately OUTLIVE a reparse: editing and saving a
 * document with ten screenshots in it should not re-decode ten PNGs.
 */

#ifndef NEMDOWN_IMAGES_H
#define NEMDOWN_IMAGES_H

#include <cairo.h>
#include <stdbool.h>

struct nd_image_cache;

/* `root_dir` bounds what a document may reference: paths resolving outside it
 * are treated as missing, and draw the placeholder. */
struct nd_image_cache *nd_images_new(const char *base_dir, const char *root_dir);
void nd_images_free(struct nd_image_cache *c);

/* Resolution-dependent surfaces are dropped when the display scale changes. */
void nd_images_set_scale(struct nd_image_cache *c, double scale);

/* Natural size without decoding pixels; false if the file cannot be found. */
bool nd_images_probe(struct nd_image_cache *c, const char *src,
                     double *w, double *h);

/* Decodes (or returns cached) at the requested logical size. NULL on failure,
 * which the caller draws as a placeholder. */
cairo_surface_t *nd_images_get(struct nd_image_cache *c, const char *src,
                               double w, double h);

#endif /* NEMDOWN_IMAGES_H */
