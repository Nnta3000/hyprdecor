# AGENTS.md: hyprdecor

How to work in this repository. Read this file, then `docs/SPEC.md` (what the finished
product must do), then do **only** the commit the user names from `ROADMAP.md`.

## Where things live

Each fact lives in exactly one file; the others link to it.

| File | For | Holds | Does not hold |
|---|---|---|---|
| `README.md` | users | what it is, build, install, how to configure what works today | internals, plans, dev paths |
| `docs/SPEC.md` | contributors, agents | the target config and the normative semantics | status of work |
| `docs/ninepatch.md` | asset authors | marker format, naming, verification | config options |
| `ROADMAP.md` | everyone | status, decisions, milestones, commit plan | rules, semantics |
| `AGENTS.md` | agents, contributors | architecture rules, process, verified facts, build and test, glossary | the plan, user docs |

---

## 1. What this is

A Hyprland 0.56 plugin that draws configurable window decorations (retro pixel-art
frames with title, icons and buttons), driven entirely from the Lua config
(`hl.plugin.hyprdecor.setup({...})`). Built against Lua 5.5.

Not in scope (belongs in quickshell): cables between windows, taskbars, panels,
anything that is not attached to a window's own frame. The plugin should instead
expose enough state (`hyprctl hyprdecor status`, JSON) for quickshell to align with it.

---

## 2. Architecture rules

1. **One config source.** `SConfig` in `config.hpp`, filled by `setup()`. No
   `addConfigValue` registrations and no null-pointer fallbacks. Defaults live in
   `config.hpp` only; changing a default is a behaviour change and gets its own commit.
2. **Layout is a pure function** in a header with no Hyprland includes (like
   `ninepatch.hpp` and `titlebox.hpp`), with unit tests asserting exact rectangle lists.
3. **Compute once, consume everywhere.** One layout pass produces the resolved items.
   Drawing, hit-testing and status all read that list. Calling `bandBox()`/`elementBox()`
   style helpers from several places is how hover drifted from drawing twice.
4. **Frame = one nine-patch.** Regions never re-implement it.
5. **One texture upload helper** (`uploadCairoSurface`, direct `glTexImage2D` + swizzle).
   Never `CGLTexture::update(DRM_FORMAT_ARGB8888, ...)`: it leaves the texture black.
6. **Rebuild a texture whenever its size differs from the box**, not only on focus
   changes (see the stale-stretch bug in section 4).
7. **Ownership.** Hyprland owns decorators. The plugin tracks windows by
   `PHLWINDOWREF`, registers every listener it needs (close included), and removes
   its decorators in `PLUGIN_EXIT`.
8. **Lua safety.** Lua 5.5 only (`pkg-config lua5.5`; never include
   `/usr/include/lua5.4/*` or link `liblua5.4`). Never call `lua_error` while C++ objects with
   destructors are live. Defer layout/damage work to the `tick` listener; never apply
   re-entrantly inside a Lua callback.
9. Do not edit `PLUGIN_API_VERSION()`. No `#define private public` in headers. Zero
   compiler warnings (`-Wall -Wextra`).

---

## 3. How to work in this repo (process rules)

These exist because of what went wrong before. They are not optional.

1. **One commit, then stop.** The user builds and tests between commits. Never batch.
   Ten untested commits once made a regression impossible to locate.
2. **Never mix a refactor with a feature.** A refactor must leave the rendering
   pixel-identical (baseline screenshot before, comparison after).
3. **Evidence before code.** For any bug: reproduce, write down a prediction ("if X is
   the cause the log will show Y"), instrument, then change one thing.
4. **Run the control first.** Check whether the bug happens without the plugin loaded.
   (The maximised-window bug did, and a lot of work went into the wrong place.)
5. **Never call code "dead" or "not called" without `grep` for callers.**
   `renderBarButtons` was called as dead and was not.
6. **Do not filter logs while diagnosing.** Capture the whole log to a file
   (`2>&1 | tee ~/full.log`) and grep the file afterwards; a narrow live grep hid the
   Hyprland lines that mattered.
7. **Do not change a default or a fallback silently.** Making `col_text` opaque exposed
   a latent upload bug. A changed default changes what is drawn.
8. **If wrong, say so briefly, and revert the speculative change.** No narrative.
9. Commit messages state what was **tested and how**.
10. Plan before building anything touching layout; ask when a semantic is not decided
    in `docs/SPEC.md` rather than choosing silently.

---

## 4. Verified facts, known bugs, unknowns

**Verified (from probes and screenshots)**
- The decoration positioner honours reserved extents under `DECORATION_POSITION_ABSOLUTE`
  (`reserved L32 T32 R32 B32` and later `L8...` were reported and window sizes agreed).
- `hyprctl clients -j` in the broken state shows `"fullscreen": 1` (maximised) on the
  new window. It also reproduces **without the plugin**, so it is not caused by it.
- `CGLTexture::update(DRM_FORMAT_ARGB8888, ...)` leaves textures black; the direct
  upload path works. Black patches appeared when title text became visible.
- The title was half hidden because the reserved top (8) was smaller than the art's
  title band (about 30): the frame draws under the window.

**Verified for dispatchers and window state (C4 probe, 0.56.2)**
- `hyprctl dispatch` on a Lua-config Hyprland takes **Lua**, not a command name:
  `hl.dispatch(closewindow)` errors with "expected a dispatcher (e.g.
  hl.dsp.window.close())". Dispatchers are CTypes under `hl.dsp.<ns>.<verb>()`.
- **Pinning requires a floating window.** Pinning a tiled window makes Hyprland emit an
  error/warning, so a `pin` action must float first or refuse.
- **Fullscreen on a pinned window is a silent no-op**: no error, no state change. Never
  treat "no error" as proof an action took effect; read the state back.

**Known bugs / verify first**
- Stale stretched frame: the frame texture is rebuilt on focus change, not on size
  change, so unfocused windows that get resized show a stretched old texture. Rebuild
  when the texture size differs from the target box (title and buttons textures too).
  Check whether this is already fixed before working on it.
- Unit mixing in `bandBox()`: scaled offsets + raw nine-patch padding + logical padding.
  Possible double-counting of the nine-patch padding. Remove by resolving frame metrics
  once, in logical px.
- Maximised-on-open bug: **unsolved, separate thread, not the plugin.** Try
  `kitty -o start_as=normal`, another terminal (foot), `WAYLAND_DEBUG=client` for
  `set_maximized`, and a no-plugin run on a real TTY. Do not mix it into layout work.

**Unknown (test, do not assume)**
- Asymmetric reserved extents (for example left 24, top 14, bottom 38) under `ABSOLUTE`.
- Changing a region size at runtime.
- Fractional monitor scales with pixel art (round or document).
- The Lua syntax for custom-effect window rules (`hyprdecor:theme`).

---

## 5. Build, test, run

```bash
cd /home/agent/Documents/hyprdecor
make debug && ./build/ninepatch_test          # build + pure-logic tests
Hyprland -c /home/agent/Documents/hyprtest/test.lua 2>&1 | tee ~/full.log
hyprctl hyprdecor status                       # resolved layout, once C1 lands
hyprctl clients -j > ~/clients.json            # run inside the nested session
```

Nested Hyprland is the only supported test setup on the user's machine (binds use ALT).
Keep `test.lua` as the throwaway config; never point tests at the user's real config.

---

## 6. Glossary

- **Decoration box**: window rect plus reserved extents; what the frame is stretched over.
- **Reserved extents**: per-side space Hyprland keeps free around the window.
- **Region**: one side's slice of the decoration box, holding items.
- **Item**: an image, button, title, text or spacer placed in a region.
- **Art px / logical px / device px**: source image pixels / compositor units / physical
  screen pixels. `frame.scale` converts art to logical; monitor scale converts to device.
- **Content padding**: where the window may sit inside the nine-patch (bottom and right
  marker bars). Not the same as the stretch zone.
