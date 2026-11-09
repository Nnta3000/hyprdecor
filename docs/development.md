# Development

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
| `src/compat.hpp` | Small shims for Hyprland APIs that changed between Hyprland releases. |
| `tests/` | Unit tests for the pure-logic headers. |
| `assets/` | Sample frame sets (`xp`, `2000`). The `orange` set is local-only. |

---
