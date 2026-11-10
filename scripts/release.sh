#!/usr/bin/env bash
# Bump, verify, commit and tag a release. The push is left to the caller.
#
#   scripts/release.sh patch    # 0.4.0 -> 0.4.1
#   scripts/release.sh minor    # 0.4.0 -> 0.5.0
#   scripts/release.sh major    # 0.4.0 -> 1.0.0
#
# CMakeLists.txt is the version source; hyprpm.toml is rewritten from it. The script
# refuses to run unless the two already agree, the tree is clean and the branch is main.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

die() { printf 'release: %s\n' "$*" >&2; exit 1; }

case "${1:-}" in
    patch|minor|major) KIND=$1 ;;
    *) die "usage: $0 patch|minor|major" ;;
esac

[ "$(git rev-parse --abbrev-ref HEAD)" = "main" ] || die "must run on main"
[ -z "$(git status --porcelain)" ] || die "working tree is not clean"

CURRENT=$(grep -m1 -E '^    VERSION [0-9]+\.[0-9]+\.[0-9]+$' CMakeLists.txt | awk '{print $2}')
HYPRPM=$(sed -n 's/^version = "\([0-9]*\.[0-9]*\.[0-9]*\)"$/\1/p' hyprpm.toml)
[ -n "$CURRENT" ] || die "could not read VERSION from CMakeLists.txt"
[ "$CURRENT" = "$HYPRPM" ] || die "CMake version $CURRENT != hyprpm version ${HYPRPM:-<missing>}"

IFS=. read -r MAJOR MINOR PATCH <<< "$CURRENT"
case "$KIND" in
    patch) PATCH=$((PATCH + 1)) ;;
    minor) MINOR=$((MINOR + 1)); PATCH=0 ;;
    major) MAJOR=$((MAJOR + 1)); MINOR=0; PATCH=0 ;;
esac
NEXT="$MAJOR.$MINOR.$PATCH"

git rev-parse -q --verify "refs/tags/v$NEXT" >/dev/null && die "tag v$NEXT already exists"

# Do not leave a half-bumped tree behind if the build or tests fail.
restore() { git checkout -- CMakeLists.txt hyprpm.toml 2>/dev/null || true; }
trap restore ERR

sed -i -E "s/^    VERSION [0-9]+\.[0-9]+\.[0-9]+$/    VERSION $NEXT/" CMakeLists.txt
sed -i -E "s/^version = \"[0-9]+\.[0-9]+\.[0-9]+\"$/version = \"$NEXT\"/" hyprpm.toml

printf 'release: %s -> %s (%s)\n' "$CURRENT" "$NEXT" "$KIND"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

trap - ERR
git add CMakeLists.txt hyprpm.toml
git commit -m "chore: release v$NEXT"
git tag -a "v$NEXT" -m "hyprdecor $NEXT"

printf '\nReleased v%s\n' "$NEXT"
printf 'Push:   git push --follow-tags fork main\n'
printf 'Notes:  gh release create v%s --generate-notes\n' "$NEXT"
