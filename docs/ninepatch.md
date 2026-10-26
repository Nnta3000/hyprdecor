# Authoring nine-patch images

A `.png` wrapped in a 1px marker border. The border is metadata and is cropped off. Only
opaque pure black (`#000000`, alpha 255) counts as a marker.

| Edge | Black pixels mean | Parser field |
|---|---|---|
| Top | columns that **stretch** | `border` left / right = fixed corner widths |
| Left | rows that **stretch** | `border` top / bottom = fixed corner heights |
| Bottom | columns of the **content area** | `padding` left / right |
| Right | rows of the **content area** | `padding` top / bottom |

The two pairs are independent. Do **not** mirror stretch marks onto the content edges
unless the stretch zone really equals the content area.

Worked example, `window-background.png` (23x23 canvas, 21x21 art):
- Stretch marks (keep): top x = 13..14, left y = 10..11, a 2px slice inside the flat white
  interior. Parser: `border L12 T9 R7 B10`.
- Content marks (set): bottom x = 9..19, right y = 3..14, the white interior.
  Parser: `padding L8 T2 R2 B7`.
- Never stretch across outlines or shadows; that distorts the art.

## Expected parser output

Check what the parser resolves before touching any code (run the standalone nine-patch
parser on the file). For the worked example above it must report:

```
border  L12 T9 R7 B10     (from the top and left bars)
padding L8  T2 R2 B7      (from the bottom and right bars)
```

If it does not, a marker is off by a pixel. Fix the png before touching code.

## Common mistakes

- **Mirroring the marks.** Copying the top bar onto the bottom bar (and left onto right)
  makes the content area equal the stretch zone, which is almost never what you want.
- **Anti-aliased or semi-transparent marks.** Only opaque `#000000` counts. Use the pencil
  tool at 1px, no soft brush, no layer blending.
- **Stretching across art.** If the stretch band covers an outline or a shadow, that
  detail is stretched and the frame looks distorted. Keep the band inside flat areas.
- **No markers at all.** The whole image is then stretched as one piece.
- **A very small stretch band** (1 to 2 px) is fine when the middle is flat colour.

## File naming

`texture = "/path/frame"` is a base path without extension. The first existing file wins:

| State | Suffixes tried, in order | Fallback |
|---|---|---|
| Focused frame | `_active`, `_focused`, `active`, none | none (frame skipped) |
| Unfocused frame | `_inactive`, `_unfocused`, `inactive` | the focused frame |

State suffixes for buttons and other images are specified in `docs/SPEC.md`.
Always use absolute paths: relative paths resolve against Hyprland's working directory.
