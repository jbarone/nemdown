/* entity.h — HTML entity decoding.
 *
 * md4c explicitly does not decode entities; it hands them over as
 * MD_TEXT_ENTITY and leaves the policy to the caller.
 */

#ifndef NEMDOWN_ENTITY_H
#define NEMDOWN_ENTITY_H

#include <stddef.h>

/* Decodes `&...;` at src (length n) into out, which must hold >= 8 bytes.
 * Returns the number of UTF-8 bytes written, or 0 if unrecognised. */
int nd_entity_decode(const char *src, size_t n, char out[8]);

#endif /* NEMDOWN_ENTITY_H */
