#pragma once

// The plugin's entire configuration, in one place.
//
// This replaces the legacy HyprlandAPI::addConfigValue() registrations: the Lua config
// manager never commits those values, so every read needed a null check and a fallback,
// and a wrong fallback silently changed behaviour (see the black title text bug).
//
// Everything here is filled by hl.plugin.hyprdecor.setup(); the struct itself starts at
// documented defaults, so an unset field is always a deliberate value, never garbage.

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

    std::string path;
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
    std::string titlePlacement = "top";
    uint64_t    textColor      = 0xFFFFFFFF;
    // Padding INSIDE the title box, between its edge and the text.
    double      titleInset     = 0;
    // fill mode: distance between the title box and its neighbours, on the main axis
    // only. One scalar, because a gap needs two things to sit between; the vertical
    // placement of a short title is `height` + `cross_align` + `margin`, not a gap.
    // Negative pulls the box past its neighbours (tucking it under an icon).
    double      titleGap       = 0;
    // Cross-axis placement when the box is shorter than the band.
    std::string titleCrossAlign = "centre"; // start | centre | end
    // Space around the box inside the flow; only `top`/`bottom` can act on a row.
    double      titleMarginTop    = 0;
    double      titleMarginBottom = 0;
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
    int         buttonPadding        = 5;
    std::string buttonsAlignment     = "right";
    bool        partOfWindow         = true;
    bool        precedenceOverBorder = false;
    bool        renderAbove          = false;
    std::string onDoubleClick;

    // App icon
    bool     appIconEnabled = false;
    double   appIconOffsetX = 0;
    double   appIconOffsetY = 0;

    // Plain solid outline, useful before textures are set up.
    uint64_t outlineColor  = 0xFFFFFFFF;
    int      outlineWidth  = 0;
    bool     outlineAlways = false;

    // Positioned images in the top band, drawn in order.
    std::vector<SElement> elements;

    // How many times setup() has been applied, for logging / status.
    int applies = 0;
};

inline SConfig g_config;
