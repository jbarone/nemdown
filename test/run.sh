#!/bin/bash
#
# Run the security checks, then the engine harnesses over every fixture.
# These are headless: no compositor is involved.
#
# Usage: test/run.sh   (or `make test`)

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

fail=0

# These two assert rather than report, so they run first: a regression in
# either is a hang or a path escape, not a rendering nit.
for check in check_utf8 check_pathguard; do
  if ! ./build/"$check"; then
    fail=1
  fi
done
echo

for f in $(find test/fixtures -name "*.md" | sort); do
  printf '%-40s' "$f"
  if out=$(./build/dump_tree "$f" 2>&1); then
    blocks=$(printf '%s\n' "$out" | grep -cE '^\s*(PARA|H |H level|LIST|CODE|TABLE|QUOTE)' || true)
    printf 'ok  (%s blocks)\n' "$blocks"
  else
    printf 'FAIL\n%s\n' "$out"
    fail=1
  fi
done

exit "$fail"
