/* nemdown — a native Wayland markdown viewer.
 *
 * Usage: nemdown <file.md>
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "ui/filechooser.h"
#include "util/log.h"

static void usage(FILE *out) {
  fputs("usage: nemdown [file.md]\n"
        "\n"
        "  j / k or { / }   block         g / G   first / last block\n"
        "  arrows           scroll        f       reading guide on/off\n"
        "  PgUp / PgDn      page          b       toggle sidebar\n"
        "  o                open a file   q / Esc quit\n"
        "\n"
        "With no file, the desktop's Open dialog asks for one.\n",
        out);
}

int main(int argc, char **argv) {
  if (argc > 2) {
    usage(stderr);
    return 2;
  }
  if (argc == 2 && (strcmp(argv[1], "-h") == 0 ||
                    strcmp(argv[1], "--help") == 0)) {
    usage(stdout);
    return 0;
  }

  /* We write to a pipe feeding wl-copy. If that process is absent, wedged, or
   * exits early, the default SIGPIPE disposition would kill us mid-copy — and
   * the payload size is document-controlled, since Ctrl-C with no selection
   * copies the whole source. Take the EPIPE return instead. */
  signal(SIGPIPE, SIG_IGN);

  /* Both spawn sites fire and forget. Without this every copy and every link
   * click leaves a zombie for the life of the process. */
  signal(SIGCHLD, SIG_IGN);

  /* No file: ask the desktop for one. This is the launcher case -- an entry
   * in a menu cannot pass a path -- and doing it BEFORE any Wayland setup is
   * what keeps it simple: there is no window yet, so no compositor ping to
   * answer and nothing to draw an empty state for. Cancelling just exits. */
  char *chosen = NULL;
  if (argc == 1) {
    struct nd_filechooser fc = {.fd = -1, .wfd = -1};
    if (!nd_filechooser_start(&fc, NULL)) {
      nd_die("no file given, and no file chooser available "
             "(is xdg-desktop-portal running?)");
    }
    chosen = nd_filechooser_wait(&fc);
    nd_filechooser_finish(&fc);
    if (!chosen) return 0;   /* cancelled */
  }

  struct nd_app app;
  char *err = NULL;
  if (!nd_app_init(&app, chosen ? chosen : argv[1], &err)) {
    nd_die("%s", err ? err : "initialisation failed");
  }

  int rc = nd_app_run(&app);
  nd_app_finish(&app);
  free(chosen);
  return rc;
}
