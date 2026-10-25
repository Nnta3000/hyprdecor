// Resolved layout: one pure pass that turns a frame's metrics plus the configured
// elements into a list of rectangles in LOGICAL pixels.
//
// This is the single source of truth for "where is everything". Drawing, hit-testing and
// `hyprctl hyprdecor status` all read the same list. Calling separate bandBox() /
// elementBox() style helpers from several places is exactly how hover drifted away from
// the drawn art twice, so those helpers are replaced by this module.
//
// No Hyprland includes: the tests cover this file directly.

#pragma once

#include "ninepatch.hpp"

#include <algorithm>
#include <format>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Layout
{

enum class eRegion : uint8_t {
    Top,
    Bottom,
    Left,
    Right,
};

enum class eItemKind : uint8_t {
    Image,
    Button,
    Title,
};

enum class eAnchor : uint8_t {
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

// Where an element sits on its region's main axis.
enum class eAlign : uint8_t {
    Start,
    Centre,
    End,
};

struct SRect
{
    double x = 0, y = 0, w = 0, h = 0;

    bool drawable() const { return w >= 1 && h >= 1; }
    bool contains(double px, double py) const { return px >= x && px < x + std::max(1.0, w) && py >= y && py < y + std::max(1.0, h); }
};

// Everything about the frame that layout needs, resolved ONCE from the config in logical
// pixels. Passing raw config values around instead is what produced the unit mixing.
struct SFrameMetrics
{
    // Reserved space per side, in logical px: the art's content padding (or border),
    // times frame.scale.
    double left = 0, top = 0, right = 0, bottom = 0;

    // Nine-patch content inset of the art, already scaled. This is the part of the frame
    // that must never be covered by an item or the window.
    double contentLeft = 0, contentTop = 0, contentRight = 0, contentBottom = 0;

    // bar.padding: the gap the old band left between the frame and the items.
    double padding = 0;

    // The decoration box in logical pixels, in decoration-local coordinates.
    double boxW = 0, boxH = 0;

    // Extra space requested per side by a region's own `inset`.
    double insetLeft = 0, insetTop = 0, insetRight = 0, insetBottom = 0;
};

// One configured element, reduced to what layout needs. Nothing here measures a font:
// layout is pure, so text items must be given a width.
struct SItemSpec
{
    int          id      = 0;
    eItemKind    kind    = eItemKind::Image;
    eAlign       align   = eAlign::Start;
    double       offsetX = 0;
    double       offsetY = 0;
    double       w       = -1; // <= 0: natural width (image) or an error (text)
    double       h       = -1; // <= 0: natural height
    bool         fill    = false;
    // Stretch band of a nine-patch item, in source px; -1 = unset. A plain image ignores
    // these and draws at its natural size.
    bool         ninepatch     = false;
    bool         stretchVertical = false;
    double       minStretch    = -1;
    double       maxStretch    = -1;
    // Natural size in SOURCE px, scaled by the item's own scale factor by layout.
    double       srcW = 0, srcH = 0;
    double       scale = 1;
    // Nine-patch markers, used when `ninepatch` is set.
    SNinePatchInfo npi;
    // Present only for the title, which is sized by what its neighbours leave.
    bool         isTitle = false;
};

struct SResolvedItem
{
    int        id     = 0;
    eItemKind  kind   = eItemKind::Image;
    eRegion    region = eRegion::Top;
    SRect      rect;
    // The rectangle the texture is drawn into. For a plain image this equals `rect`;
    // for a nine-patch it is the size the stretch band actually produced, which a cap
    // may make smaller than the space it was given.
    SRect      draw;
    // Source px -> logical px for this item, used by the renderer for UVs and repetition.
    double     artScale = 1;
    // True when the nine-patch had less room than its fixed corners need.
    bool       overflowed = false;
};

struct SResult
{
    std::vector<SResolvedItem> items;
    // True when the neighbours left the title no room, so nothing is drawn for it.
    bool titleVoids = false;
};

// The band a region occupies inside the decoration box, in logical px. `top`/`bottom`
// span the full width; `left`/`right` run between them.
inline SRect regionRect(const SFrameMetrics &m, eRegion region)
{
    const double L = m.left + m.insetLeft;
    const double T = m.top + m.insetTop;
    const double R = m.right + m.insetRight;
    const double B = m.bottom + m.insetBottom;

    switch (region)
    {
        case eRegion::Top: return {0, 0, m.boxW, T};
        case eRegion::Bottom: return {0, m.boxH - B, m.boxW, B};
        case eRegion::Left: return {0, T, L, std::max(0.0, m.boxH - T - B)};
        case eRegion::Right: return {m.boxW - R, T, R, std::max(0.0, m.boxH - T - B)};
    }

    return {};
}

// The area inside a region where items may sit: the region minus the frame's content
// inset and bar.padding. This is the old "band", kept with the same numbers so the
// existing config renders identically.
inline SRect itemArea(const SFrameMetrics &m, eRegion region, const SRect &r)
{
    const double P = m.padding;

    switch (region)
    {
        case eRegion::Top:
            return {r.x + m.contentLeft + P, r.y, std::max(0.0, r.w - m.contentLeft - m.contentRight - 2 * P), std::max(0.0, m.top + P)};
        case eRegion::Bottom:
            return {r.x + m.contentLeft + P, r.y + r.h - (m.bottom + P), std::max(0.0, r.w - m.contentLeft - m.contentRight - 2 * P), m.bottom + P};
        case eRegion::Left:
            return {r.x, r.y + m.contentTop + P, m.left + P, std::max(0.0, r.h - m.contentTop - m.contentBottom - 2 * P)};
        case eRegion::Right:
            return {r.x + r.w - (m.right + P), r.y + m.contentTop + P, m.right + P, std::max(0.0, r.h - m.contentTop - m.contentBottom - 2 * P)};
    }

    return {};
}

// Natural size of an item in logical px: source px times the item's own scale. A
// nine-patch that stretches horizontally keeps the height of its fixed art.
inline void naturalSize(const SItemSpec &s, double &w, double &h)
{
    w = s.w > 0 ? s.w : s.srcW * s.scale;
    h = s.h > 0 ? s.h : s.srcH * s.scale;

    if (s.ninepatch && s.npi.defined && !s.stretchVertical)
        h = (s.npi.border[1] + s.npi.border[3]) * s.scale;
}

// Where an item sits on its cross axis: centred by default.
inline double crossPos(const SRect &area, double size, eRegion region)
{
    const bool HORIZONTAL = region == eRegion::Top || region == eRegion::Bottom;
    return HORIZONTAL ? area.y + (area.h - size) / 2.0 : area.x + (area.w - size) / 2.0;
}

// Lays out one region's items along the main axis.
//
// Start-aligned items stack from the start edge in list order. End-aligned items are
// anchored to the end edge INDEPENDENTLY, matching the old element placement so that the
// first commit is behaviour-preserving; C3 turns them into a stacked flow. The title
// fills what is left between the furthest start item and the nearest end item.
inline void layoutRegion(const SFrameMetrics &m, eRegion region, const std::vector<SItemSpec> &specs, std::vector<SResolvedItem> &out,
                         bool &titleVoids)
{
    const bool   HORIZONTAL = region == eRegion::Top || region == eRegion::Bottom;
    const SRect  RR         = regionRect(m, region);
    const SRect  AREA       = itemArea(m, region, RR);

    if (AREA.w <= 0 || AREA.h <= 0)
        return;

    // Main-axis extent available to items.
    const double MAIN = HORIZONTAL ? AREA.w : AREA.h;
    const double MAIN_ORIGIN = HORIZONTAL ? AREA.x : AREA.y;

    // Measure the fixed items first: the title takes whatever they leave.
    double        startUsed = 0, endUsed = 0;
    bool          hasTitle  = false;

    for (const auto &s : specs)
    {
        if (s.isTitle)
        {
            hasTitle = true;
            continue;
        }

        double w = 0, h = 0;
        naturalSize(s, w, h);
        const double EXTENT = HORIZONTAL ? w : h;

        if (s.align == eAlign::Start)
            startUsed += EXTENT;
        else if (s.align == eAlign::End)
            endUsed += EXTENT;
    }

    const double FREE = std::max(0.0, MAIN - startUsed - endUsed);

    if (hasTitle && FREE <= 0)
        titleVoids = true;

    double cursorStart = MAIN_ORIGIN;

    for (const auto &s : specs)
    {
        double w = 0, h = 0;
        naturalSize(s, w, h);

        // The title spans the free gap, whatever it is.
        if (s.isTitle)
        {
            w = HORIZONTAL ? FREE : w;
            h = HORIZONTAL ? AREA.h : FREE;
            if (FREE <= 0)
                continue;
        }

        double mainPos = 0;

        if (s.isTitle || s.align == eAlign::Start)
        {
            mainPos = cursorStart + s.offsetX;
            cursorStart += HORIZONTAL ? w : h;
        }
        else if (s.align == eAlign::End)
        {
            // C1 keeps the OLD behaviour, where every element was positioned from its
            // own anchor and none was pushed by its neighbours. C3 replaces this with
            // stacking flow; until then this is what makes the commit pixel-identical.
            mainPos = MAIN_ORIGIN + MAIN - (HORIZONTAL ? w : h) + s.offsetX;
        }
        else
            mainPos = MAIN_ORIGIN + (MAIN - (HORIZONTAL ? w : h)) / 2.0 + s.offsetX;

        if (!HORIZONTAL)
            mainPos += 0; // the column already sits in AREA.x

        const double CROSS = crossPos(AREA, HORIZONTAL ? h : w, region);

        SResolvedItem item;
        item.id       = s.id;
        item.kind     = s.kind;
        item.region   = region;
        item.artScale = s.scale;

        if (HORIZONTAL)
            item.rect = {mainPos, CROSS + s.offsetY, w, h};
        else
            item.rect = {CROSS + s.offsetY, mainPos, w, h};

        item.draw = item.rect;

        // A nine-patch item may be capped in the middle, so the drawn extent can be
        // smaller than the rectangle it was given. Resolve that here, once, instead of
        // in the renderer.
        if (s.ninepatch && s.npi.defined)
        {
            const double MAXW = s.maxStretch;
            const double MINW = s.minStretch;
            const double MAXH = s.stretchVertical ? MAXW : 0;
            const double MINH = s.stretchVertical ? MINW : 0;

            const auto G = NinePatch::layout(s.npi, s.srcW, s.srcH, item.rect.w, item.rect.h, s.scale, MAXW, MAXH, MINW, MINH);

            item.draw.w       = NinePatch::layoutWidth(G);
            item.draw.h       = NinePatch::layoutHeight(G);
            item.overflowed   = G.degraded;
        }

        out.push_back(item);
    }
}

// One spec list per region. Regions are independent: a row flows left to right, a column
// flows top to bottom, and neither is affected by the other except through the reserved
// space the frame metrics already account for.
inline SResult compute(const SFrameMetrics &m, const std::vector<SItemSpec> &top, const std::vector<SItemSpec> &bottom = {},
                       const std::vector<SItemSpec> &left = {}, const std::vector<SItemSpec> &right = {})
{
    SResult result;

    for (const auto &[REGION, SPECS] : {std::pair{eRegion::Top, &top}, std::pair{eRegion::Bottom, &bottom}, std::pair{eRegion::Left, &left},
                                       std::pair{eRegion::Right, &right}})
    {
        bool voided = false;
        layoutRegion(m, REGION, *SPECS, result.items, voided);

        // Any region's title that cannot fit voids; there is only ever one title today.
        if (voided)
            result.titleVoids = true;
    }

    return result;
}

// Renders the resolved items as a JSON array, so the exact same rectangle list can be
// asserted in tests and printed by `hyprctl hyprdecor status`. `originX/Y` translate
// decoration-local coordinates to global ones.
inline std::string itemsJson(const std::vector<SResolvedItem> &items, double originX, double originY)
{
    const auto FMT = [](double v) { return std::format("{:.0f}", std::round(v)); };
    const auto KIND = [](eItemKind k) { return k == eItemKind::Button ? "button" : k == eItemKind::Title ? "title" : "image"; };

    std::string out = "[";
    bool        first = true;

    for (const auto &item : items)
    {
        if (!first)
            out += ", ";
        first = false;

        out += std::format("{{\"id\": {}, \"type\": \"{}\", \"region\": \"{}\", \"rect\": {{\"x\": {}, \"y\": {}, \"w\": {}, \"h\": {}}}}}",
                           item.id, KIND(item.kind), item.region == eRegion::Top ? "top" : item.region == eRegion::Bottom ? "bottom" : item.region == eRegion::Left ? "left" : "right",
                           FMT(originX + item.rect.x), FMT(originY + item.rect.y), FMT(item.rect.w), FMT(item.rect.h));
    }

    out += "]";
    return out;
}

// Rounds a resolved layout to whole logical pixels and snaps it to whole DEVICE pixels,
// which is what the specification requires: drawing and hit-testing consume the same
// rectangles, so neither may keep sub-pixel values the other rounds away.
inline void snap(std::vector<SResolvedItem> &items, double monitorScale)
{
    const auto SNAP = [&](double v) { return monitorScale > 0 ? std::round(std::round(v * monitorScale) / monitorScale) : v; };

    for (auto &item : items)
    {
        item.rect.x = SNAP(item.rect.x);
        item.rect.y = SNAP(item.rect.y);
        item.rect.w = SNAP(item.rect.w);
        item.rect.h = SNAP(item.rect.h);
        item.draw.x = SNAP(item.draw.x);
        item.draw.y = SNAP(item.draw.y);
        item.draw.w = SNAP(item.draw.w);
        item.draw.h = SNAP(item.draw.h);
    }
}

}
