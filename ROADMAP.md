# Roadmap

This file is the plan: what is built, what is next, and the decisions behind it.
How to work is in `AGENTS.md`. What the finished config must do is in `docs/SPEC.md`.

> This file replaces the earlier "Lua-first widget API" roadmap. Its verified facts are kept
> below; its phase plan was replaced because the project moved to a single `setup()`
> config and a regions-and-items layout model (decisions D1 to D4).

## Background (why the config is Lua-only)

On Hyprland 0.56 with the Lua config manager, the old flat options are not merely
deprecated, they are broken:

- `addConfigKeyword` cannot work ("Keyword can't change with legacy parsers, use evals"), so
  `hyprdecor-button = ...` is unreachable from `hyprland.lua`.
- `hl.plugin.hyprdecor` is `nil` after a plain `.so` load. Only `addLuaFunction` creates the
  namespace.


## Where we are

Present in the code at the last upload:

- [x] `hl.plugin.hyprdecor.setup({...})` through `addLuaFunction`, validated, applied atomically.
- [x] One config struct (`SConfig`) filled by `setup()`.
- [x] Lua 5.5 ABI resolved (see the verified table).
- [x] Nine-patch frame, title (fill and box modes), image elements with three top anchors,
      state textures, `hyprctl hyprdecor status` (text output).
- [x] Direct GL texture upload helper (fixed the black-texture bug).
- [ ] Legacy button code (`_hover`, `_normal`, `_clicked` suffixes) is still in `plugin.cpp`.
      Retire it when buttons become items (C3/C4).

Known problems and unknowns are tracked in `AGENTS.md` section 4, not here.

## Decisions (proposed, confirm or change)

| # | Decision | Replaces | Why |
|---|---|---|---|
| D1 | No flat `plugin:hyprdecor:*` values. Everything goes through `setup()`. | Phase 1, the `addConfigValueV2` migration | One source of truth. Flat values were never committed under the Lua manager and caused silent wrong defaults. Revisit only if live `hl.config` tweaking is wanted; that needs the check listed under "To verify". |
| D2 | Actions are named (`close`, `maximize`, ...) plus a typed `dispatch = "name args"`. The plugin never spawns shells. | `{ type = "dispatch" }` objects and the earlier `exec` idea | Keeps shell execution out of the model, matching the old design rule 3. |
| D3 | Per-window variation uses Hyprland window-rule effects (`hyprdecor:theme`, `hyprdecor:no_bar`, ...) plus named `themes`. | Plugin-side `rules = { match = ... }` | Reuses Hyprland's matcher instead of maintaining a second one. Fall back to plugin-side rules only if the effect syntax proves unusable. |
| D4 | Regions and items with flow layout. | The `buttons` list with anchors, and `layout` / `button_group` (which were "not a priority") | Vertical buttons on any side and under the window are now a core goal, and flow removes the "measured from what?" problem. |
| D5 | State suffixes: `_unfocused`, `_hovered`, `_pressed`, `_on`. | The legacy `_hover`, `_normal`, `_clicked` | One vocabulary. The old names live only in the legacy button path and go with it. |

## Milestones

### M1: stabilise before new features
- [ ] Rebuild frame, title and buttons textures when their size differs from the target box
      (stretched stale frame on resized unfocused windows).
- [ ] Resolve frame metrics once in logical px (the unit mixing in `bandBox()`).
- [ ] Content markers on the shipped frame art (see `docs/ninepatch.md`).
- [ ] Separate thread, not the plugin: the maximised-on-open bug. It reproduces without the
      plugin.

### M2: layout engine (one commit at a time; the user tests between commits)

Each commit ends with the user testing. The next one starts only after that.

| # | Commit | Done when |
|---|---|---|
| C0 | Baseline | Screenshot of current `test.lua`; unit tests green; recorded in the repo. |
| C1 | Pure layout + resolved rect list + `status` JSON, **top band only**, behaviour-preserving | `test.lua` renders pixel-identical to C0. Hover hit-areas equal drawn rectangles (status list matches). Unit tests assert exact rects. |
| C2 | Left, right and bottom regions with asymmetric extents; frame still one nine-patch | Status JSON shows correct `reserved`; windows sized correctly; corners continuous; probe log consistent. |
| C3 | Unified items: `button`, `text`, `spacer`, flow, fill weights, gap, align; title becomes an item; legacy config desugars | Old `test.lua` unchanged in output; new column of buttons works on the left and bottom. |
| C4 | `visible_when` / `collapse`, `_on` state, actions (close, maximize, fullscreen, pin, float, drag, resize, dispatch) | Every action works; hidden items keep their slot. |
| C5 | Text templates, `rotate`, drag and resize grips, `hyprdecor:theme` window rule | Per-window themes work; vertical text renders. |
| C6 | Polish | Docs, example configs, deprecation notice, the target config in `docs/SPEC.md` section 2 loads as written. |

### M3: later
- [ ] Redraw-on-animation: items signal damage while animating, no polling.
- [ ] Lua action callbacks (re-entrancy design first).
- [ ] External renderer transport (separate protocol, not part of the plugin core).
- [ ] Per-item hover animation, tooltips, group tabs, urgent flash.

## Non-goals

- An animation scripting language.
- Any external-process rendering protocol in the core plugin.
- Quickshell as a dependency. It stays optional; the plugin only exposes state for it.
- Taskbars, panels and cables between windows (they belong in quickshell).

## To verify before relying on it

- Whether `hl.config({ ["plugin:hyprdecor:key"] = value })` applies (only matters if D1 changes).
- The dispatcher string format on a Lua-config Hyprland (`killactive` versus a `hl.dsp...`
  expression), before C4.
- The Lua syntax for a custom-effect window rule such as `hyprdecor:theme`, before C5.
- Asymmetric reserved extents under `DECORATION_POSITION_ABSOLUTE`, in C2.

## Verified against Hyprland 0.56.2 (`/usr/bin/Hyprland`, headers in `/usr/include/hyprland`)

| Fact | Evidence |
|---|---|
| `addLuaFunction(handle, ns, name, fn)` is exported | `nm -D` shows `T addLuaFunction` |
| `removeLuaFunction` exported | `T removeLuaFunction` |
| `addConfigValueV2(handle, SP<IValue>)` exported | `T addConfigValueV2` |
| Lua namespace errors exist in the binary | `hl.plugin.{} already exists and is not a namespace table`, `hl.plugin: this function is not callable` |
| Plugin values must be named `plugin:<ns>:<key>` | binary string `name must start with plugin:` |
| `hl.plugin` has only `load` and a dynamic string index | `/usr/share/hypr/stubs/hl.meta.lua`, `HL.PluginNamespace` |
| `HL.ConfigOpt` has no `plugin` field | same stubs; only `general`, `decoration`, ... |
| Hyprland links both Lua 5.4 and 5.5 | `ldd /usr/bin/Hyprland` |
| Lua 5.5 headers are available and correct | `/usr/include/lua.h`, `LUA_VERSION_NUM 505`, `pkg-config lua5.5` gives 5.5.1 |

The Lua ABI is resolved: Hyprland links `liblua.so.5.5` directly, and `liblua5.4` only arrives
transitively through `libhyprtoolkit`. Plugin callbacks receive a 5.5 `lua_State*`, and the
unversioned headers plus `-llua` match it. No vendoring is needed.

**Rule:** never include `/usr/include/lua5.4/*` and never link `liblua5.4`. Use
`pkg-config lua5.5` only.

