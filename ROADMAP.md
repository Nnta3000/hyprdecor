# Roadmap — Lua-first widget API

Status: **planning**. Nothing here is implemented yet beyond the notes marked *verified*.

## Why

The current plugin exposes ~30 options through `HyprlandAPI::addConfigValue` and one
`addConfigKeyword`, all of which the 0.56 headers mark deprecated
(`deprecated: please use V2`).  On a Lua config manager this is not merely
deprecated, it is broken:

* `addConfigKeyword` cannot work, because the message from Hyprland is literally
  *"Keyword can't change with legacy parsers, use evals"* — the Lua config manager
  has no stateful keyword parser.
* `hyprdecor-button = ...` is therefore unreachable from `hyprland.lua` at all.
* `hl.plugin.hyprdecor` is `nil`: a `.so` load does **not** create a Lua namespace.
  Only `addLuaFunction` does.

## Verified against Hyprland 0.56.2 (`/usr/bin/Hyprland`, headers in `/usr/include/hyprland`)

| Fact | Evidence |
|---|---|
| `addLuaFunction(handle, ns, name, fn)` is exported | `nm -D` → `T addLuaFunction` |
| `removeLuaFunction` exported | `T removeLuaFunction` |
| `addConfigValueV2(handle, SP<IValue>)` exported | `T addConfigValueV2` |
| Lua namespace errors exist in binary | `hl.plugin.{} already exists and is not a namespace table`, `hl.plugin: this function is not callable` |
| Plugin values must be named `plugin:<ns>:<key>` | binary string `name must start with plugin:` |
| `hl.plugin` has only `load` and a dynamic string index | `/usr/share/hypr/stubs/hl.meta.lua` `HL.PluginNamespace` |
| `HL.ConfigOpt` has **no** `plugin` field | same stubs; only `general`, `decoration`, … |
| Hyprland links **both** Lua 5.4 and 5.5 | `ldd /usr/bin/Hyprland` |
| Lua **5.5** headers are available and correct | `/usr/include/lua.h`, `LUA_VERSION_NUM 505`, `pkg-config lua5.5` → 5.5.1 |

✅ **Lua ABI resolved.** Hyprland links `liblua.so.5.5` directly; `liblua5.4` comes in
transitively via `libhyprtoolkit` and is harmless as long as we never call it.
Plugin callbacks receive a 5.5 `lua_State*`, and the headers
(`/usr/include/lua.h`, unversioned, `LUA_VERSION_NUM 505`) plus `-llua`
(`liblua.so` → `liblua.so.5.5.1`) match that runtime exactly. No vendoring needed.

⚠️ **Rule:** never include `/usr/include/lua5.4/*` and never link `liblua5.4`. Use
`pkg-config lua5.5` only.

## Target API

```lua
hl.plugin.load("/abs/path/hyprdecor.so")

hl.plugin.hyprdecor.setup({
    assets = { close = "/abs/buttons/close" },

    defaults = {
        border  = { texture = "/abs/frame", thickness = 22, alpha = 1.0 },
        buttons = {
            { id = "close",    asset = "close",    action = { type = "dispatch", name = "killactive" },
              position = { anchor = "top-right", offset_x = -8, offset_y = 4 } },
            { id = "maximize", asset = "maximize", action = { type = "dispatch", name = "fullscreen 1" },
              position = "top-left" },
        },
        layout = {
            { type = "button", id = "menu" },
            { type = "title",  align = "center" },
            { type = "button_group", buttons = { "minimize", "maximize", "close" }, align = "right" },
        },
    },

    rules = {
        { match = { class = "firefox" },  border = { thickness = 16 } },
        { match = { floating = true },    border = { texture = "/abs/floating-frame" } },
    },
})
```

Design rules:

1. **One public function.** `setup()` takes the whole configuration. Avoid a
   proliferation of `configure()`/`button()`/`set_rule()` entry points.
2. **Flat V2 values only for scalars** that must be settable as `plugin:hyprdecor:*`.
   Structured data (button lists, rules, layouts) goes through `setup()`.
3. **Actions are typed**, never raw `hyprctl` strings:
   `{ type = "dispatch", name = "..." }` now, `{ type = "lua", fn = ... }` later.
   This keeps shell execution out of the model and leaves room for other providers.
4. **Anchors before groups.** `position = "top-right"` shorthand plus
   `position = { anchor = ..., offset_x = ..., offset_y = ... }`.
   `layout`/`button_group` is explicitly **not** a priority.
5. **Rules are partial overrides** matched by `id`, with documented precedence
   (first matching rule wins per-property, or last-wins — pick one and document it).
6. **Hyprdecor stays self-contained.** Quickshell or any external renderer is an
   optional provider, never a dependency. An external Wayland surface cannot be
   embedded in a decoration; that needs its own transport and lifecycle protocol.

## Phases

### Phase 0 — unblock (do first)
- [x] Resolve the Lua 5.4/5.5 ABI question — **5.5 headers are present and correct**
      (`/usr/include/lua.h`, `LUA_VERSION_NUM 505`, `pkg-config lua5.5` = 5.5.1).
- [ ] Confirm `hl.config({ ["plugin:hyprdecor:key"] = value })` actually applies,
      via `hyprctl configerrors` + `hyprctl getoption`.

### Phase 1 — V2 config migration (no new features)
- [ ] Replace all `addConfigValue` calls with `addConfigValueV2` + typed
      `Config::Values::C*Value` objects.
- [ ] Replace `getConfigValue(...)->getDataStaticPtr()` casts with `->value()`.
      This deletes the class of null-deref bug that crashed the plugin on load.
- [ ] Remove `addConfigKeyword` (`hyprdecor-button`) entirely.
- [ ] Update README to the new option surface.

### Phase 2 — `setup()` (milestone)
- [ ] `addLuaFunction(handle, "hyprdecor", "setup", ...)`.
- [ ] Parse and **validate** the table; report field-path errors instead of crashing.
- [ ] `defaults.border` → texture, thickness, alpha.
- [ ] `defaults.buttons` → list with `position` anchor + offsets, typed actions.
- [ ] `rules` → matching on `class`, `title`, `floating`, `fullscreen` with partial
      overrides.
- [ ] Per-window widget state; cleanup on window close and plugin unload.

### Phase 3 — providers
- [ ] Provider interface: `create / update / render / destroy`, per-window state.
- [ ] `text` provider (title, `{title}` / `{class}` interpolation).
- [ ] `image` provider.
- [ ] Redraw-on-animation: provider signals damage while animating; no polling.
- [ ] One animated custom-renderer demo to prove the boundary.

### Phase 4 — later
- [ ] `layout` / `button_group` (explicitly low priority).
- [ ] Lua action callbacks; external action providers.
- [ ] External renderer transport (separate protocol, not part of the plugin core).

## Non-goals for the first milestone

- An animation scripting language.
- Any external-process rendering protocol.
- Quickshell as a dependency.

## Immediate practical goal

Regardless of the roadmap above, the **XP example must render**. The nine-patch
frame path already works in principle; what is missing is correct configuration
reaching it. That is the Phase 0/1 exit criterion.
