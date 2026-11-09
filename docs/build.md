# Building from source

## Requirements

| | |
|---|---|
| Hyprland | 0.57.x, using the **Lua** config (`hyprland.lua`) |
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

`hyprpm` performs the build automatically; see the install section in the README.

---
