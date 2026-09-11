/* check_pathguard.c — assert that nd_path_within admits nothing outside root.
 *
 * pathguard is the only thing standing between a note we did not write and
 * every file the user can read, so it is checked against a real directory
 * tree rather than by reasoning about strings. The tree is built in a fresh
 * mkdtemp() and torn down at exit, so the test is hermetic and can run
 * anywhere, in parallel, with no fixtures to keep in sync.
 *
 * Usage: build/check_pathguard
 */

#include <ftw.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "doc/pathguard.h"

static char tmp[4096];
static int fails;

/* Paths are built against the temp root, so every case reads as a relative
 * position in the tree rather than a machine-specific absolute path. */
static const char *at(const char *rel) {
  static char buf[8][4096];
  static unsigned turn;
  char *b = buf[turn++ % 8];
  if (!rel) return NULL;
  if (!*rel) { b[0] = '\0'; return b; }
  snprintf(b, sizeof buf[0], "%s/%s", tmp, rel);
  return b;
}

static void want(const char *root, const char *path, bool expect,
                 const char *why) {
  bool got = nd_path_within(root, path);
  printf("  %-3s %-4s  %s\n", got ? "in" : "out",
         got == expect ? "ok" : "FAIL", why);
  if (got != expect) fails++;
}

static void must(int rc, const char *what) {
  if (rc != 0) { perror(what); exit(2); }
}

static int unlink_cb(const char *p, const struct stat *st, int flag,
                     struct FTW *f) {
  (void)st; (void)flag; (void)f;
  return remove(p);
}

static void build_tree(void) {
  must(mkdir(at("vault"), 0755) , "mkdir vault");
  must(mkdir(at("vault/sub"), 0755), "mkdir vault/sub");
  must(mkdir(at("vault/sub/deep"), 0755), "mkdir vault/sub/deep");
  must(mkdir(at("vault-private"), 0755), "mkdir vault-private");
  must(mkdir(at("vaultx"), 0755), "mkdir vaultx");

  const char *files[] = {"secret.md", "vault/note.md", "vault/sub/deep/n.md",
                         "vault-private/secret.md"};
  for (unsigned i = 0; i < 4; i++) {
    FILE *f = fopen(at(files[i]), "w");
    if (!f) { perror("create"); exit(2); }
    fclose(f);
  }

  /* The three that matter: a link out to a file, a link out to a directory
   * (traversal through it must also fail), and a link that stays inside. */
  char target[4096];
  snprintf(target, sizeof target, "%s/secret.md", tmp);
  must(symlink(target, at("vault/esc")), "symlink esc");
  snprintf(target, sizeof target, "%s/vault-private", tmp);
  must(symlink(target, at("vault/escdir")), "symlink escdir");
  snprintf(target, sizeof target, "%s/vault/note.md", tmp);
  must(symlink(target, at("vault/inlink")), "symlink inlink");
  snprintf(target, sizeof target, "%s/vault", tmp);
  must(symlink(target, at("vlink")), "symlink vlink");
}

int main(void) {
  snprintf(tmp, sizeof tmp, "/tmp/nemdown-pathguard-XXXXXX");
  if (!mkdtemp(tmp)) { perror("mkdtemp"); return 2; }
  build_tree();

  const char *V = at("vault");
  char root[4096];
  snprintf(root, sizeof root, "%s", V);

  puts("nd_path_within");
  want(root, at("vault/note.md"),             true,  "plain member");
  want(root, at("vault"),                     true,  "the root itself");
  want(root, at("vault/sub/deep/n.md"),       true,  "nested member");
  want(root, at("vault/inlink"),              true,  "symlink to a file inside");

  want(root, at("vault-private/secret.md"),   false, "prefix sibling");
  want(root, at("vaultx"),                    false, "prefix sibling, no dash");
  want(root, at("secret.md"),                 false, "plain outsider");
  want(root, at("vault/../secret.md"),        false, "dot-dot escape");
  want(root, at("vault/sub/../../secret.md"), false, "nested dot-dot escape");
  want(root, at("vault/esc"),                 false, "symlink to a file outside");
  want(root, at("vault/escdir"),              false, "symlink to a dir outside");
  want(root, at("vault/escdir/secret.md"),    false, "traversal via that symlink");
  want(root, at("vault/nope.md"),             false, "path does not exist");

  /* Shapes the callers can actually produce: doc.c stores dirname(argv[1])
   * verbatim, so the root may carry a trailing slash or a dot component. */
  want(at("vault/"),  at("vault/note.md"), true,  "root with trailing slash");
  want(at("vault/."), at("vault/note.md"), true,  "root with dot component");
  want(at("vlink"),   at("vault/note.md"), true,  "root reached via a symlink");
  want(at("nosuch"),  at("vault/note.md"), false, "root does not exist");
  want("/",           at("vault/note.md"), true,  "root is /");
  want("/",           "/",                 true,  "/ is within /");

  want(root, NULL,               false, "NULL path");
  want(NULL, at("vault/note.md"), false, "NULL root");
  want("",   at("vault/note.md"), false, "empty root");
  want(root, "",                  false, "empty path");

  nftw(tmp, unlink_cb, 16, FTW_DEPTH | FTW_PHYS);

  printf("%s: %d failure(s)\n", fails ? "FAIL" : "ok", fails);
  return fails != 0;
}
