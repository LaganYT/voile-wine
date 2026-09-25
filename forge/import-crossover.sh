#!/bin/bash
# Imports the Wine sources of one CrossOver release as a single commit on the
# crossover branch, tagged crossover-<version>. The import happens in a
# temporary worktree, so the current branch and its files stay untouched.
set -euo pipefail
version="${1:?Usage: $0 <version>, for example 26.4.0}"
root="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "$root"

fail() {
    echo "$*" >&2
    exit 1
}

git rev-parse -q --verify "refs/tags/crossover-$version" >/dev/null &&
    fail "CrossOver $version is already imported"
work="$(mktemp -d)"
trap 'git worktree remove --force "$work/tree" 2>/dev/null || true; rm -rf "$work"' EXIT

archive="$work/crossover-sources-$version.tar.gz"
curl -fL --progress-bar -o "$archive" \
    "https://media.codeweavers.com/pub/crossover/source/crossover-sources-$version.tar.gz"
sha="$(shasum -a 256 "$archive" | cut -d' ' -f1)"

git worktree add -q "$work/tree" crossover
(
    cd "$work/tree"
    git rm -rq --ignore-unmatch .
    tar -xzf "$archive" --strip-components 2 sources/wine
    [[ -f VERSION ]] || fail "The archive holds no sources/wine"
    git add -A
    git commit -q -m "CrossOver $version

Wine sources from crossover-sources-$version.tar.gz, published by
CodeWeavers at https://www.codeweavers.com/crossover/source (sha256
$sha)."
    git tag "crossover-$version"
)

echo "Imported CrossOver $version: $(cat "$work/tree/VERSION")"
echo "Archive sha256, for Forge's sources.json: $sha"
echo "Next: git rebase crossover-$version main, resolve conflicts, build, then tag v$version-1"
