#!/usr/bin/env bash
# Regenerate the palette-dither screen shaders and install them where hyprshade
# looks for them.
#
#   tools/install-shaders.sh                 # uses ../orange-palette-q3.png
#   tools/install-shaders.sh path/to/pal.png
#
# Writes TWO shaders from one palette:
#   palette_dither.frag       PIXEL_SIZE 1  (full resolution)
#   palette_dither_half.frag  PIXEL_SIZE 2  (one sample per 2x2 block)
#
# The bar's eye cycles off -> full -> half -> off, and hyprshade only knows on/off, so
# two files is how the third state is expressed. Generating both from the same palette
# in one run is what keeps them identical apart from the sampling factor.
#
# 3 bits is not a taste call: a 5-bit table (32768 entries) makes Intel/Mesa hang while
# compiling the screen shader. See DEFAULT_KEY_BITS in tools/dither_core.hpp.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
GEN="$REPO/build/palette-gen"

PALETTE="${1:-$REPO/../orange-palette-q3.png}"
DEST="${HYPRSHADE_SHADERS_DIR:-$HOME/.config/hypr/shaders}"
KEY_BITS="${KEY_BITS:-3}"

if [ ! -x "$GEN" ]; then
    echo "install-shaders: palette-gen not built at $GEN" >&2
    echo "  build it: cd $REPO && cmake --build build --target palette-gen" >&2
    exit 1
fi

if [ ! -f "$PALETTE" ]; then
    echo "install-shaders: palette not found: $PALETTE" >&2
    exit 1
fi

mkdir -p "$DEST"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for PS in 1 2; do
    "$GEN" --palette "$PALETTE" --out-dir "$TMP/ps$PS" --key-bits "$KEY_BITS" --pixel-size "$PS" >/dev/null

    if [ "$PS" = "1" ]; then NAME="palette_dither";      else NAME="palette_dither_half"; fi
    cp "$TMP/ps$PS/palette_dither.frag" "$DEST/$NAME.frag"

    # A shader that does not compile blanks the screen, so refuse to install one.
    if command -v glslangValidator >/dev/null 2>&1; then
        if ! glslangValidator -S frag "$DEST/$NAME.frag" >/dev/null 2>&1; then
            echo "install-shaders: $NAME.frag failed to compile; removing it" >&2
            rm -f "$DEST/$NAME.frag"
            exit 1
        fi
    fi
done

echo "install-shaders: installed to $DEST"
ls -1 "$DEST"/palette_dither*.frag
echo
echo "cycle in the bar:  off -> palette_dither -> palette_dither_half -> off"
