#!/bin/bash
#
# Vendor a release of pele, the mermaid renderer nemdown runs in QuickJS.
#
# Usage: tools/update-pele.sh 1.10.0
#
# Fetches the npm tarball, checks it against the sha512 the registry publishes
# for that version, and copies out the two files nemdown ships: the global
# bundle, which src/doc/pele_bundle.c links in, and the licence, which `make
# install` installs beside ours. Then rebuild and run `make test`: the bundle
# is linked into every harness, so check_mermaid sweeps the new version.
#
# The registry's checksum guards against a damaged or substituted download,
# not against a malicious release -- read the changelog before vendoring one.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [[ $# -ne 1 || ! $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "usage: $0 <version>   e.g. $0 1.10.0" >&2
  exit 2
fi
version=$1

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

curl -fsSL "https://registry.npmjs.org/pele/$version" -o "$tmp/meta.json"
tarball=$(jq -r '.dist.tarball' "$tmp/meta.json")
integrity=$(jq -r '.dist.integrity' "$tmp/meta.json")
if [[ $integrity != sha512-* ]]; then
  echo "no sha512 integrity for pele $version" >&2
  exit 1
fi

curl -fsSL "$tarball" -o "$tmp/pele.tgz"
got="sha512-$(openssl dgst -sha512 -binary "$tmp/pele.tgz" | base64 -w0)"
if [[ $got != "$integrity" ]]; then
  echo "checksum mismatch for $tarball" >&2
  echo "  registry: $integrity" >&2
  echo "  download: $got" >&2
  exit 1
fi

mkdir "$tmp/x"
tar -xzf "$tmp/pele.tgz" -C "$tmp/x" package/dist/pele.global.js package/LICENSE

mkdir -p vendor/pele
install -m644 "$tmp/x/package/dist/pele.global.js" vendor/pele/pele.global.js
install -m644 "$tmp/x/package/LICENSE" vendor/pele/LICENSE
printf '%s\n%s\n' "$version" "$integrity" > vendor/pele/VERSION

echo "vendored pele $version"
