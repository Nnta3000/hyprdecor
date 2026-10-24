# hyprdecor

A Hyprland plugin that draws **nine-patch (`.9.png`) window decorations**: custom
title bars and/or fancy frames around windows.

The nine-patch format comes from Android. A 1 px border around the image stores
metadata that defines which rows/columns stretch and where content is padded, so
one small image scales cleanly to any window size without distortion.

<img width="747" height="587" alt="image" src="https://github.com/user-attachments/assets/2ce3cba3-1669-40d7-ae1d-37f58707acd7" />
<img width="754" height="628" alt="image" src="https://github.com/user-attachments/assets/483eb4c3-684b-4f32-9be8-e7b0adc6455a" />
<img width="698" height="582" alt="image" src="https://github.com/user-attachments/assets/27b81efa-4656-4e1b-909a-c3d7895e5616" />

> **Status:** ported to Hyprland **0.56**. The plugin compiles and loads; drawing and
> input paths are still being validated. See [Testing](#testing).

---

## Requirements

| | |
|---|---|
| Hyprland | 0.56.x (`pkg-config hyprland`) |
| Compiler | C++23 (`set(CMAKE_CXX_STANDARD 23)`) |
| Deps | libdrm, libinput, libudev, pangocairo, pixman, wayland-server, xkbcommon, cairo |

The plugin must be built against the **same headers as the running Hyprland**.
Hyprland checks this at load time and refuses mismatched builds.

---

## Build

### CMake / Make (system packages)

```bash
make debug      # Debug build  -> build/hyprdecor.so
make release    # Release build
```

`make` runs CMake into `build/`. The output is `build/hyprdecor.so`.

### Nix

```bash
nix build                    # plugin -> ./result/lib/libhyprdecor.so
nix run                      # launch Hyprland with ./hyprland.conf
nix run .#debug              # same, under gdb
```

### hyprpm

`hyprpm.toml` declares `output = "build/hyprdecor.so"` and `build = ["make all"]`:

```bash
hyprpm add .          # register this checkout
hyprpm update
hyprpm enable hyprdecor
```

---

## Install / enable

The plugin is enabled by **loading it into the running compositor**. A plugin that
fails to load can take the compositor down with it, so test in a throwaway instance
first — see [Testing](#testing).

```bash
hyprctl plugin load /absolute/path/to/build/hyprdecor.so
hyprctl plugin list
hyprctl plugin unload /absolute/path/to/build/hyprdecor.so
```

To load it at startup instead, add it to your config:

**Lua config** (`~/.config/hypr/hyprland.lua`):

```lua
hl.plugin.load("/absolute/path/to/build/hyprdecor.so")
```

**Legacy config** (`~/.config/hypr/hyprland.conf`):

```conf
plugin = /absolute/path/to/build/hyprdecor.so
```

---

## Quick start: a nine-patch title bar

The repo ships two sample sets: `assets/xp/` (XP-Luna style, 92×92 frames) and
`assets/2000/` (2000-era style, 66×66 frames). Each has `frame_active.png` /
`frame_inactive.png` plus button state images.

**Lua config:**

```lua
hl.plugin.load("/absolute/path/to/build/hyprdecor.so")

hl.plugin("hyprdecor", {
    -- "frame" resolves to frame_active.png / frame_inactive.png
    ninepatch_texture        = "/absolute/path/to/assets/xp/frame",
    ninepatch_linear_filtering = 1,
    ninepatch_repeat         = 0,

    -- This is what makes the frame visible: space reserved outside the window.
    decoration_offset_top    = 22,
    decoration_offset_left   = 5,
    decoration_offset_right  = 5,
    decoration_offset_bottom = 5,

    decoration_padding       = 1,
    decoration_title_enabled = 1,
    decoration_title_size    = 9,
    decoration_title_align   = 0,
    decoration_title_placement = "top",
    bar_text_font            = "Tahoma Bold",

    hyprdecor-button = "0, hyprctl dispatch killactive, /absolute/path/to/assets/xp/close",
    hyprdecor-button = "0, hyprctl dispatch fullscreen 1, /absolute/path/to/assets/xp/maximize",
})
```

**Legacy config:**

```conf
plugin = /absolute/path/to/build/hyprdecor.so

plugin {
    hyprdecor {
        ninepatch_texture   = /absolute/path/to/assets/xp/frame
        decoration_offset_top = 22
        decoration_offset_left = 5
        decoration_offset_right = 5
        decoration_offset_bottom = 5

        decoration_title_enabled = 1
        decoration_title_size = 9
        bar_text_font = Tahoma Bold

        hyprdecor-button = 0, hyprctl dispatch killactive, /absolute/path/to/assets/xp/close
        hyprdecor-button = 0, hyprctl dispatch fullscreen 1, /absolute/path/to/assets/xp/maximize
    }
}
```

**Important:** use **absolute paths**. Relative paths resolve against Hyprland's
working directory, not your config directory, so `./assets/...` will usually fail
silently and fall back to a plain colored bar.

If the texture is missing or fails to parse, the plugin falls back to a solid
`bar_color` rectangle, so "no frame, just a block" means the path is wrong.

---

## How the nine-patch textures are found

`ninepatch_texture` is a **base path without extension**. The plugin appends
suffixes and uses the first file that exists:

| Lookup | Suffixes tried, in order | Fallback if nothing matches |
|---|---|---|
| Active/focused frame | `_active.png`, `_focused.png`, `active.png`, `.png` | texture loading skipped |
| Inactive frame | `_inactive.png`, `_unfocused.png`, `inactive.png` | active frame |
| Buttons | `_normal/_active/_focused`, `_unfocused/_inactive`, `_hover`, `_pressed/_clicked` | previous state |

So `ninepatch_texture = .../assets/xp/frame` resolves to `frame_active.png` and
`frame_inactive.png`. You can also set `ninepatch_active` / `ninepatch_inactive`
explicitly to override this.

### The `.9.png` metadata format

The image has a **1 px border** that the plugin reads and then crops off. Opaque
black pixels in that border mark regions:

```
        ┌─────────────── top edge: X range that stretches ──────────────┐
        ↓                                                              ↓
   ┌────┬──────────────────────────────────────────────────────────────┐
   │    │                                                              │ ← left edge: Y range
   │    │                    image content                             │   that stretches
   │    │                                                              │
   └────┴──────────────────────────────────────────────────────────────┘
        └────────────── bottom edge: X range that pads content ────────┘
                                                                       right edge: Y padding
```

| Border | Meaning |
|---|---|
| Top | Horizontal stretch region |
| Left | Vertical stretch region |
| Bottom | Horizontal content padding |
| Right | Vertical content padding |

Corners stay unscaled; the marked middle band stretches (or tiles, see
`ninepatch_repeat`). Content padding positions the title text and buttons.

Both shipped sets parse as:

| Set | Size | Stretch X | Stretch Y | Pad X | Pad Y |
|---|---|---|---|---|---|
| `xp` | 92×92 | 6–85 | 31–86 | 4–87 | 31–86 |
| `2000` | 66×66 | 21–42 | 22–61 | 3–62 | 22–62 |

---

## Options

All options live under `plugin:hyprdecor:` (`plugin { hyprdecor { ... } }` in legacy
config, `hl.plugin("hyprdecor", {...})` in Lua). All options are `long`/number
unless noted.

### Frame / nine-patch

| Option | Default | Description |
|---|---|---|
| `enabled` | `1` | Master switch. `0` disables all decorations. |
| `ninepatch_texture` | `""` | Base path of the frame images (no extension). |
| `ninepatch_active` | `""` | Explicit focused frame path. Overrides the lookup. |
| `ninepatch_inactive` | `""` | Explicit unfocused frame path. Falls back to active. |
| `ninepatch_middle_alpha` | `0.0` | Alpha of the **center** patch. `0` = see-through middle. Use `1.0` for a solid frame image, `0.0` for a frame that only draws its borders. |
| `ninepatch_repeat` | `0` | `1` tiles the middle patches, `0` stretches them. |
| `ninepatch_linear_filtering` | `1` | `1` smooth scaling, `0` nearest-neighbour (crisp pixels). |

### Reserved space (this is what puts the frame on screen)

The decoration is drawn in space **outside** the window, so these must be non-zero
or the frame overlaps the window and mostly hides behind Hyprland's own border.

| Option | Default | Description |
|---|---|---|
| `decoration_offset_top` | `0` | Pixels reserved above the window (title bar height). |
| `decoration_offset_left` | `0` | Pixels reserved left of the window. |
| `decoration_offset_right` | `0` | Pixels reserved right of the window. |
| `decoration_offset_bottom` | `0` | Pixels reserved below the window. |
| `decoration_inset` | `0` | Inset applied to the frame. |
| `decoration_padding` | `7` | Padding between frame edge and title/buttons. |
| `decoration_title_placement` | `"top"` | `top`, `bottom`, `left`, or `right`. |
| `bar_precedence_over_border` | `0` | `1` draws over Hyprland's border instead of behind it. |
| `bar_part_of_window` | `1` | Whether the bar counts as part of the window. |
| `decoration_render_above` | `0` | Render the decoration above instead of below the window. |

### Title text

| Option | Default | Description |
|---|---|---|
| `decoration_title_enabled` | `1` | Draw the window title. |
| `decoration_title_size` | `10` | Font size in points. |
| `decoration_title_align` | `0.5` | `0.0` = left, `0.5` = center, `1.0` = right. |
| `bar_text_font` | `"Sans"` | Pango font description, e.g. `"Tahoma Bold 9"`. |
| `col.text` | `rgba(ffffffff)` | Title text color. |

### Buttons

The `hyprdecor-button` keyword adds a button to the bar. Repeat it per button; they
are laid out in the order defined.

**Legacy config** (`keyword = value`, comma separated):

```conf
hyprdecor-button = size, command, active_tex[, inactive_tex[, hover_tex[, pressed_tex]]]
```

**Lua config** (string form):

```lua
hyprdecor-button = "0, hyprctl dispatch killactive, /abs/path/assets/xp/close"
```

| Field | Description |
|---|---|
| `size` | Button size in px. `0` = use the image's own size. |
| `command` | Dispatched when clicked. Usually `hyprctl dispatch ...`. |
| `active_tex` | Base path for the button image. With only this field given, state suffixes are auto-resolved (`_normal`, `_unfocused`, `_hover`, `_pressed`, …). |
| `inactive_tex`, `hover_tex`, `pressed_tex` | Optional explicit paths for each state. |

| Option | Default | Description |
|---|---|---|
| `bar_buttons_alignment` | `"right"` | `left` or `right`. |
| `bar_button_padding` | `5` | Gap between buttons. |
| `on_double_click` | `""` | Command run when the bar is double-clicked, e.g. `hyprctl dispatch fullscreen 1`. |

### Appearance

| Option | Default | Description |
|---|---|---|
| `bar_color` | `rgba(33333388)` | Solid bar color, used when no texture resolves, and under blur. |
| `bar_blur` | `0` | Blur behind a translucent bar. |
| `bar_outline_color` | `rgba(ffffffff)` | Solid outline color (see below). |
| `bar_outline_width` | `1` | Outline thickness in px. `0` disables it. |
| `bar_outline_always` | `1` | `1` = draw outline over textures too, `0` = only when no texture resolved. |

### App icon

| Option | Default | Description |
|---|---|---|
| `decoration_appicon_enabled` | `1` | Show the app icon in the bar. |
| `decoration_appicon_offset` | `0 0` | X/Y offset in px. |

---

## Window rules

Three effects are registered by the plugin:

| Rule | Description |
|---|---|
| `hyprdecor:no_bar` | Hide the decoration for matching windows. |
| `hyprdecor:bar_color` | Override `bar_color` for matching windows (rgba color). |
| `hyprdecor:title_color` | Override `col.text` for matching windows (rgba color). |

```conf
windowrulev2 = hyprdecor:no_bar, class:^(steam)$
windowrulev2 = hyprdecor:bar_color rgba(ff0000ff), class:^(firefox)$
```

---

## Debugging and the outline placeholder

If nothing appears, work through this order:

1. `hyprctl plugin list` — is the plugin loaded?
2. `hyprctl configerrors` — did any option fail to parse?
3. Are `decoration_offset_top/left/right/bottom` non-zero? With all-zero offsets
   the frame has nowhere to draw and hides behind the window border.
4. Are the texture paths **absolute** and correct? A wrong path silently falls back
   to `bar_color`.
5. `print`/log: the plugin logs with the `[HYPRDECOR]` prefix at debug level.
   Enable `debug:enable_stdout_logs = true` and `debug:log_level = 0` to see it.

### Outline placeholder

A solid outline is drawn around the decoration box, useful to confirm the decoration
exists before textures are set up:

```conf
bar_outline_color  = rgba(ffffffff)   # white
bar_outline_width  = 3
bar_outline_always = 1
```

Keep `bar_outline_width` small (1–3 px). It is drawn **inside** the decoration box,
so with zero offsets a 1 px line sits under Hyprland's own border and is invisible.

---

## Testing

A plugin crash takes down the compositor that loaded it. Test in a **nested**
Hyprland instance (a window inside your session) and load the plugin by hand.

A ready-made harness lives in `~/Documents/hyprtest/`:

```bash
# from a terminal inside your real session
Hyprland -c /home/agent/Documents/hyprtest/test.lua

# inside the nested window
hyprctl plugin load /home/agent/Documents/hyprdecor/build/hyprdecor.so
hyprctl plugin list
```

Logs land in `$XDG_RUNTIME_DIR/hypr/<signature>/hyprland.log`; crash reports in
`~/.cache/hyprland/`.

Only add `plugin = ...` to your real config once the nested instance has survived a
workout: open/resize windows, switch workspaces, fullscreen, click buttons, drag by
the bar.

---

## Layout of the source

| File | Role |
|---|---|
| `src/main.cpp` | `PLUGIN_INIT`/`PLUGIN_EXIT`, event registration, window rules. |
| `src/plugin.{hpp,cpp}` | `CPlugin`: config values, frame/button texture loading. |
| `src/hyprWindowDecorator.{hpp,cpp}` | `IHyprWindowDecoration`: layout, drawing, mouse/touch input. |
| `src/renderPassElement.{hpp,cpp}` | Render-pass element that draws the decoration. |
| `src/util.hpp` | Nine-patch parsing, texture upload, app icon lookup. |
| `src/compat.hpp` | Small shims for Hyprland APIs that moved/were removed in 0.56. |

---

## Credits

Originally by CapsAdmin. Decorations inspired by the Android nine-patch format.
