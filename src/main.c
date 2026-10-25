/* nemdown — a native Wayland markdown viewer.
 *
 * Usage: nemdown [-f] [file.md]
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "ui/filechooser.h"
#include "util/log.h"

static void usage(FILE *out) {
  fputs("usage: nemdown [-f] [file.md]\n"
        "\n"
        "  -f, --foreground stay attached to the terminal\n"
        "\n"
        "  j / k or { / }   block         g / G   first / last block\n"
        "  arrows           scroll        f       reading guide on/off\n"
        "  PgUp / PgDn      page          b       toggle sidebar\n"
        "  o                open a file   q / Esc quit\n"
        "\n"
        "With no file, the desktop's Open dialog asks for one.\n",
        out);
}

/* Given a file, behave like `nemdown file.md &` without the `&`: the parent
 * returns the shell its prompt and the child keeps the window. The parent
 * waits on a pipe until the child reports the document opened, so a missing
 * file or no compositor is still an error message and a non-zero status in
 * the terminal that asked, not a silent exit in the background. Forking here,
 * before any Wayland connection, font cache or thread exists, is what makes
 * it safe -- the child is the only process that ever touches any of them.
 * Returns the write end of the pipe, or -1 if detaching failed and we simply
 * run in the foreground. */
static int detach(void) {
  int p[2];
  if (pipe2(p, O_CLOEXEC) < 0) return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    return -1;
  }
  if (pid > 0) {
    close(p[1]);
    char c;
    ssize_t n;
    do n = read(p[0], &c, 1); while (n < 0 && errno == EINTR);
    if (n == 1) _exit(0);
    /* EOF without the byte: the child died during init, and has already said
     * why on the stderr we share. Pass its status through. */
    int st;
    pid_t r;
    do r = waitpid(pid, &st, 0); while (r < 0 && errno == EINTR);
    if (r < 0) _exit(1);
    _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
  }
  close(p[0]);
  /* A new session drops the controlling terminal, so closing it sends no
   * SIGHUP -- the one way this is better than `&`. */
  setsid();
  return p[1];
}

/* Init succeeded: release the parent, then let go of the terminal's stdio so
 * later warnings do not print over the user's next command. */
static void detach_done(int ready) {
  if (ready < 0) return;
  ssize_t n;
  do n = write(ready, "", 1); while (n < 0 && errno == EINTR);
  close(ready);
  int null = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (null < 0) return;
  dup2(null, STDIN_FILENO);
  dup2(null, STDOUT_FILENO);
  dup2(null, STDERR_FILENO);
  if (null > STDERR_FILENO) close(null);
}

int main(int argc, char **argv) {
  bool foreground = false;
  const char *path = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(stdout);
      return 0;
    } else if (strcmp(argv[i], "-f") == 0 ||
               strcmp(argv[i], "--foreground") == 0) {
      foreground = true;
    } else if (path) {
      usage(stderr);
      return 2;
    } else {
      path = argv[i];
    }
  }

  int ready = -1;
  if (path && !foreground) ready = detach();

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
  if (!path) {
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
  if (!nd_app_init(&app, chosen ? chosen : path, &err)) {
    nd_die("%s", err ? err : "initialisation failed");
  }
  detach_done(ready);

  int rc = nd_app_run(&app);
  nd_app_finish(&app);
  free(chosen);
  return rc;
}
