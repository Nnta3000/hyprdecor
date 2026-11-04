# hyprdecor: target specification

What the finished plugin must do, and the exact rules that make every behaviour defined.

**Status: target.** Not everything here exists yet; `ROADMAP.md` says what does. This file
is normative: if code and this file disagree, fix one of them in a deliberate commit.
How to work on it is in `AGENTS.md`; asset authoring is in `docs/ninepatch.md`.

## 1. Acceptance scenarios

When the project is finished, a user can do all of this with config alone, no code:

1. Draw a frame from one nine-patch png, with separate `_active` / `_inactive` art.
2. Scale pixel art by an integer (2x, 3x) with nearest-neighbour filtering, crisp at any
   window size.
3. Put items on **any of the four sides**: a title row on top, a **vertical column of
   buttons on the left or right**, a status strip **underneath**.
4. Items are laid out by **flow** (start / centre / end, gaps, fillers). The user never
   computes coordinates for normal layouts.
5. The title is just an item: it fills leftover space, ellipsises, has a background,
   a font, a colour, and can be left/centre/right aligned.
6. Buttons have images per state (unfocused, hovered, pressed, toggled-on) and an
   action: close, maximise, fullscreen, pin, float, drag, resize, or any Hyprland dispatcher.
7. Any item can show window data as text: `{title}`, `{class}`, `{workspace}`; text
   can be rotated for vertical regions.
8. An item can appear only in certain window states (`visible_when`), without moving
   other items.
9. For the rare case, an item can be placed explicitly relative to the frame, a
   region or another item (`at`).
10. Different windows can use different themes (including "no decoration") through
    Hyprland window rules.
11. A mistake in the config (typo, missing file, bad value) fails **loudly at config
    time** with the exact path of the problem, never with a silently undecorated window.
12. `hyprctl hyprdecor status` prints the resolved layout as JSON, so a human or
    quickshell can check exactly where everything is.
13. Reloading the config re-applies everything; a failing reload keeps the last good state.
14. The existing `test.lua` config (single `defaults.border/title/elements`) keeps
    working throughout the migration.

## 2. The target config

This is what a finished `config.lua` looks like. If an implementation cannot express
this file, it is not finished. (Key spelling follows Hyprland: `color`, not `colour`.)

```lua
hl.plugin.load("/path/to/hyprdecor.so")

hl.plugin.hyprdecor.setup({
  defaults = {

    -- One nine-patch, drawn over the whole decoration box. Never split per side.
    -- `defaults.scale` (below) applies to EVERYTHING - frame and elements alike - and
    -- any specific `scale` overrides it. frame.scale scales the frame ART only.
    frame = {
      texture = "/assets/orange/frame",  -- resolves frame_active.png / _inactive / plain
      scale   = 2,                       -- scales the ART only
      filter  = "nearest",               -- "nearest" | "linear"
    },

    -- TOP: a row. Flows left to right.
    top = {
      size    = "art",                   -- reserved space = frame art's content padding
      gap     = 4,
      padding = { left = 8, right = 8 }, -- between region edge and items
      drag    = true,                    -- dragging the empty band moves the window
      items = {
        { type = "button", src = "/assets/orange/eye.png" },   -- hover/pressed only
        { type = "title",  fill = true,
          font = "BlueFont", size = 13, color = 0xFFFFFFFF,
          align = "left", ellipsize = true, text_offset = { x = 9, y = 0 },
          bg = { src = "/assets/orange/title.png", stretch = "horizontal" } },
        { type = "button", src = "/assets/orange/maximize.png",
          action = "maximize", align = "end" },
        { type = "button", src = "/assets/orange/close.png",
          action = "close",    align = "end" },
      },
    },

    -- LEFT: a column. Flows top to bottom, between the top and bottom regions.
    left = {
      size = 24, gap = 2, padding = { top = 4, bottom = 4 },
      items = {
        { type = "button", src = "/assets/orange/pin.png",   action = "pin"   },
        { type = "button", src = "/assets/orange/float.png", action = "float" },
        { type = "spacer", fill = true },
        { type = "text",   text = "{workspace}", rotate = -90,
          font = "BlueFont", size = 10, height = 40 },
      },
    },

    -- RIGHT: not declared at all -> reserves only what the frame art needs.
    -- Write `right = { size = 0 }` to deliberately reserve nothing.

    -- BOTTOM: a row under the window.
    bottom = {
      size = 16, gap = 6, padding = { left = 8, right = 8 },
      items = {
        { type = "text", text = "{class}", fill = true, align = "start",
          font = "BlueFont", size = 10 },
        -- only exists while the window is pinned; keeps its slot otherwise
        { type = "image", src = "/assets/orange/pinned.png",
          visible_when = { pinned = true }, align = "end" },
        { type = "button", src = "/assets/orange/grip.png",
          action = "resize", align = "end" },
      },
    },

    -- Escape hatch: out of flow, positioned explicitly.
    -- (items may also live at the top level of `defaults` for this)
    --   { type = "image", src = "/assets/orange/badge.png",
    --     at = { relative_to = "frame", anchor = "top-right", x = -2, y = -2 } },
  },

  -- Alternative looks, selected per window by a Hyprland window rule.
  themes = {
    minimal = {
      frame = { texture = "/assets/orange/frame", scale = 2, filter = "nearest" },
      top   = { size = 12, items = { { type = "title", fill = true, size = 10 } } },
    },
  },
})
```

Per-window selection uses window-rule effects. `hyprdecor:no_bar`, `hyprdecor:bar_color`
and `hyprdecor:title_color` are already registered; add `hyprdecor:theme`. The Lua
syntax for a custom-effect rule in Hyprland 0.56 is **unverified**: prove it in a
throwaway config before writing it anywhere. An invalid rule aborts the whole config.

## 3. Semantics (the rules, so nothing is arbitrary)

**Units.** Everything is logical pixels unless stated. `frame.scale` multiplies the art
only, and the reserved space follows it. The monitor scale is applied exactly once, at
render. Layout outputs rectangles **snapped to device pixels**, and both drawing and
hit-testing consume those same rectangles.

**Frame.** One nine-patch drawn over the whole decoration box (window rect + reserved
extents). It is never split into bands; regions only partition the interior, so corners
stay continuous automatically. `_active`/`_inactive` swap by focus.

**Regions.** `top`, `bottom`, `left`, `right`. Each has a `size`, `inset`, `gap`,
`padding`, `items`, optional `drag`.
- `size = "art"` means the nine-patch **content padding** for that side (the bottom and
  right marker bars), falling back to the border, times `frame.scale`. A number is
  logical px and overrides the art. `0` is allowed and means reserve nothing.
- `inset` adds extra reserved space on top of `size`.
- An undeclared side behaves as `size = "art"` with no items, so the window never
  overlaps frame art.
- **A region never sizes to its content.** Only to art or a number. This keeps layout
  finite and keeps reserved space (and therefore the client size) independent of
  titles, focus and visibility.
- Reserved extents come from regions only, through one `currentExtents()` used by
  positioning, drawing and hit-testing.
- `top` and `bottom` span the full decoration width; `left` and `right` run between them.

**Flow.** The main axis is a row for top/bottom and a column for left/right.
- `align = "start" | "centre" | "end"` (default `start`). The `end` group keeps list
  order, so the last-listed item touches the end edge.
- Cross-axis default is centred. `cross_align = "start" | "centre" | "end"` overrides.
- `fill = true` means weight 1; a number is a weight. Fillers share what is left after
  fixed items, gaps and padding, split by weight, each honouring `min`/`max`.
  Two fillers split the space (never an error, never "first wins").
- Fixed items never shrink. If fixed items alone do not fit, items are hidden from the
  last-listed backwards and one warning is logged per window.
- `text` and `title` items must have `fill` or an explicit main-axis length (`width` in a
  row, `height` in a column). This is a setup()
  error otherwise, because layout is pure and never measures fonts.

**Items.** `image`, `button`, `title`, `text`, `spacer`. All share: `id`, `align`,
`cross_align`, `fill`, `min`, `max`, `width`, `height`, `margin`, `src`/`scale`/`filter` where
an image applies, `visible_when`, `collapse`, `at`.

**Item sizing and spacing.** `width` and `height` (logical px) set an item's box. An unset
dimension is the natural size times `scale` for images and buttons; on the cross axis a
`title` or `text` item uses the region's cross size minus padding. `margin = { left, top,
right, bottom }` adds space around an item inside the flow. With `cross_align`, this places a
short title strip inside a taller band without any vertical "gap":
`{ type = "title", fill = true, height = 22, cross_align = "start", margin = { top = 6 } }`.

**Global scale.** `defaults.scale = N` scales EVERYTHING: the frame art and every item that
does not set its own `scale`. Resolution order, highest first:

1. `<item>.scale` / `frame.scale` — that one thing only
2. `defaults.scale` — the fallback for both
3. `1`

So `defaults.scale = 2` with one element at `scale = 4` draws that element at 4 and
everything else at 2. The monitor scale is applied on top, exactly once, as for every other
length.
- `button`: an image with states plus an optional `action`. No action means
  hover/pressed art only.
- Actions: `close`, `maximize`, `fullscreen`, `pin`, `float`, `minimize`.
  `minimize` parks the window on `special:min-<its workspace>` (project convention, not
  a native Hyprland state); the bar lists those windows per workspace.
  `maximize` and `fullscreen` toggle: a second press restores. They pass only `mode` to
  the dispatcher, letting Hyprland do the state decision — reading the state here and
  choosing set/unset duplicated it and made restore fail.
  `retro_filter` runs `hyprshade toggle <bar.retro_shader>`; it is the only action that
  starts a process.
  `drag` and `resize` were dropped: they are pointer GESTURES, not clicks, and the
  decorator swallows the press they need. Hyprland already offers both through
  `SUPER + right-drag` / `SUPER + left-drag` and `general:resize_on_border`, so a
  decoration button adds nothing and cannot work reliably.
  The plugin never spawns processes or shells itself; a user who
  wants a command binds a dispatcher that runs one.
  **Verified (C4 probe, 0.56.2):** `hyprctl dispatch` on a Lua-config Hyprland evaluates
  its argument as Lua and takes a CType, not a command name. The plugin's internal
  `m_dispatchers["closewindow"]` / `["fullscreenstate"]` / `["togglefloating"]` /
  `["pin"]` do resolve, confirmed by clicking real buttons. A `dispatch = "name args"`
  STRING therefore cannot work; the escape hatch needs a table of known verbs.
  Lua callbacks (`on_click = function ...`) are **deliberately not in v1**: holding Lua
  references and re-entering Lua from the input/render path is the same hazard class as
  the old re-entrant setup() bugs.
- `text` / `title` templates: `{title}`, `{class}`, `{workspace}`. An unknown
  placeholder is a setup() error. `rotate` is `0`, `90` or `-90`.

**State textures.** Resolved from `src` by suffix. Precedence: `_pressed`, `_hovered`,
`_on` (toggle actions that are currently active), `_unfocused`, then the base file. A
missing file falls through to the next one down.

**`visible_when` is a filter, not an expression.** v1 keys: `focused`, `floating`,
`maximised`, `fullscreen`, `pinned`.
- A hidden item **keeps its slot** (invisible, not hit-testable), so nothing moves when
  focus or state changes. `collapse = true` opts in to releasing the slot.
- It is different from `_unfocused`: `_unfocused` swaps pixels; `visible_when` decides
  whether the item exists on screen at all. Both can be used on one item.

**`at` (escape hatch).** `at = { relative_to = "frame" | "<region name>" | "<item id>",
anchor = <one of 9>, x = , y = }`. Nine anchors: top-left, top-centre, top-right,
centre-left, centre, centre-right, bottom-left, bottom-centre, bottom-right. An item with
`at` is out of flow and takes no flow space.

**Validation.** Everything is validated in `setup()`; the error names the path:
`hyprdecor.setup: defaults.left.items[3].src: file not found: "/x/pin.png"`.
`setup()` applies completely or not at all. Soft problems (a stretch band under 2px, an
item hidden by overflow) are warnings, logged once.

**Legacy sugar.** `defaults.border`, `defaults.title` and `defaults.elements` keep
loading and desugar into `frame` and `top` items, with a one-time deprecation log.
The current `test.lua` must keep rendering the same through every commit before the last.

**`hyprctl hyprdecor status`** prints JSON:

```json
{ "version": 1,
  "windows": [ {
    "address": "0x55b2...",
    "decoration_box": { "x": 4, "y": 4, "w": 467, "h": 245 },
    "reserved": { "left": 12, "top": 30, "right": 4, "bottom": 14 },
    "frame": { "texture": "/assets/orange/frame", "state": "active" },
    "items": [ { "id": "close", "region": "top", "type": "button",
                 "rect": { "x": 440, "y": 8, "w": 16, "h": 16 },
                 "visible": true, "state": "hovered" } ]
  } ] }
```
Coordinates are global logical pixels; include the monitor scale.

---
