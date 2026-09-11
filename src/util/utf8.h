/* utf8.h — make a document's bytes safe to hand to Pango and GLib.
 *
 * node.h promises inl.text is "UTF-8, NUL-terminated, no interior NUL", but
 * nothing upstream enforced it: the bytes come straight off disk. That is not
 * a cosmetic contract. Pango's valid-UTF-8 precondition is load-bearing —
 * pango_get_log_attrs() loops FOREVER on a buffer that ends mid-sequence, so
 * one stray byte in a downloaded note froze the whole viewer on a double-click.
 */

#ifndef NEMDOWN_UTF8_H
#define NEMDOWN_UTF8_H

#include <stddef.h>

/* Replaces every byte not part of a well-formed UTF-8 sequence, and every NUL,
 * with '?'. Returns how many bytes were replaced.
 *
 * LENGTH-PRESERVING, deliberately: task_mark_offset and the embed edit map
 * index the file by byte, and the checkbox write goes to the real file on
 * disk, so a replacement that changed length would silently retarget it. That
 * rules out the conventional U+FFFD, which is three bytes.
 *
 * Rejects overlong encodings, surrogates and values above U+10FFFF, so the
 * result is not merely decodable but well-formed. */
size_t nd_utf8_scrub(char *buf, size_t len);

#endif /* NEMDOWN_UTF8_H */
