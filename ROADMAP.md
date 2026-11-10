# Roadmap

This file is the plan: what is built, what is next, and the decisions behind it.
How to work is in `AGENTS.md`. What the finished config must do is in `docs/SPEC.md`.

Last reviewed: 2026-11-10 (full code review; see "Review findings"). Target: **Hyprland 0.57**
with the **Lua** config manager, Lua 5.5.

## Where we are

Present in the code:

- [x] `hl.plugin.hyprdecor.setup({...})` through `addLuaFunction`, validated, applied atomically.
- [x] One config struct (`SConfig`) filled by `setup()`.
- [x] Lua 5.5 ABI resolved (`pkg-config lua5.5` only).
- [x] Nine-patch frame, title (fill and box modes), image/button/spacer/text items in four
      regions, flow layout, state textures (`_unfocused`, `_hovered`, `_pressed`, `_on`),
      `visible_when`, named actions, resize band, `hyprctl hyprdecor status`.
- [x] Frame-only per-window themes (`themes` + plugin-side `rules`).
- [x] Stale stretched frame fixed: the frame, title and button textures are rebuilt when
      their size differs from the target box (`renderPass`, `m_size != ...WANT`).
- [x] `bandBox()` / `elementBox()` are gone; layout is one pure pass (`layout.hpp`).
- [ ] Full per-window themes (title, items).
- [ ] Release readiness: see milestone R.
- [x] R0-R4: review patches, shader removal, legacy buttons removed, one config source, pure strict parser.

### Review patches 01 to 04 (applied)

Applied in order on 2026-11-10. The plugin builds against Hyprland 0.57, `ctest` is green and
the nested session confirmed the runtime behaviour.

| Patch | Contains | Test |
|---|---|---|
| 01-dead-code | `m_seExtents`, `m_iLastHeight`, `m_vLastTitleSize`, `isInResizeBand`, `g_config`, three unused `CPlugin` fields, duplicate `assignedBoxGlobal()` call, orphan comments | Builds with zero warnings; screenshot identical to baseline. |
| 02-robustness | scissor left enabled on an early return; `renderPass`/`updateRules` null checks; per-window instead of shared log guard; `exists()` that cannot throw; unparsable rule colours are logged and ignored | Open two windows: `draw:` debug line appears once per size change, not per frame. `hyprctl dispatch` a rule with a bad colour: one warning, colour unchanged. |
| 03-lua-error-safety | `raise()` throws, `lSetup()` raises the Lua error after all C++ objects are destroyed; failed init leaves no `gPlugin` | Config with a typo'd field: error text unchanged, compositor stays up. Run under ASan/valgrind if available. |
| 04-colour-parsing-cmake | `colorparse.hpp` + test; `rgba(rrggbbaa)` now yields 0xAARRGGBB; strict parsing; `ctest`; tools off by default | `ctest --test-dir build`. Rule `hyprdecor:bar_color = rgba(ff000080)` shows semi-transparent red (**verify**: this assumes `CHyprColor(uint64_t)` is ARGB, which `SConfig::barColor = 0x88333333` implies). |

## Decisions

| # | Decision | Why |
|---|---|---|
| D1 | No flat `plugin:hyprdecor:*` values. Everything goes through `setup()`. | One source of truth. Flat values were never committed under the Lua manager. |
| D2 | Actions are named (`close`, `maximize`, ...). **The plugin never spawns processes or shells.** | Keeps execution out of the model. D6 removes the one exception. |
| D3 | Per-window variation uses plugin-side `rules` (app id) and named `themes`. The effects `hyprdecor:no_bar`, `bar_color`, `title_color`, `theme` stay registered for configs that can reach them. | The 0.57 Lua window-rule binding has a fixed effect list. Revisit if it gains custom effects. |
| D4 | Regions and items with flow layout. | Vertical buttons on any side; removes "measured from what?". |
| D5 | State suffixes: `_unfocused`, `_hovered`, `_pressed`, `_on`. | One vocabulary. |
| D6 | **Remove the shader path** (`retro_filter`, `retro_shader`, `hyprshade`, `tools/palette_gen`, `tools/dither_test`). | Decided 2026-11-10. It was the only process-spawning action and was personal to one setup. |
| D7 | **Pixel snapping happens at draw time, in device pixels.** Layout stays in unrounded logical px. Each item's device rectangle is `round(edge * monitorScale)` on its *edges* (not on x and w separately). | `Layout::snap()` is never called, and as written it rounds back to whole logical px, which defeats device snapping at 1.25/1.5. Hit-testing may differ from drawing by under half a device pixel. Replaces the SPEC claim "layout outputs rectangles snapped to device pixels". |
| D8 | **Strict config readers.** A key that is present with the wrong type, or outside its valid range, is a `setup()` error naming the path. Nothing falls back silently. | SPEC acceptance 11. Today `boolField`, `numberField` and `stringField` return the fallback on a wrong type. |
| D9 | `hyprctl hyprdecor status` stays human text; add `hyprctl hyprdecor json` that prints **only** JSON. | quickshell needs parseable output; the current status mixes text and JSON. |
| D10 | Source layout splits into `core/` (pure, no Hyprland), `config/`, `decoration/`, `platform/` (see `AGENTS.md` section 8). | Upgrades touch only `platform/` and `decoration/`; `core/` stays testable. |
| D11 | **Licence: open.** Resolve before publishing (see R9). | The origin is CapsAdmin's hyprdecor; its licence governs. |
| D12 | **Open: `defaults.icon`** (app icon). It is reachable from Lua but undocumented, placed by hardcoded arithmetic outside the layout engine, and searches the disk synchronously in the render path. Proposal: delete it, and re-add later as an `icon` item type if wanted. | Needs the maintainer's call. |
| D13 | **Open: `minimize` action.** It hardcodes the `special:min-<workspace>` convention. Proposal: keep it, document the convention in the README, and make it opt-in. | Needs the maintainer's call. |
| D14 | `title.placement` (`left`/`right`) is legacy vertical text. Keep until C5 provides `rotate`, then delete. | Avoids a visible regression. |

## Milestones

### R: release readiness (do before tagging a public version)

One commit at a time; the user tests between commits (`AGENTS.md` section 3). Refactors must
render pixel-identically against the baseline screenshot.

| # | Commit | Done when |
|---|---|---|
| R0 | Apply and test patches 01 to 04 above | **Done.** `ctest` green, nested session confirmed. |
| R1 | Remove the shader path (D6) | **Done.** `grep -ri 'hyprshade\|retro\|palette\|dither' src CMakeLists.txt docs README.md` returns nothing; `tools/` deleted; CMake tools block deleted. Shader tooling moved to `orange-config/dithering/shaders-tools`. |
| R2 | Delete the legacy button path | **Done.** `onNewButtonTextured`, `SHyprButton`, `m_vButtons`, `m_pButtonsTex`, `m_iButtonPressedIdx`, `m_iButtonHoverState`, `m_bButtonsDirty`, `indexToButton`, `renderBarButtons`, `renderBarButtonsText`, `getVisibleButtonCount`, `damageOnButtonHover`, the `exec_cmd` dispatch, `onPreConfigReload`, `Compat::splitVars`, and the button-related `bar_button_padding`/`buttonsAlignment`/`onDoubleClick` config are gone. Item elements remain. |
| R3 | One config source | **Done.** The mirrored `CPlugin` fields are deleted; every reader uses `cfg`. `defaultScale` defaults to 1 and the guards are gone. Defaults exist only in `config.hpp`. |
| R4 | Pure, strict Lua parser | **Done.** `parseSetup(lua_State*, const SParseContext&) -> SConfig` lives in `luaParse.cpp` with injected texture resolution and no `gPlugin` dependency. Strict type readers and `intField`/`uintField` ranges throw; `lua_parse_test` runs against Lua 5.5 (also under ASan). |
| R5 | Remove duplication | `SConfig::scale()`; `drawNinePatch()` (elements and title background); `loadNinePatchTexture()` (five copies); one frame-suffix list; `ruleEffect(idx)`; `Sides::over()` for theme resolution; `dispatchLua` takes a `sync` flag; title-background cache key includes `linearFiltering`. |
| R6 | Layout and status | `resolveLayout()` cached per (decoration box, window-state snapshot, `cfg.applies`). `layoutJson()` and `liveDecorators()` are `const` without `const_cast`. Pixel snapping per D7, with tests. `hyprctl hyprdecor json` per D9. |
| R7 | File organisation | Folder layout from `AGENTS.md` section 8, as a move-only commit; `hyprWindowDecorator.cpp` split into input, actions, render, rules; CMake lists sources explicitly (no `GLOB_RECURSE`). |
| R8 | Docstring pass | Every declaration documented per `AGENTS.md` section 7; changelog-style comments ("this used to", "was declared but never assigned") deleted. |
| R9 | Release hygiene | LICENCE settled (D11); README states a single Hyprland version and matches `AGENTS.md`; `hyprpm.toml` has `[repo]` + `commit_pins` for each supported Hyprland commit, `make all` does not build tests; version has one source, bumped by `scripts/release.sh` / `make release-patch\|release-minor\|release-major`, which refuses a dirty tree, checks CMake and hyprpm agree, runs `ctest`, commits and creates an annotated tag; git commit/date stamped by a build-time custom target; CI builds in a container against the Hyprland headers, runs `ctest`, and fails on warnings; asset licences checked for `assets/xp` and `assets/2000`. |
| R10 | D12 and D13 resolved | Each either deleted or documented in README and SPEC. |

### M1: stabilise (carried over)
- [ ] Content markers on the shipped frame art (see `docs/ninepatch.md`).
- [ ] Separate thread, not the plugin: the maximised-on-open bug. It reproduces without the plugin.
- [ ] Fractional monitor scales with pixel art: verify after R6 on a 1.25 and 1.5 monitor.

### M2: layout engine (C-series, carried over)

| # | Commit | Status |
|---|---|---|
| C0 to C4 | Baseline, top band, four regions, unified items, `visible_when` and actions | Done. |
| C5a | Frame-only per-window themes | Done (0.3.0). |
| C5 | Text templates, `rotate`, drag and resize grips, full themes | Open. Full themes should be `SConfig` sub-struct overlays (`AGENTS.md` rule 11), not another hand-mirrored `STheme`. |
| C6 | Polish: docs, example configs, deprecation notice, SPEC section 2 loads as written | Open. |

Do R3 and R4 **before** C5: full themes multiply the config surface, and the current flat
`SConfig` (about 70 fields, three different "unset" conventions) would triple the duplication.

### M3: later
- [ ] Redraw-on-animation, no polling.
- [ ] Lua action callbacks (re-entrancy design first).
- [ ] External renderer transport (separate protocol).
- [ ] Per-item hover animation, tooltips, group tabs, urgent flash.

## Non-goals

- An animation scripting language.
- Any external-process rendering protocol in the core plugin.
- Quickshell as a dependency; the plugin only exposes state for it.
- Taskbars, panels and cables between windows (quickshell).
- **Starting any process.** (D2, D6.)

## SPEC.md corrections (docs commit D0, no code)

`docs/SPEC.md` contradicts itself and the code in these places; fix in one docs-only commit:

1. Section 1 scenario 6 lists `drag`, `resize` and "any Hyprland dispatcher"; section 3 says
   `drag`/`resize` were dropped and a `dispatch = "name args"` string cannot work. Align both.
2. Section 3 says "the plugin never spawns processes" and, a few lines earlier, that
   `retro_filter` "is the only action that starts a process". Delete `retro_filter` (D6).
3. Section 3 cites the internal `m_dispatchers[...]` map as verified; the code uses Lua
   `eval` of `hl.dsp.window.*`. Replace with the Lua verbs and re-verify on 0.57.
4. "Units": replace "snapped to device pixels" with D7.
5. The status example shows string ids, `visible`, `state` and `frame.texture`; the code prints
   numeric ids and none of those. Decide the schema (D9) and make code and SPEC match.
6. Every "Hyprland 0.56" becomes 0.57, or is marked as verified on 0.56.2 only.
7. `hyprdecor:no_bar` and friends are described as usable; per D3 they are not reachable from
   the Lua binding today.

## Review findings (2026-11-10)

Silent or dangerous problems found by reading all of `src/`. "Fixed" means a patch above;
"R#" means the commit that owns it.

| # | Problem | Severity | Status |
|---|---|---|---|
| F1 | `lua_error` longjmp over live C++ objects in `lSetup` (violated AGENTS rule 8) | High | Fixed, patch 03 |
| F2 | `configStringToInt` returned `rgba()` as RRGGBBAA while everything else is AARRGGBB; failures became colour 0 (transparent black); `stoll("12abc")` returned 12 | High | Fixed, patches 02 + 04 |
| F3 | Scissor left enabled on the rounded early return in `renderPass` | Medium | Fixed, patch 02 |
| F4 | `std::filesystem::exists(path)` throws on permission errors, called from the render path | Medium | Fixed, patch 02 |
| F5 | `renderPass`/`updateRules` dereference a window or rule applicator without a null check | Medium | Fixed, patch 02 |
| F6 | A `static CBox` log guard shared by all windows logged every frame with two windows | Low | Fixed, patch 02 |
| F7 | Wrong-typed config values are silently ignored (`title.size = "big"`, `border.repeat = "yes"`, `font = 12`); numbers are accepted as booleans | High | Fixed, R4 (D8) |
| F8 | Config doubles cast to `int`/`uint64_t` with no range check (`title.size = 1e9`, negative `color`) is undefined behaviour or huge allocations | Medium | Fixed, R4 |
| F9 | `Layout::snap()` is never called, and rounds to whole logical px (wrong at fractional scale) | Medium | R6 (D7) |
| F10 | Title background texture cached by path only; `m_szLoadedTitleBg` never reset; a `linear_filtering` change is ignored | Low | R5 |
| F11 | Defaults in `plugin.hpp` differ from `SConfig` (`appicon`, `outline_width`, `outline_always`), live until the first `update()` | Low | R3 |
| F12 | `loadAppIcon` searches about 200 paths synchronously in the render path | Low | R10 (D12) |
| F13 | `resolveLayout()` recomputed on every pointer event and frame | Low (CPU) | R6 |
| F14 | `liveDecorators() const` and `layoutJson() const` mutate or cast away const | Low | R6 |
| F15 | CMake stamps git commit and date at configure time, so the stamp goes stale after a rebuild | Low | Fixed: build-time generated version header (R9) |
| F16 | `make all` (hyprpm) builds tests and tools | Low | Tools off by default in patch 04; tests R9 |
| F17 | `status` output is not valid JSON (text followed by JSON) | Low | R6 (D9) |
| F18 | `resolveTexturePath` logs every candidate at DEBUG and keeps hidden state in `m_szLastResolveError` | Low | R4 |
| F19 | `PLUGIN_EXIT` removes pass elements by the string `"CRenderPassElement"` | Verify | Check it cannot match a Hyprland class on 0.57 |

## To verify on 0.57 (verification debt)

Everything in `AGENTS.md` section 4 marked "0.56.2" was probed on the older build.

- Each action (`close`, `maximize`, `fullscreen`, `pin`, `float`, `minimize`) with one real click.
- `hl.dsp.window.*` with a `window = hl.get_window("address:0x...")` selector still evaluates.
- The Lua syntax for a custom-effect window rule (`hyprdecor:theme`); the binding used a fixed list.
- `CHyprColor(uint64_t)` byte order (F2): one visible test.
- `m_ruleApplicator->m_otherProps.props`, `Fullscreen::controller()`, `State::monitorState()` and
  the `#define private public` for `InputManager::m_exclusiveLSes` all still build and behave.
- A clean `hyprpm add` + `hyprpm update` from the public repository on a stock install.

## Verified against Hyprland 0.56.2 (kept for reference)

| Fact | Evidence |
|---|---|
| `addLuaFunction(handle, ns, name, fn)` and `removeLuaFunction` are exported | `nm -D` |
| Plugin values must be named `plugin:<ns>:<key>`; `hl.plugin` has no config namespace | binary strings, `/usr/share/hypr/stubs/hl.meta.lua` |
| Hyprland links `liblua.so.5.5` directly; plugin callbacks receive a 5.5 `lua_State*` | `ldd /usr/bin/Hyprland` |
| `hyprctl dispatch` evaluates Lua and takes a CType, not a command name | C4 probe |
| Asymmetric reserved extents are honoured under `DECORATION_POSITION_ABSOLUTE` | C2 probe |

**Rule:** never include `/usr/include/lua5.4/*` and never link `liblua5.4`. Use
`pkg-config lua5.5` only.
