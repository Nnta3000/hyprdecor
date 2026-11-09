# Configuration reference

The options below are the supported configuration surface.

## `defaults.border`: the frame

| Key | Description |
|---|---|
| `defaults.scale` | **Global** integer scale: applies to the frame art AND every element that does not set its own `scale`. Per-element `scale` and `border.scale` override it. |
| `texture` | Base path of the frame images, no extension. Required. |
| `linear_filtering` | `true` smooth scaling, `false` nearest-neighbour. Use `false` for pixel art. |
| `scale` | Integer scale of the frame art (2 = pixel art at 2x). The space the frame reserves around the window scales with it. Overrides `defaults.scale` for the frame. |
| `thickness` | Optional override of the reserved space per side. Leave it out and the nine-patch markers decide, which is usually right. |

The frame is one nine-patch drawn over the whole decoration box, so corners stay
continuous. The space it reserves around the window comes from the image: its content
padding markers, falling back to the border sizes. See [ninepatch.md](ninepatch.md).

**How the image files are found.** `texture` is a base path. The first existing file wins:

| Frame | Tried in order | If none match |
|---|---|---|
| Focused | `_active.png`, `_focused.png`, `active.png`, `.png` | no frame image (a plain fallback bar may be drawn) |
| Unfocused | `_inactive.png`, `_unfocused.png`, `inactive.png` | the focused frame |

## `defaults.title`: the title text

| Key | Description |
|---|---|
| `enabled` | Draw the window title. |
| `size`, `font` | Font size in points and a Pango font family (for example `"Sans"`). |
| `font_unfocused` | Font family for the title while the window is not focused. Optional; omit it to keep using `font`. Swapped like `frame_inactive`, so the title is re-rendered on focus change. |
| `mode` | `"fill"`: the title spans what the elements leave free. `"box"`: the title is placed like an element. |
| `margin` | Space around the title item: `{ left, right, top, bottom }`. Negative sides can tuck it under a neighbouring icon. |
| `align` | `left`, `centre` or `right`, inside the title box. |
| `text_offset_x`, `text_offset_y` | Move the text inside its box. |
| `ellipsize` | Shorten long titles with an ellipsis. |
| `background.image` | Image drawn behind the text, in the same box so they cannot drift apart. |

## `defaults.elements`: images and icons

A list of images placed in the top band and drawn in order.

| Key | Description |
|---|---|
| `image` | Absolute path to a png. |
| `anchor` | `"top-left"`, `"top-centre"` or `"top-right"`. |
| `offset_x`, `offset_y` | Distance from the anchor, in logical pixels. |
| `scale` | Integer scale for this image. Overrides `defaults.scale`. |
| `linear_filtering` | Overrides the frame's filtering for this image. |
| `interactive` | Reacts to the pointer: enables the `_hovered` and `_pressed` images. |

**State images.** They are found from `image` by suffix, placed before the extension
(`close.png` becomes `close_hovered.png`). A missing file falls back to the base image, so
adding states is optional.

| Suffix | When it is shown |
|---|---|
| `_unfocused` | the window is not focused |
| `_hovered` | the pointer is over the element (`interactive = true`) |
| `_pressed` | the pointer is pressed on it (`interactive = true`) |

## `themes` and `rules`: per-window frame textures

A theme is a partial frame override. It inherits every field it does not set from
`defaults.border`:

```lua
hl.plugin.hyprdecor.setup({
  defaults = { --[[ ... ]] },
  themes = {
    terminal = {
      border = {
        texture = "/absolute/path/to/frame_inset",
        linear_filtering = false,
        -- Optional: scale, thickness, inset, repeat.
      },
    },
  },
  rules = {
    { class = "kitty", theme = "terminal" },
    { class = "foot",  theme = "terminal" },
  },
})
```

`class` matches the window's app id (X11 class / Wayland app_id). Rules are checked
first-match-wins; a window with no matching rule uses the default frame.

A `hyprdecor:theme` window-rule effect is also registered and takes precedence over the
`rules` list when present. The Lua syntax for custom-effect rules is still unverified on
this build, so the plugin-side `rules` list is the safe path.

---

## Window rules

Four window-rule effects are registered:

| Effect | Description |
|---|---|
| `hyprdecor:no_bar` | Hide the decoration for matching windows. |
| `hyprdecor:bar_color` | Override the solid bar colour (used when no frame texture resolves). |
| `hyprdecor:title_color` | Override the title text colour. |
| `hyprdecor:theme` | Select a named `themes` frame by effect value. |

The `hyprdecor:theme` effect is registered, but the Lua window-rule binding on Hyprland
0.57 uses a fixed effect list, so custom effects are not reachable from `hl.window_rule()`.
Use the plugin-side `rules` list instead; see the themes section above.

**Element actions** (`defaults.elements[i].action`) are `close`, `maximize`, `fullscreen`,
`pin`, `float` and `minimize`. Setting one makes the element interactive on its own, so `_hovered` and
`_pressed` art are picked up without listing `interactive = true`. `maximize` and
`fullscreen` are **toggles**: pressing again restores the tiled size, and the `_on` art is
shown while the window is maximised or fullscreen. An element with **no** `action` is
decoration only: it never hovers, never takes a press, and a click on it drags the window
like the rest of the band.

---

## Inspecting and debugging

```bash
hyprctl hyprdecor status     # the plugin's current state, as text
```

If nothing appears, work through this list:

1. `hyprctl plugin list`: is the plugin loaded?
2. Did `setup()` run, and did it fail? Check Hyprland's config error output
   (`hyprctl configerrors`) and the log; the message names the field.
3. Are all paths absolute, and can the compositor's user read them?
4. Does the frame png have markers? Without them the whole image stretches as one piece.
   See [ninepatch.md](ninepatch.md).
5. Enable `debug:enable_stdout_logs` and look for lines prefixed `[HYPRDECOR]`.
   When diagnosing, save the whole log to a file and search that, rather than filtering live.

---
