#!/bin/bash
# Reports whether CodeWeavers has published CrossOver sources newer than the
# last import. Exits 0 when up to date and 10 when a new release is waiting.
set -euo pipefail
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

latest="$(curl -fsSL https://www.codeweavers.com/crossover/source |
    grep -o 'crossover-sources-[0-9][0-9.]*[0-9]\.tar\.gz' | sort -uV | tail -1)"
[[ -n "$latest" ]] || { echo "No CrossOver sources found on the CodeWeavers page" >&2; exit 1; }
version="${latest#crossover-sources-}"
version="${version%.tar.gz}"
imported="$(git tag --list 'crossover-*' | sed 's/^crossover-//' | sort -V | tail -1)"

if git rev-parse -q --verify "refs/tags/crossover-$version" >/dev/null; then
    echo "Up to date with CrossOver $version"
else
    echo "CrossOver $version is available (last import: ${imported:-none})"
    echo "Import it with: forge/import-crossover.sh $version"
    exit 10
fi
