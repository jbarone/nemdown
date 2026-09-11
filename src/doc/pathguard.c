/* pathguard.c — path containment. */

#include "doc/pathguard.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

bool nd_path_within(const char *root, const char *path) {
  if (!root || !path) return false;

  char real_root[PATH_MAX];
  char real_path[PATH_MAX];

  /* realpath() is what makes this sound: it collapses `..`, resolves symlinks,
   * and yields the path the kernel would actually open. Comparing the strings
   * as written would be defeated by any of those. */
  if (!realpath(root, real_root)) return false;
  if (!realpath(path, real_path)) return false;

  size_t n = strlen(real_root);
  if (n == 0) return false;

  /* Trailing slash only for "/", where root and prefix coincide. */
  if (real_root[n - 1] == '/') n--;

  if (strncmp(real_path, real_root, n) != 0) return false;

  /* The next character must be the separator or the end, so that a root of
   * `/home/a/notes` does not also admit `/home/a/notes-private`. */
  return real_path[n] == '\0' || real_path[n] == '/';
}
