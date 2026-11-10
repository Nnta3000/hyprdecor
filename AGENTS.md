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
| `ROADMAP.md` | everyone | status, decisions, milestones, commit plan, review findings | rules, semantics |
| `AGENTS.md` | agents, contributors | architecture rules, process, conventions, verified facts, build and test, layout | the plan, user docs |

---

## 1. What this is

A Hyprland **0.57** plugin that draws configurable window decorations (retro pixel-art
frames with title, icons and buttons), driven entirely from the Lua config
(`hl.plugin.hyprdecor.setup({...})`). Built against Lua 5.5.

Not in scope (belongs in quickshell): cables between windows, taskbars, panels, anything
that is not attached to a window's own frame. The plugin exposes state
(`hyprctl hyprdecor status`, and `json` once ROADMAP D9 lands) for quickshell to align with it.

**The plugin never starts a process or a shell.** (ROADMAP D2, D6.)

---

## 2. Architecture rules

1. **One config source.** `SConfig` in `config.hpp`, filled by `setup()`. No
   `addConfigValue` registrations, no null-pointer fallbacks, and **no second copy of a
   config value on `CPlugin`** (the mirrored `bar_*`/`decoration_*` fields are being
   deleted, ROADMAP R3). Defaults live in `config.hpp` only; changing a default is a
   behaviour change and gets its own commit.
2. **Layout is a pure function** in a header with no Hyprland includes (like
   `ninepatch.hpp`, `titlebox.hpp`, `layout.hpp`), with unit tests asserting exact
   rectangle lists.
3. **Compute once, consume everywhere.** One layout pass produces the resolved items.
   Drawing, hit-testing and status all read that list, and the result is cached until its
   inputs change. Do not recompute it per pointer event.
4. **Frame = one nine-patch.** Regions never re-implement it.
5. **One texture upload helper** (`uploadCairoSurface`, direct `glTexImage2D` + swizzle).
   Never `CGLTexture::update(DRM_FORMAT_ARGB8888, ...)`: it leaves the texture black.
6. **Rebuild a texture whenever its size or its inputs differ**, not only on focus changes.
   The cache key is everything the pixels depend on (path, filter, size, colour, font).
7. **Ownership.** Hyprland owns decorators. The plugin tracks windows by `PHLWINDOWREF`,
   registers every listener it needs (close included), and removes its decorators in
   `PLUGIN_EXIT`. A `PHLWINDOWREF` is locked **once** per function and null-checked
   before use; never lock twice and assume both succeed.
8. **The Lua boundary is exception-based.** Parsing code throws (`raise()`); only
   `lSetup()` catches, copies the message into a plain buffer, lets every C++ object go out
   of scope, and then calls `lua_error`. Never call `lua_error` while a C++ object with a
   destructor is live. Lua 5.5 only (`pkg-config lua5.5`; never include
   `/usr/include/lua5.4/*` or link `liblua5.4`). Defer layout/damage work to the `tick`
   listener; never apply re-entrantly inside a Lua callback.
9. Do not edit `PLUGIN_API_VERSION()`. No `#define private public` in headers (the one in a
   `.cpp` is tracked for removal, ROADMAP R7). Zero compiler warnings (`-Wall -Wextra`).
10. **Core and platform are separate.** Code under `core/` and `config/` must not include any
    Hyprland header. Everything that touches Hyprland lives in `platform/` and
    `decoration/`, and reaches version-sensitive API only through `platform/compat.hpp`.
    A Hyprland upgrade should change `platform/` and `decoration/` and nothing else.
11. **Config structs are composable.** One "unset" convention only: `std::optional<T>`
    (not `-1`, not `0`). Sections are sub-structs (`SFrameConfig`, `STitleConfig`, ...) so a
    per-window theme is an optional overlay of the same types, never a hand-mirrored copy.
12. **Strict config.** A key that is present with the wrong type or an out-of-range value is a
    `setup()` error naming the exact path. Nothing is silently ignored or defaulted, and no
    config double is cast to an integer without a range check. (ROADMAP D8.)
13. **A render pass leaves GL state as it found it on every return path** (scissor, stencil,
    blend). Early returns are where this breaks; check each one.
14. **Colour integers are `0xAARRGGBB` everywhere** (`SConfig`, `CHyprColor`). The only place
    that sees `rgba(rrggbbaa)` text is `ColorParse::parse`.
15. **Filesystem calls on any path reachable from rendering or input use the `error_code`
    overloads** (`pathExists()` in `util.hpp`). `std::filesystem::exists(path)` can throw.
16. **No blocking I/O in the render path.** Disk lookups (icons, textures) happen on config
    apply or are cached by key.

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
5. **Never call code "dead" or "not called" without `grep` for callers**, and count the hits:
   one hit is the declaration. `renderBarButtons` was called "dead" and was not.
   Also trace the *config* path: a feature with no caller may still be reachable from
   Lua (`defaults.icon` is).
6. **Do not filter logs while diagnosing.** Capture the whole log to a file
   (`2>&1 | tee ~/full.log`) and grep the file afterwards.
7. **Do not change a default or a fallback silently.** Making `col_text` opaque exposed a
   latent upload bug.
8. **If wrong, say so briefly, and revert the speculative change.** No narrative.
9. Commit messages state what was **tested and how**, and say plainly what was **not**
   tested (for example "not compiled against 0.57").
10. Plan before building anything touching layout; ask when a semantic is not decided in
    `docs/SPEC.md` rather than choosing silently.
11. **Every pure change gets a test** in `tests/`, run with `ctest`. A bug fix starts with
    a failing test when the code is pure.
12. **Moving files is its own commit**: no content change, only paths and CMake. Do not
    reformat code in the same commit as any other change.

---

## 4. Verified facts, known bugs, unknowns

Facts marked 0.56.2 were probed on the older build; re-verify on 0.57 (ROADMAP,
"To verify on 0.57") before relying on them.

**Verified (probes and screenshots)**
- The decoration positioner honours reserved extents under `DECORATION_POSITION_ABSOLUTE`,
  including asymmetric extents (0.56.2).
- `hyprctl clients -j` in the broken state shows `"fullscreen": 1` (maximised) on the new
  window. It reproduces **without the plugin**, so it is not caused by it.
- `CGLTexture::update(DRM_FORMAT_ARGB8888, ...)` leaves textures black; the direct upload
  path works.
- The frame draws under the window, so the reserved top must cover the art's title band.
- `hyprctl dispatch` on a Lua-config Hyprland takes **Lua**, not a command name; dispatchers
  are CTypes under `hl.dsp.<ns>.<verb>()` (0.56.2).
- **Pinning requires a floating window.** Pinning a tiled window emits a warning, so `pin`
  floats first.
- **Fullscreen on a pinned window is a silent no-op.** Never treat "no error" as proof an
  action took effect; read the state back.

**Resolved**
- Stale stretched frame: textures are rebuilt when their size differs from the target box.
- Unit mixing in `bandBox()`: the helper no longer exists; layout is one pure pass.

**Known bugs**
- See `ROADMAP.md`, "Review findings" (F1 to F19) for the current list and owners.
- Maximised-on-open bug: **unsolved, separate thread, not the plugin.** Try
  `kitty -o start_as=normal`, another terminal (foot), `WAYLAND_DEBUG=client` for
  `set_maximized`, and a no-plugin run on a real TTY. Do not mix it into layout work.

**Unknown (test, do not assume)**
- Changing a region size at runtime.
- Fractional monitor scales with pixel art (ROADMAP D7 defines the intended behaviour).
- The Lua syntax for custom-effect window rules (`hyprdecor:theme`).
- `CHyprColor(uint64_t)` byte order: assumed ARGB (see ROADMAP F2).

---

## 5. Build, test, run

```bash
cd /home/agent/Documents/hyprdecor
make debug && ctest --test-dir build --output-on-failure   # build + all pure-logic tests
Hyprland -c /home/agent/Documents/hyprtest/test.lua 2>&1 | tee ~/full.log
hyprctl hyprdecor status                       # resolved layout
hyprctl clients -j > ~/clients.json            # run inside the nested session
```

Tests are standalone executables (`ninepatch_test`, `layout_test`, `config_value_test`,
`colorparse_test`). They link only cairo or lua, never Hyprland, which is how rule 10 is
enforced: **a file under `core/` that includes a Hyprland header will fail to build in
the test targets.** Run the parser tests under AddressSanitizer
(`-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined`).

`make all` is what hyprpm runs, so it must build only the plugin. Tests are opt-in for
hyprpm users (ROADMAP R9).

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
- **Core**: Hyprland-free code (`core/`, `config/`). **Platform**: Hyprland-facing code.

---

## 7. Code conventions

**Documentation is docstrings, not scattered comments.**

- Every function, class, struct and non-obvious member in a header has a `/** ... */`
  docstring: one summary sentence in the imperative or descriptive mood, then only what a
  caller needs: units (`logical px`), ownership, what returns `nullopt` or `nullptr`, what
  throws, which thread or phase it may run in.
- A docstring describes what is true **now**. History ("this used to", "was declared but
  never assigned", "C3 replaces C1") belongs in the commit message and `ROADMAP.md`, never
  in the code.
- Comments inside a function body say **why** something non-obvious is done (a Hyprland
  quirk, an ordering constraint), in one or two lines. Do not narrate what the next line does.
- Do not leave commented-out code. Delete it; git remembers.
- Module headers (`layout.hpp`, `titlebox.hpp`, ...) keep one short top-of-file paragraph
  saying what the module is and what it must not depend on.

**Naming and structure**

- One concept, one name. If two types differ by a letter (`SElementTex` / `SElementTexture`),
  rename one.
- A function longer than about 80 lines is split; `renderPass` (about 500) is the target.
  Split by responsibility (frame, title, items), not by line count.
- A file longer than about 800 lines is split by responsibility.
- Prefer a small helper over a third copy of any block. Two copies are tolerated; three are not.
- RAII for every resource: `cairo_surface_t*` and `cairo_t*` live in a `unique_ptr` with a
  deleter, never as a raw owned pointer in a struct that can be copied.
- Logging: `Log::logger->log(level, "[HYPRDECOR] ...")`. Never log per frame or per pointer
  event at a level above DEBUG, and never log inside a loop over candidates.
- Errors the user can cause (config) are `setup()` errors with a path. Errors the compositor
  can cause (a window vanished, a texture failed) are logged once and the frame continues.
  Do not add defensive code for states that cannot happen.

---

## 8. Repository layout

Target layout (ROADMAP R7). Migrate with a move-only commit (process rule 12).

```
src/
  main.cpp                    PLUGIN_INIT / PLUGIN_EXIT only
  core/                       pure; no Hyprland includes
    ninepatch.hpp
    layout.hpp
    titlebox.hpp
    itemstate.hpp
    colorparse.hpp
  config/                     pure; no Hyprland includes
    config.hpp                SConfig and sub-structs
    luaRead.hpp               typed readers (throw on wrong type or range)
    luaParse.{hpp,cpp}        parseSetup(lua_State*, ctx) -> SConfig
  platform/                   the only place that names version-sensitive Hyprland API
    compat.hpp
    glTexture.{hpp,cpp}       uploadCairoSurface and texture loading
    luaSetup.cpp              lSetup(): the exception-to-lua_error boundary
  plugin/
    plugin.{hpp,cpp}          CPlugin: state, update(), frame resolution
    status.cpp                statusString(), json
  decoration/
    hyprWindowDecorator.hpp
    decorator.cpp             lifecycle, positioning, damage
    decoratorInput.cpp        pointer, touch, resize band, cursor
    decoratorActions.cpp      named actions (the only file with hl.dsp.* strings)
    decoratorRender.cpp       renderPass and its helpers
    decoratorRules.cpp        window rules and plugin-side theme rules
    renderPassElement.{hpp,cpp}
tests/                        one executable per pure module, run by ctest
docs/                         configuration, build, ninepatch, development, SPEC
assets/                       sample frame sets (licences checked; see R9)
```

Rules for the layout:

- `core/` and `config/` never include `<hyprland/...>`. Tests compile them without Hyprland.
- Version-sensitive Hyprland calls (window list, monitor list, fullscreen mode, pinned state,
  rule effects, Lua `eval`, render-pass removal) are wrapped once in `platform/compat.hpp`.
  A new Hyprland release should be absorbed by editing that file.
- CMake lists sources **explicitly** per directory, not `file(GLOB_RECURSE)`, so a moved or
  stray file fails the build instead of being silently included or dropped.
- Generated files (`build/`, `cmake_install.cmake`, `Makefile` from CMake) are never committed
  and never uploaded for review. `.gitignore` must be named `.gitignore`.
- Version number has one source (`CMakeLists.txt`); `hyprpm.toml` is generated or checked
  against it. Git commit and build date are stamped at **build** time.
- `hyprpm.toml` carries `commit_pins` for every supported Hyprland commit.

---

## 9. Review checklist (run before every commit)

- [ ] Builds with zero warnings; `ctest` green.
- [ ] No new `#include <hyprland/...>` under `core/` or `config/`.
- [ ] Every `return` in a render function leaves GL state as found.
- [ ] Every `lock()` of a weak ref is null-checked.
- [ ] No `std::filesystem::exists(path)` without an `error_code`.
- [ ] No new config field without: a default in `config.hpp`, a strict reader, a test, a line in
      `docs/configuration.md`.
- [ ] No new comment that explains history; docstrings on every new declaration.
- [ ] Commit message says what was tested and what was not.
