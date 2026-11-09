# hyprdecor

A Hyprland plugin that draws **nine-patch window decorations**: pixel-art frames, title
bars, icons and buttons around your windows, configured from your Lua config.

The nine-patch format comes from Android. A 1 px border around a png stores metadata that
says which rows and columns stretch and where content may sit, so one small image scales
cleanly to any window size without distorting its corners.

<img width="747" height="587" alt="image" src="https://github.com/user-attachments/assets/2ce3cba3-1669-40d7-ae1d-37f58707acd7" />
<img width="754" height="628" alt="image" src="https://github.com/user-attachments/assets/483eb4c3-684b-4f32-9be8-e7b0adc6455a" />
<img width="698" height="582" alt="image" src="https://github.com/user-attachments/assets/27b81efa-4656-4e1b-909a-c3d7895e5616" />

> **Status:** works on Hyprland **0.56** with the **Lua config manager**. The config surface
> below is what works today. It is being extended into a regions-and-items layout (items on
> all four sides, vertical buttons, flexible placement); see [ROADMAP.md](ROADMAP.md) and
> [docs/SPEC.md](docs/SPEC.md). Expect additions, and a gradual move of the old `title` and
> `elements` blocks into that model (the old syntax will keep loading).

hyprdecor decorates windows. Panels, taskbars and anything that is not attached to a
window's own frame belong in a shell such as quickshell.

---

## Documentation

| File | Read it if you want to |
|---|---|
| this README | install the plugin and configure what works today |
| [docs/ninepatch.md](docs/ninepatch.md) | make or fix frame images (markers, naming, common mistakes) |
| [docs/SPEC.md](docs/SPEC.md) | see the target config and exact semantics of the finished plugin |
| [ROADMAP.md](ROADMAP.md) | see what is done, what is next and why |
| [AGENTS.md](AGENTS.md) | work on the code (rules, architecture, build and test) |

---

## Requirements

| | |
|---|---|
| Hyprland | 0.56.x, using the **Lua** config (`hyprland.lua`) |
| Compiler | C++23 |
| Libraries | cairo, pangocairo, pixman, libdrm, libinput, libudev, wayland-server, xkbcommon, Lua 5.5 (`pkg-config lua5.5`) |

The plugin must be built against the **same headers as the running Hyprland**. Hyprland
compares a version hash at load time and refuses mismatched builds.

All configuration goes through `hl.plugin.hyprdecor.setup({...})`, which only exists with
the Lua config manager. A legacy `hyprland.conf` can load the plugin but cannot configure it.

---

## Build

```bash
make debug      # Debug build   -> build/hyprdecor.so
make release    # Release build
```

`make` runs CMake into `build/`. The same build also produces the `ninepatch_test` binary
(pure-logic unit tests; run it with `./build/ninepatch_test`).

**Nix**

```bash
nix build        # plugin -> ./result/lib/libhyprdecor.so
nix run          # launch Hyprland with ./hyprland.conf
nix run .#debug  # same, under gdb
```

**hyprpm** (`hyprpm.toml` declares `output = "build/hyprdecor.so"`):

```bash
hyprpm add .
hyprpm update
hyprpm enable hyprdecor
```

---

## Install

A plugin that crashes takes the compositor with it, so try it in a nested instance first
(see [Testing](#testing)). To load it from your config, add this to
`~/.config/hypr/hyprland.lua` and then call `setup()`:

```lua
hl.plugin.load("/absolute/path/to/build/hyprdecor.so")
```

To try it in a running session without editing the config:

```bash
hyprctl plugin load /absolute/path/to/build/hyprdecor.so
hyprctl plugin list
hyprctl plugin unload /absolute/path/to/build/hyprdecor.so
```

Loading the plugin alone configures nothing: until `setup()` is called there is no frame
image, so you will not see a frame.

---

## Quick start

The smallest working config is a frame. The repo ships sample sets in `assets/`
(`xp`, `2000`). The `orange` set used by the development config is local-only and is
not redistributed.

```lua
hl.plugin.load("/absolute/path/to/build/hyprdecor.so")

hl.plugin.hyprdecor.setup({
  defaults = {
    scale  = 2,   -- one number: frame art and every element (see `defaults.scale`)
    border = {
      -- base path, no extension: resolves frame_active.png / frame_inactive.png
      texture = "/absolute/path/to/assets/xp/frame",
    },
  },
})
```

Then add a title and some images. This is the shape of the configuration used for
development:

```lua
hl.plugin.hyprdecor.setup({
  defaults = {
    -- One number for the whole frame: scales the frame art AND every element that does
    -- not set its own `scale`. Per-element `scale` wins over this.
    scale = 2,

    border = {
      texture          = "/absolute/path/to/frame",
      linear_filtering = false,   -- false = crisp pixels (nearest-neighbour)
      scale            = 2,       -- scales the frame art (pixel art at 2x)
    },

    title = {
      enabled = true,
      size    = 13,
      font    = "Sans",
      font_unfocused = "OrangeFont", -- optional: title font when the window is unfocused
      mode    = "fill",           -- the title fills the space between the elements
      align   = "left",           -- left | centre | right, inside its box
      text_offset_x = 9,
      text_offset_y = 0,
      ellipsize = true,           -- long titles become "..."
      background = { image = "/absolute/path/to/title.png" },
    },

    elements = {
      { image = "/absolute/path/to/eye.png",
        anchor = "top-left", offset_x = 19, offset_y = 9,
        scale = 2, linear_filtering = false, interactive = true },
      { image = "/absolute/path/to/close.png",
        anchor = "top-right", offset_x = -8, offset_y = 8,
        scale = 2, interactive = true },
    },
  },
})
```

**Use absolute paths.** Relative paths resolve against Hyprland's working directory, not
your config directory.

`setup()` validates everything when the config loads. A mistake (a missing file, a wrong
type) raises a Lua error that names the offending field, and nothing is half-applied.

---

## Configuration reference (what works today)

This reference follows the working development config. Keys not listed here may exist in
the source (`src/luaApi.cpp`); the target syntax is in [docs/SPEC.md](docs/SPEC.md).

### `defaults.border`: the frame

| Key | Description |
|---|---|
| `defaults.scale` | **Global** integer scale: applies to the frame art AND every element that does not set its own `scale`. Per-element `scale` and `border.scale` override it. |
| `texture` | Base path of the frame images, no extension. Required. |
| `linear_filtering` | `true` smooth scaling, `false` nearest-neighbour. Use `false` for pixel art. |
| `scale` | Integer scale of the frame art (2 = pixel art at 2x). The space the frame reserves around the window scales with it. Overrides `defaults.scale` for the frame. |
| `thickness` | Optional override of the reserved space per side. Leave it out and the nine-patch markers decide, which is usually right. |

The frame is one nine-patch drawn over the whole decoration box, so corners stay
continuous. The space it reserves around the window comes from the image: its content
padding markers, falling back to the border sizes. See [docs/ninepatch.md](docs/ninepatch.md).

**How the image files are found.** `texture` is a base path. The first existing file wins:

| Frame | Tried in order | If none match |
|---|---|---|
| Focused | `_active.png`, `_focused.png`, `active.png`, `.png` | no frame image (a plain fallback bar may be drawn) |
| Unfocused | `_inactive.png`, `_unfocused.png`, `inactive.png` | the focused frame |

### `defaults.title`: the title text

| Key | Description |
|---|---|
| `enabled` | Draw the window title. |
| `size`, `font` | Font size in points and a Pango font family (for example `"Sans"`). |
| `font_unfocused` | Font family for the title while the window is not focused. Optional; omit it to keep using `font`. Swapped like `frame_inactive`, so the title is re-rendered on focus change. |
| `mode` | `"fill"`: the title spans what the elements leave free. `"box"`: the title is placed like an element. |
| `gap` | Fill mode: `{ left, right, top, bottom }`. Left and right are the distance from neighbouring elements; top and bottom inset the box from the band edges. |
| `align` | `left`, `centre` or `right`, inside the title box. |
| `text_offset_x`, `text_offset_y` | Move the text inside its box. |
| `ellipsize` | Shorten long titles with an ellipsis. |
| `background.image` | Image drawn behind the text, in the same box so they cannot drift apart. |

### `defaults.elements`: images and icons

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

### `themes` and `rules`: per-window frame textures

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

The syntax for writing these as Lua window rules has not been verified yet (it is on the
[roadmap](ROADMAP.md)). An invalid rule aborts the whole config, so try it in a throwaway
config first.

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
   See [docs/ninepatch.md](docs/ninepatch.md).
5. Enable `debug:enable_stdout_logs` and look for lines prefixed `[HYPRDECOR]`.
   When diagnosing, save the whole log to a file and search that, rather than filtering live.

---

## Testing

A plugin crash takes down the compositor that loaded it. Test in a **nested** Hyprland
instance (a window inside your session) with a throwaway config, never your real one:

```bash
# from a terminal inside your real session
Hyprland -c /path/to/test.lua 2>&1 | tee ~/full.log
```

Pure logic (nine-patch parsing, layout arithmetic) has unit tests that do not need Hyprland:

```bash
make debug && ./build/ninepatch_test
```

Only add the plugin to your real config after the nested instance has survived a workout:
open and resize windows, change focus, switch workspaces, fullscreen, hover and click
elements. Crash reports land in `~/.cache/hyprland/`.

---

## Layout of the source

| File | Role |
|---|---|
| `src/main.cpp` | `PLUGIN_INIT` / `PLUGIN_EXIT`, event listeners, window-rule effects, `hyprctl` command. |
| `src/config.hpp` | The whole configuration as one struct, filled by `setup()`. |
| `src/luaApi.{hpp,cpp}` | `hl.plugin.hyprdecor.setup()`: parsing and validation of the Lua table. |
| `src/plugin.{hpp,cpp}` | `CPlugin`: applies the config, resolves and loads textures. |
| `src/hyprWindowDecorator.{hpp,cpp}` | The window decoration: layout, drawing, mouse and touch input. |
| `src/renderPassElement.{hpp,cpp}` | The render-pass element that draws the decoration. |
| `src/ninepatch.hpp` | Nine-patch parsing and composition (no Hyprland dependency, unit-tested). |
| `src/titlebox.hpp` | Title box arithmetic (no Hyprland dependency, unit-tested). |
| `src/util.hpp` | Texture upload and app-icon lookup. |
| `src/compat.hpp` | Small shims for Hyprland APIs that changed in 0.56. |
| `tests/` | Unit tests for the pure-logic headers. |
| `assets/` | Sample frame sets (`xp`, `2000`). The `orange` set is local-only. |

---

## Credits

Originally by CapsAdmin. Decorations inspired by the Android nine-patch format.

The `orange` frame set is based on the visual style created by
**[@P1PEGraphics](https://www.youtube.com/@P1PEGraphics)** for
[this video](https://www.youtube.com/watch?v=GPr_cM7Nyzk). It is used locally but is
**not redistributed** with this repository; the images are excluded by `.gitignore`.
See [`assets/orange/CREDITS.md`](assets/orange/CREDITS.md).
