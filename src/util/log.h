/* log.h — stderr diagnostics. */

#ifndef NEMDOWN_LOG_H
#define NEMDOWN_LOG_H

void nd_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nd_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nd_die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* Caller owns the result. Used for error strings handed back through APIs. */
char *nd_strdup_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#if NEMDOWN_DEBUG
void nd_dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
#define nd_dbg(...) ((void)0)
#endif

#endif /* NEMDOWN_LOG_H */
