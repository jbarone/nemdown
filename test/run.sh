#!/bin/bash
#
# Run the engine harnesses over every fixture and report parse failures.
# These are headless: no compositor is involved.
#
# Usage: test/run.sh   (or `make test`)

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

fail=0
for f in test/fixtures/*.md; do
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
