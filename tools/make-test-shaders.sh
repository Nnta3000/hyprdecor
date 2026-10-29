#!/usr/bin/env bash
# Generates the staged screen-shader test set, so each step can be checked on its own.
#
#   tools/make-test-shaders.sh <out-dir>
#
# Stage order matters: each one adds exactly one thing to the previous, so when an
# image looks wrong the culprit is the change you just enabled.
#
#   0 passthrough  -- identity. If the screen changes at all, the shader is not
#                     being applied cleanly and nothing after it can be trusted.
#   1 pixelate     -- 2x2 block sampling, no palette. Shows the grid and proves
#                     sampling coordinates are right.
#   2 palette      -- palette mapping, FULL resolution, no dither. If this looks
#                     posterised-but-solid, the LUT lookup is correct.
#   3 dither       -- palette + 4x4 Bayer. The finished effect.
set -euo pipefail

OUT="${1:?usage: make-test-shaders.sh <out-dir>}"
mkdir -p "$OUT"

HDR='#version 300 es
precision highp float;
in vec2 v_texcoord;
uniform sampler2D tex;
layout(location = 0) out vec4 fragColor;
// TEST SHADER -- see tools/make-test-shaders.sh
'

cat > "$OUT/0-passthrough.frag" <<EOF
$HDR
void main() {
    fragColor = texture(tex, v_texcoord);
}
EOF

cat > "$OUT/1-pixelate.frag" <<EOF
$HDR
const int PIXEL_SIZE = 2;
void main() {
    // No palette: raw output, so only the sampling is under test. Kept premultiplied
    // (the input is), so this stage must be pixel-identical to it modulo the blocks.
    vec2 texel = 1.0 / vec2(textureSize(tex, 0));
    vec2 blockSize = texel * float(PIXEL_SIZE);
    vec2 blockUV = floor(v_texcoord / blockSize);
    vec2 snapped = (blockUV + 0.5) * blockSize;
    fragColor = texture(tex, clamp(snapped, vec2(0.0), vec2(1.0)));
}
EOF

# Stages 2 and 3 reuse the generated shader with the dither disabled/enabled, so the
# palette table is identical to production and only the dither decision differs.
GEN="${2:-}"
if [ -n "$GEN" ] && [ -f "$GEN" ]; then
    # Palette mapping only: always draw `first`, i.e. pretend every ratio is 0. This
    # isolates "is the LUT lookup right" from "is the dither pattern right".
    sed 's|vec3 outCol = paletteColour(threshold < level ? second : first);|vec3 outCol = paletteColour(first);|' \
        "$GEN" > "$OUT/2-palette-nodither.frag"
    cp "$GEN" "$OUT/3-dither.frag"
else
    echo "make-test-shaders: no generated shader given; wrote stages 0 and 1 only" >&2
fi

echo "wrote test shaders to $OUT:"
ls -1 "$OUT"
