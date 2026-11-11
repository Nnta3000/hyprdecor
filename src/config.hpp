#pragma once

// The plugin's entire configuration, in one place.
//
// This replaces the legacy HyprlandAPI::addConfigValue() registrations: the Lua config
// manager never commits those values, so every read needed a null check and a fallback,
// and a wrong fallback silently changed behaviour (see the black title text bug).
//
// Everything here is filled by hl.plugin.hyprdecor.setup(); the struct itself starts at
// documented defaults, so an unset field is always a deliberate value, never garbage.

#include "itemstate.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct SFrameThickness
{
    // Per-side reserved space in logical pixels. -1 means "not set".
    double left   = -1;
    double top    = -1;
    double right  = -1;
    double bottom = -1;

    bool   allSet() const { return left >= 0 && top >= 0 && right >= 0 && bottom >= 0; }
};

// Per-side override used when a named theme is resolved over the default config: a
// theme side is authoritative only when it is set (>= 0).
namespace Sides {
inline SFrameThickness over(const SFrameThickness &higher, const SFrameThickness &base)
{
    return {higher.left >= 0 ? higher.left : base.left,
            higher.top >= 0 ? higher.top : base.top,
            higher.right >= 0 ? higher.right : base.right,
            higher.bottom >= 0 ? higher.bottom : base.bottom};
}
} // namespace Sides

// A plain image drawn at a fixed place inside the frame band. No hit-testing, no
// action: this is the primitive for art like an eye, a title background or a button
// backdrop. Interactive buttons stay a separate feature.
struct SElement
{
    enum class eAnchor : uint8_t {
        TopLeft,
        TopCentre,
        TopRight,
    };

    // Which side of the frame this element lives in. A row for top/bottom, a column for
    // left/right. Defaults to top so every existing config keeps working unchanged.
    enum class eRegion : uint8_t {
        Top,
        Bottom,
        Left,
        Right,
    };

    eRegion     region = eRegion::Top;

    // What this entry draws. `image` and `button` need a file; `spacer` and `text` do
    // not -- a spacer only takes flow space and text is drawn by the renderer.
    enum class eType : uint8_t {
        Image,
        Button,
        Spacer,
        Text,
    };

    eType       type = eType::Image;

    std::string path;
    // Only for type == Text.
    std::string text;
    eAnchor     anchor  = eAnchor::TopLeft;
    double      offsetX = 0;
    double      offsetY = 0;
    // <= 0 means "use the image's natural size".
    double width  = -1;
    double height = -1;

    // Per-element overrides; nullopt = inherit from the border defaults.
    std::optional<bool>   filter; // true = linear/bilinear, false = nearest
    std::optional<int>    scale;  // overrides border.scale
    // Nine-patch stretch band, in source px: -1 = unset, 0 = corners only, N = N px.
    std::optional<double> maxStretch; // ceiling (-1 = none)
    std::optional<double> minStretch; // floor  (-1 = none)

    // Draw as a nine-patch instead of a plain image. Horizontal only by default: a
    // title background stretches sideways but keeps its height.
    bool   ninepatch      = false;
    bool   stretchVertical = false;

    // Interaction. State textures are resolved from `path` by suffix, the same way the
    // frame resolves _inactive:
    //
    //   path                  window focused, pointer elsewhere
    //   path + "_unfocused"   window NOT focused (frame_inactive is showing)
    //   path + "_hovered"     pointer over this element
    //   path + "_pressed"     pointer pressed on this element
    //
    // A missing file falls back to the focused texture, so adding states is optional.
    bool   interactive = false; // hit-testable: enables _hovered / _pressed

    // What a click does. Named actions, never shell commands (ROADMAP D2): the plugin
    // asks Hyprland to do something it already knows. Empty means "no action": the
    // element is still hoverable, which is enough for decorative art.
    //
    // `pin` floats the window first, because Hyprland rejects pinning a tiled window.
    enum class eAction : uint8_t {
        None,
        Close,
        Maximize,
        Fullscreen,
        Pin,
        Float,
        // Parks the window on special:min-<its workspace>. Project convention for
        // "minimise"; Hyprland has no native minimised state (see AGENTS.md).
        Minimize,
    };

    eAction action = eAction::None;

    // `visible_when` is a FILTER: every declared condition must match. An unset field
    // means "don't care", never "must be false". A hidden item keeps its slot, so
    // nothing shifts when a window is pinned or focused; `collapse` releases the slot.
    std::optional<bool> whenFocused;
    std::optional<bool> whenFloating;
    std::optional<bool> whenMaximised;
    std::optional<bool> whenFullscreen;
    std::optional<bool> whenPinned;
    bool                collapse = false;

    // Flow, inside the element's region. `fill` makes the element share the leftover
    // space by `weight`; `align` places it on the main axis, `crossAlign` across it.
    bool   fill   = false;
    double weight = 1;
    std::string crossAlign = "centre"; // start | centre | end
    // Space around the element inside the flow.
    double marginLeft = 0, marginTop = 0, marginRight = 0, marginBottom = 0;
};

// Per-window frame override. Empty optionals inherit the corresponding default from
// SConfig; each theme is resolved into its own SFrame at update() time.
struct STheme
{
    std::optional<std::string> textureBase;
    std::optional<int>         artScale;
    std::optional<bool>        linearFiltering;
    std::optional<bool>        repeat;
    SFrameThickness            thickness;
    SFrameThickness            inset;
};

// Plugin-side selection: the app id (Wayland app_id / X11 class) of a window. Kept as
// a list rather than a custom Hyprland rule effect because the Lua syntax for custom
// effects on this build is still unverified.
struct SThemeRule
{
    std::string className;
    std::string theme;
};

struct SConfig
{
    bool        enabled = true;

    // Frame texture. `textureBase` is the path without extension; the actual files are
    // resolved per state (_active/_inactive).
    std::string textureBase;

    // `thickness` OVERRIDES how thick the frame art is, per side. Leave it unset and
    // the nine-patch markers decide, which is almost always what you want: the markers
    // exist precisely to describe the art.
    SFrameThickness thickness;

    // `inset` ADDS space on top of the art, per side, pushing the window inward while
    // the nine-patch stretches to fill the larger box. The frame is never drawn with a
    // hole: reserved area == the box the art is stretched over. A visual gap belongs
    // in the png, not here.
    SFrameThickness inset;

    // Scales the frame ART only (pixel-art art at 2x, 3x, ...). Because the art then
    // occupies artScale times as many pixels, the reserved space is scaled to match -
    // otherwise the frame would overlap the window or leave a gap.
    // Independent of the monitor scale and of any future title.scale.
    int artScale = 1;

    // `defaults.scale`: one number that scales EVERYTHING - the frame, and every element
    // that does not set its own. 0 means "not set", so an explicit per-element
    // `scale` can be distinguished from "inherit". Resolved at setup() time, not at
    // draw time, so the layout pass always sees a concrete number.
    int defaultScale = 1;

    // `defaults.scale` as a multiplier. Kept in one place so every distance, offset and
    // text size reads the same clamped value instead of re-deriving it.
    double scale() const { return (double)std::max(1, defaultScale); }

    bool   linearFiltering = true;
    bool   repeat          = false;

    // Title. Two explicit modes, like `elements`:
    //
    //   "fill" : the box is whatever the elements anchored left/right leave between
    //            them. `gap` is the distance from those elements.
    //   "box"  : the box is placed like an element: `anchor` + `offset`, then sized.
    enum class eTitleMode : uint8_t {
        Fill,
        Box,
    };

    // Where the box sits in the band. `offset` is applied from that corner/edge.
    enum class eTitleAnchor : uint8_t {
        TopLeft,
        TopCentre,
        TopRight,
        CentreLeft,
        Centre,
        CentreRight,
        BottomLeft,
        BottomCentre,
        BottomRight,
    };

    eTitleMode   titleMode   = eTitleMode::Fill;
    eTitleAnchor titleAnchor = eTitleAnchor::Centre;
    double      titleOffsetX   = 0;        // box mode: from the anchor
    double      titleOffsetY   = 0;
    // box mode: stretch band in source px (0 = corners only, -1 = natural size).
    double      titleWidth     = -1;
    double      titleHeight    = -1;

    bool        titleEnabled   = true;
    int         titleSize      = 10;
    std::string titleFont      = "Sans";
    // Font for the title while the window is NOT focused. Empty = use titleFont, so an
    // existing config renders exactly as before.
    std::string titleFontUnfocused;
    std::string titlePlacement = "top";
    uint64_t    textColor      = 0xFFFFFFFF;
    // Padding INSIDE the title box, between its edge and the text.
    double      titleInset     = 0;
    // Vertical placement of the text baseline inside the title box.
    //
    //   "font"  (default) centre on the FONT's own ascent+descent, not on the laid-out
    //           line box. The line box is the maximum over every run, so one fallback
    //           glyph from a different family (an accented letter the pixel font lacks)
    //           makes the whole line taller and shifts every title unevenly.
    //   "line"  the old behaviour: centre on the laid-out height.
    //
    // "font" is what keeps a title steady whether or not its text falls back.
    std::string titleBaseline = "font"; // font | line
    // Extra pixels added to the baseline in "font" mode. Positive moves the text DOWN.
    // Art units, so it scales with defaults.scale. Use it to compensate for a pixel
    // font whose real ink sits off its nominal metrics.
    double      titleBaselineOffset = 0;
    // Distance between adjacent items on a region's main axis.
    double      regionGap = 0;
    // End items stack from the end edge instead of anchoring to it independently, so the
    // last-listed one touches the edge. OPT-IN because it moves existing end items.
    bool        regionStackEnd = false;
    // The title is a FLOW ITEM (kind == Title) in the top region, so it is spaced by
    // `margin` like every other item. There is no title-specific gap: `margin` replaces
    // it, and a negative side tucks the title under a neighbouring icon.
    double      titleMarginLeft   = 0;
    double      titleMarginTop    = 0;
    double      titleMarginRight  = 0;
    double      titleMarginBottom = 0;
    // Cross-axis placement when the box is shorter than the band.
    std::string titleCrossAlign = "centre"; // start | centre | end
    // Moves the TEXT inside its box (and its background), independent of the box.
    double      textOffsetX    = 0;
    double      textOffsetY    = 0;
    bool        titleEllipsize = true;

    // Horizontal alignment of the text inside its box. Derived from the anchor in box
    // mode, from `align` in fill mode (kept for the fill case where there is no anchor).
    std::string titleAlign     = "left"; // left | centre | right

    // Optional backdrop drawn BEHIND the text. Nested here rather than being a sibling
    // element, so the two can never drift apart: the background is derived from the
    // text box, not positioned independently.
    bool        titleBgEnabled = false;
    std::string titleBgPath;

    // Bar
    uint64_t    barColor             = 0x88333333;
    bool        blur                 = false;
    int         padding              = 7;

    // Width of the band along the OUTER edge of the frame that starts a resize, in ART
    // px (so it follows defaults.scale). Measured from the decoration box inward.
    //
    // This exists because Hyprland's own border-resize only triggers OUTSIDE the window,
    // and it skips any point inside a decoration declaring DECORATION_ALLOWS_MOUSE_INPUT
    // -- which the frame is. So without this band the frame was grabbable for a drag but
    // never showed or acted as a resize edge. 0 disables the band entirely.
    double      resizeGrabWidth      = 8;
    bool        partOfWindow         = true;
    bool        precedenceOverBorder = false;
    // The frame and its items are drawn OVER the window, never behind it. Under, the
    // client area painted on top of the frame's inner edge and any item overlapping the
    // window was hidden by it, which made `inset` look broken. Opting out is still
    // possible for a decoration that must never cover client pixels.
    bool        renderAbove          = true;

    // App icon
    bool     appIconEnabled = false;
    double   appIconOffsetX = 0;
    double   appIconOffsetY = 0;

    // Plain solid outline, useful before textures are set up.
    uint64_t outlineColor  = 0xFFFFFFFF;
    int      outlineWidth  = 0;
    bool     outlineAlways = false;

    // Per-window frame alternatives and the app-id rules that select them.
    std::map<std::string, STheme> themes;
    std::vector<SThemeRule>       rules;

    // Positioned images in the top band, drawn in order.
    std::vector<SElement> elements;

    // How many times setup() has been applied, for logging / status.
    int applies = 0;
};

