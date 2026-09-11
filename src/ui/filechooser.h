/* filechooser.h — the desktop's own Open dialog, via xdg-desktop-portal.
 *
 * A raw Wayland client has no file dialog and no toolkit to borrow one from,
 * and writing a file browser inside the viewer was ruled out on purpose: it is
 * a single-file reader, not a vault. The portal is the answer the desktop
 * already provides — org.freedesktop.portal.FileChooser over D-Bus, rendered
 * by whichever backend the user runs. It costs no new dependency, because gio
 * (and so GDBus) is already linked through pango's stack.
 *
 * The call runs on a worker thread and reports back through a pipe, rather
 * than embedding a GMainContext into our poll loop. That is deliberate: the
 * prepare_read/read_events dance in wl_shell.c is the most delicate thing in
 * the program, and a file dialog is not worth risking it. The loop gains one
 * more fd to watch and nothing else changes.
 */

#ifndef NEMDOWN_FILECHOOSER_H
#define NEMDOWN_FILECHOOSER_H

#include <stdbool.h>

struct nd_filechooser {
  int   fd;      /* readable once a result is ready; -1 when idle */
  int   wfd;     /* the worker's end */
  void *thread;  /* GThread *, opaque here to keep glib out of this header */
  char *result;  /* worker writes this BEFORE the byte; the pipe orders them */
  bool  busy;
};

/* Opens the dialog. `start_dir` may be NULL. One request at a time: a second
 * call while one is in flight is refused, since two dialogs would race to
 * replace the same document. Returns false if the portal is unreachable. */
bool nd_filechooser_start(struct nd_filechooser *fc, const char *start_dir);

/* The fd to poll, or -1 when idle. Safe to put straight into a pollfd: poll
 * ignores a negative fd. */
int nd_filechooser_fd(const struct nd_filechooser *fc);

/* Call once the fd is readable. Returns a malloc'd path the caller owns, or
 * NULL if the dialog was cancelled or failed. Either way the request is
 * finished and another may be started. */
char *nd_filechooser_take(struct nd_filechooser *fc);

/* Blocks until the dialog is answered. Only for the startup path, where there
 * is no window yet and so no compositor ping to keep answering. */
char *nd_filechooser_wait(struct nd_filechooser *fc);

void nd_filechooser_finish(struct nd_filechooser *fc);

#endif /* NEMDOWN_FILECHOOSER_H */
