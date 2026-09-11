/* filechooser.c — org.freedesktop.portal.FileChooser, on a worker thread. */

#include "ui/filechooser.h"

#include <gio/gio.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/log.h"

struct worker {
  struct nd_filechooser *fc;
  char *start_dir;
};

static void on_response(GDBusConnection *bus, const gchar *sender,
                        const gchar *path, const gchar *iface,
                        const gchar *signal, GVariant *params, gpointer user) {
  (void)bus; (void)sender; (void)path; (void)iface; (void)signal;
  struct worker *w = user;

  guint32 code = 1;
  GVariant *results = NULL;
  g_variant_get(params, "(u@a{sv})", &code, &results);

  if (code == 0 && results) {
    GVariant *uris =
        g_variant_lookup_value(results, "uris", G_VARIANT_TYPE_STRING_ARRAY);
    if (uris) {
      gsize n = 0;
      const gchar **a = g_variant_get_strv(uris, &n);
      if (n > 0) {
        /* The portal answers with URIs. A file:// one converts to a path;
         * anything else (a document-portal handle, say) we decline rather than
         * guess at, because the engine opens paths and nothing else. */
        char *p = g_filename_from_uri(a[0], NULL, NULL);
        if (p) w->fc->result = strdup(p);
        g_free(p);
      }
      g_free(a);
      g_variant_unref(uris);
    }
  }
  if (results) g_variant_unref(results);

  g_main_loop_quit(g_object_get_data(G_OBJECT(bus), "nd-loop"));
}

static gpointer worker_main(gpointer data) {
  struct worker *w = data;

  /* Its own context, pushed as thread-default, so nothing here touches any
   * main context the rest of the process might have. */
  GMainContext *ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);

  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) {
    nd_warn("no session bus: %s", err ? err->message : "?");
    if (err) g_error_free(err);
    goto done;
  }

  GMainLoop *loop = g_main_loop_new(ctx, FALSE);
  g_object_set_data(G_OBJECT(bus), "nd-loop", loop);

  /* The Request object path is derivable from our own bus name, so subscribe
   * BEFORE calling. Subscribing after would leave a window in which the reply
   * arrives and is dropped, and the dialog would then hang forever. */
  const char *unique = g_dbus_connection_get_unique_name(bus);
  char *sender = g_strdup(unique && unique[0] == ':' ? unique + 1 : unique);
  for (char *p = sender; p && *p; p++)
    if (*p == '.') *p = '_';

  static unsigned long serial;
  char *token = g_strdup_printf("nemdown%lu", ++serial);
  char *req_path = g_strdup_printf(
      "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);

  guint sub = g_dbus_connection_signal_subscribe(
      bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request",
      "Response", req_path, NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
      on_response, w, NULL);

  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&opts, "{sv}", "handle_token",
                        g_variant_new_string(token));
  g_variant_builder_add(&opts, "{sv}", "modal", g_variant_new_boolean(TRUE));
  if (w->start_dir) {
    /* Bytestring, NUL included, per the portal's spec. */
    g_variant_builder_add(
        &opts, "{sv}", "current_folder",
        g_variant_new_bytestring(w->start_dir));
  }

  /* Markdown first, but with a way out: plenty of notes are .txt, and a
   * filter with no escape hatch is a dialog that cannot open the file you can
   * plainly see. */
  GVariantBuilder md, any, filters;
  g_variant_builder_init(&md, G_VARIANT_TYPE("a(us)"));
  g_variant_builder_add(&md, "(us)", 0u, "*.md");
  g_variant_builder_add(&md, "(us)", 0u, "*.markdown");
  g_variant_builder_add(&md, "(us)", 0u, "*.mdown");
  g_variant_builder_add(&md, "(us)", 0u, "*.mkd");
  g_variant_builder_init(&any, G_VARIANT_TYPE("a(us)"));
  g_variant_builder_add(&any, "(us)", 0u, "*");
  g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
  g_variant_builder_add(&filters, "(s@a(us))", "Markdown",
                        g_variant_builder_end(&md));
  g_variant_builder_add(&filters, "(s@a(us))", "All files",
                        g_variant_builder_end(&any));
  g_variant_builder_add(&opts, "{sv}", "filters",
                        g_variant_builder_end(&filters));

  /* An empty parent means an unparented dialog. Parenting properly needs
   * xdg-foreign to export a window handle, which is a protocol we do not
   * implement for one dialog. */
  GVariant *reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.FileChooser", "OpenFile",
      g_variant_new("(ssa{sv})", "", "Open a markdown file", &opts),
      G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);

  if (!reply) {
    nd_warn("file chooser unavailable: %s", err ? err->message : "?");
    if (err) g_error_free(err);
  } else {
    g_variant_unref(reply);
    g_main_loop_run(loop);   /* until on_response quits it */
  }

  g_dbus_connection_signal_unsubscribe(bus, sub);
  g_free(req_path);
  g_free(token);
  g_free(sender);
  g_main_loop_unref(loop);
  g_object_unref(bus);

done:
  g_main_context_pop_thread_default(ctx);
  g_main_context_unref(ctx);

  /* The byte goes LAST. It is what the main thread waits on, so everything
   * this thread stored has already happened by the time that read returns. */
  char b = 1;
  ssize_t n = write(w->fc->wfd, &b, 1);
  (void)n;
  close(w->fc->wfd);
  w->fc->wfd = -1;

  free(w->start_dir);
  free(w);
  return NULL;
}

bool nd_filechooser_start(struct nd_filechooser *fc, const char *start_dir) {
  if (fc->busy) return false;

  int fds[2];
  if (pipe(fds) != 0) return false;

  fc->fd = fds[0];
  fc->wfd = fds[1];
  fc->result = NULL;
  fc->busy = true;

  struct worker *w = calloc(1, sizeof *w);
  if (!w) {
    close(fds[0]); close(fds[1]);
    fc->fd = fc->wfd = -1;
    fc->busy = false;
    return false;
  }
  w->fc = fc;
  w->start_dir = start_dir ? strdup(start_dir) : NULL;

  fc->thread = g_thread_new("nemdown-filechooser", worker_main, w);
  return fc->thread != NULL;
}

int nd_filechooser_fd(const struct nd_filechooser *fc) {
  return fc->busy ? fc->fd : -1;
}

char *nd_filechooser_take(struct nd_filechooser *fc) {
  if (!fc->busy) return NULL;

  char b;
  while (read(fc->fd, &b, 1) < 0 && errno == EINTR) {}

  if (fc->thread) {
    g_thread_join(fc->thread);
    fc->thread = NULL;
  }
  close(fc->fd);
  fc->fd = -1;
  fc->busy = false;

  char *r = fc->result;
  fc->result = NULL;
  return r;
}

char *nd_filechooser_wait(struct nd_filechooser *fc) {
  if (!fc->busy) return NULL;
  struct pollfd p = {.fd = fc->fd, .events = POLLIN};
  while (poll(&p, 1, -1) < 0 && errno == EINTR) {}
  return nd_filechooser_take(fc);
}

void nd_filechooser_finish(struct nd_filechooser *fc) {
  if (fc->busy) {
    /* The dialog may still be open; the worker cannot be cancelled from here,
     * so wait for it rather than leave a thread writing into a closed pipe. */
    free(nd_filechooser_take(fc));
  }
  free(fc->result);
  fc->result = NULL;
}
