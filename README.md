# hyprdecor

A Hyprland plugin that draws **nine-patch window decorations**: pixel-art frames, title
bars, icons and buttons around your windows, configured from your Lua config.

The nine-patch format comes from Android. A 1 px border around a png stores metadata that
says which rows and columns stretch and where content may sit, so one small image scales
cleanly to any window size without distorting its corners.

<img width="747" height="587" alt="image" src="https://github.com/user-attachments/assets/2ce3cba3-1669-40d7-ae1d-37f58707acd7" />
<img width="754" height="628" alt="image" src="https://github.com/user-attachments/assets/483eb4c3-684b-4f32-9be8-e7b0adc6455a" />
<img width="698" height="582" alt="image" src="https://github.com/user-attachments/assets/27b81efa-4656-4e1b-909a-c3d7895e5616" />

> **Status:** works on Hyprland **0.57** with the **Lua config manager**. The full
> configuration reference is in [docs/configuration.md](docs/configuration.md); the design
> target is in [docs/SPEC.md](docs/SPEC.md).

hyprdecor decorates windows. Panels, taskbars and anything that is not attached to a
window's own frame belong in a shell such as quickshell.

---
## Documentation

| File | Read it if you want to |
|---|---|
| [docs/configuration.md](docs/configuration.md) | all configuration options, themes, rules and window-rule effects |
| [docs/build.md](docs/build.md) | dependencies, manual builds and Nix |
| [docs/ninepatch.md](docs/ninepatch.md) | make or fix nine-patch frame images |
| [docs/development.md](docs/development.md) | tests, nested testing and source layout |
| [docs/SPEC.md](docs/SPEC.md) | the design spec and target configuration |

---

## Install

Requires Hyprland **0.57** with the **Lua** config manager.

### hyprpm (recommended)

```bash
hyprpm add https://github.com/Nnta3000/hyprdecor
hyprpm update
hyprpm enable hyprdecor
```

`hyprpm` builds the plugin from source against the running Hyprland headers.

### Manual

Build it yourself with `make release`; dependencies and Nix instructions are in
[docs/build.md](docs/build.md).

A plugin that crashes takes the compositor with it, so try it in a nested instance first
(see [docs/development.md](docs/development.md)). To load it from your config, add this to
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
(`xp`, `2000`). The `orange` set used by the original setup is local-only and is not
redistributed.

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

**Use absolute paths.** Relative paths resolve against Hyprland's working directory, not
your config directory.

`setup()` validates everything when the config loads. A mistake (a missing file, a wrong
type) raises a Lua error that names the offending field, and nothing is half-applied.

Full reference: [docs/configuration.md](docs/configuration.md).

---

## Credits

Originally by CapsAdmin. Decorations inspired by the Android nine-patch format.

The `orange` frame set is based on the visual style created by
**[@P1PEGraphics](https://www.youtube.com/@P1PEGraphics)** for
[this video](https://www.youtube.com/watch?v=GPr_cM7Nyzk). It is used locally but is
**not redistributed** with this repository; the images are excluded by `.gitignore`.
See [`assets/orange/CREDITS.md`](assets/orange/CREDITS.md).
