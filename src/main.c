/* nemdown — a native Wayland markdown viewer.
 *
 * Usage: nemdown <file.md>
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "util/log.h"

static void usage(FILE *out) {
  fputs("usage: nemdown <file.md>\n"
        "\n"
        "  j / k / arrows   scroll        g / G   top / bottom\n"
        "  PgUp / PgDn      page          b       toggle sidebar\n"
        "  q / Esc          quit\n",
        out);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    usage(argc < 2 ? stderr : stdout);
    return argc < 2 ? 2 : 0;
  }
  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
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

  struct nd_app app;
  char *err = NULL;
  if (!nd_app_init(&app, argv[1], &err)) {
    nd_die("%s", err ? err : "initialisation failed");
  }

  int rc = nd_app_run(&app);
  nd_app_finish(&app);
  return rc;
}
