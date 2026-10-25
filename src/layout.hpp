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
#include <unordered_map>
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

    // Distance between adjacent items on a region's main axis.
    double gap = 0;

    // End-aligned items stack instead of each anchoring independently. Off by default:
    // turning it on MOVES existing end items, so it is opt-in per region.
    bool stackEnd = false;

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
    double       weight  = 1;                 // fill weight; `fill = true` is weight 1
    eAlign       crossAlign = eAlign::Centre; // vertical for a row, horizontal for a column
    double       marginLeft = 0, marginTop = 0, marginRight = 0, marginBottom = 0;
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
    // The title is an ITEM (kind == Title) like any other, so its rectangle appears in
    // `items`. This index points at it for convenience, or -1 when there is none.
    int titleIndex = -1;
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

    // A nine-patch that stretches only horizontally keeps the height of its fixed art,
    // but an EXPLICIT height always wins: it is the user saying exactly how tall the
    // strip should be (a short title inside a taller band).
    if (s.ninepatch && s.npi.defined && !s.stretchVertical && !(s.h > 0))
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
// FLOW, as specified:
//   - start items stack from the start edge, in list order
//   - end items stack from the END edge, in list order, so the last-listed item touches
//     the edge. (C1 anchored every end item to the edge independently, matching the old
//     element placement; C3 replaces that with real stacking. Items with an explicit
//     offset keep their old position because the offset is still applied on top.)
//   - fillers share what is left after fixed items, gaps and padding, split by weight
//   - a gap separates adjacent items on the main axis
//
// The title is simply a filler whose main-axis size is its weight share, which is what
// makes "the title is just an item" true rather than a special case.
inline void layoutRegion(const SFrameMetrics &m, eRegion region, const std::vector<SItemSpec> &specs, std::vector<SResolvedItem> &out,
                         bool &titleVoids)
{
    const bool   HORIZONTAL  = region == eRegion::Top || region == eRegion::Bottom;
    const SRect  RR          = regionRect(m, region);
    const SRect  AREA        = itemArea(m, region, RR);

    if (AREA.w <= 0 || AREA.h <= 0)
        return;

    const double MAIN        = HORIZONTAL ? AREA.w : AREA.h;
    const double MAIN_ORIGIN = HORIZONTAL ? AREA.x : AREA.y;

    const auto EXTENT = [&](const SItemSpec &s) {
        double w = 0, h = 0;
        naturalSize(s, w, h);
        return HORIZONTAL ? w : h;
    };

    const auto CROSS_OF = [&](const SItemSpec &s) {
        double w = 0, h = 0;
        naturalSize(s, w, h);
        return HORIZONTAL ? h : w;
    };

    // 1. Measure. Fixed items take their natural size; fillers claim weight.
    // Main-axis margin of an item: the only margins that take flow space.
    const auto MAIN_MARGIN = [&](const SItemSpec &s) { return HORIZONTAL ? s.marginLeft + s.marginRight : s.marginTop + s.marginBottom; };

    double startFixed = 0, endFixed = 0, startMargins = 0, endMargins = 0, totalWeight = 0;
    int    startCount = 0, endCount = 0;

    for (const auto &s : specs)
    {
        // A filler's margins are NOT fixed cost: they are taken out of its own share
        // below, so `margin` narrows a filler instead of letting it overrun its
        // neighbours. A fixed item's margins are real space it consumes.
        if (s.fill)
        {
            totalWeight += s.weight;
            continue;
        }

        if (s.align == eAlign::End)
        {
            endFixed += EXTENT(s);
            endMargins += MAIN_MARGIN(s);
            endCount++;
        }
        else if (s.align == eAlign::Start)
        {
            startFixed += EXTENT(s);
            startMargins += MAIN_MARGIN(s);
            startCount++;
        }
    }

    // Gaps separate ADJACENT items, so there is one fewer gap than there are items on
    // each side that has any.
    const double GAPS = m.gap * (double)(std::max(0, startCount - 1) + std::max(0, endCount - 1) + (startCount > 0 && endCount > 0 ? 1 : 0));

    const double FREE = std::max(0.0, MAIN - startFixed - endFixed - startMargins - endMargins - GAPS);

    if (totalWeight > 0 && FREE <= 0)
        titleVoids = true;

    // 2. Place. Two cursors walk inwards; fillers are placed in list order between them.
    double cursorStart = MAIN_ORIGIN;
    double cursorEnd   = MAIN_ORIGIN + MAIN;

    // Placement order: start and fill items in list order, then end items in REVERSE so
    // the last-listed one is placed first against the end edge and the rest stack
    // inwards. Walking the end group forwards would run that side backwards.
    std::unordered_map<const SItemSpec *, SResolvedItem> PLACED;

    int STARTPLACED = 0, ENDPLACED = 0;

    std::vector<const SItemSpec *> WALK;
    for (const auto &s : specs)
        if (!(s.align == eAlign::End && !s.fill))
            WALK.push_back(&s);

    for (auto it = specs.rbegin(); it != specs.rend(); ++it)
        if (it->align == eAlign::End && !it->fill)
            WALK.push_back(&*it);

    for (const auto *sp : WALK)
    {
        const auto &s = *sp;
        bool isFiller = s.fill;

        // A filler with an explicit main-axis size keeps it; otherwise it takes its
        // weight share of the leftover. That is how a short title strip is expressed:
        // fill = true plus an explicit height.
        const double EXPLICIT = EXTENT(s);
        const bool   HASEXPLICIT = EXPLICIT > 0;
        // A filler's share is reduced by its own main-axis margins, so `margin` on a
        // filler changes its SIZE. Applying the margin as a shift instead left the box
        // full width and made it overrun the end group by exactly the margin.
        const double SELF_MARGIN = isFiller ? MAIN_MARGIN(s) : 0.0;
        double       extent      = isFiller ? (HASEXPLICIT ? EXPLICIT : (totalWeight > 0 ? std::max(0.0, FREE * s.weight / totalWeight - SELF_MARGIN) : 0.0))
                                            : EXPLICIT;

        // A filler's share already had BOTH margins subtracted from its SIZE. The trailing
        // margin must therefore also be consumed by the cursor, and the leading one moves
        // the box; together they keep the far edge pinned instead of letting it drift.
        const double FILL_TRAIL = isFiller ? (HORIZONTAL ? s.marginRight : s.marginBottom) : 0.0;

        if (isFiller && extent <= 0)
            continue;


        double       mainPos = 0;
        // Cross size: a filler defaults to the whole band, but an explicit cross size is
        // honoured so a short title strip can sit inside a taller band.
        const double CROSS_NATURAL = CROSS_OF(s);
        const double CROSS_SIZE    = (isFiller && CROSS_NATURAL <= 0) ? (HORIZONTAL ? AREA.h : AREA.w) : CROSS_NATURAL;

        // Main-axis margins of a FIXED item are real space. The LEADING side becomes an
        // inset on the item; BOTH sides advance the cursor, otherwise `margin.right`
        // was silently ignored and the next item butted against this one.
        const double LEAD  = HORIZONTAL ? s.marginLeft : s.marginTop;
        const double TRAIL = HORIZONTAL ? s.marginRight : s.marginBottom;

        if (s.align == eAlign::End && !isFiller)
        {
            // Stacked flow: the first end item meets the edge and each further one is
            // pushed inwards by a gap. When `stackEnd` is off, every end item is anchored
            // to the edge independently, which is the pre-C3 element behaviour and is
            // kept as the default so existing configs do not move.
            if (m.stackEnd)
            {
                if (ENDPLACED > 0)
                    cursorEnd -= m.gap;
                cursorEnd -= TRAIL + extent;
                mainPos = cursorEnd;
                cursorEnd -= 0;
            }
            else
            {
                // Anchored to the end edge; its trailing margin moves it inwards.
                mainPos = MAIN_ORIGIN + MAIN - TRAIL - extent;
            }

            ENDPLACED++;
        }
        else if (s.align == eAlign::Centre)
            mainPos = MAIN_ORIGIN + (MAIN - extent) / 2.0;
        else
        {
            // Same on the start side: a gap separates, it does not lead.
            if (STARTPLACED > 0)
                cursorStart += m.gap;
            cursorStart += LEAD;
            mainPos = cursorStart;
            cursorStart += extent + TRAIL + FILL_TRAIL;
            STARTPLACED++;
        }

        // Leading margin already applied above; only the user nudge remains.
        mainPos += HORIZONTAL ? s.offsetX : s.offsetY;

        // Cross axis: centred by default, or placed by cross_align inside the area minus
        // this item's cross margins.
        double crossAvail = (HORIZONTAL ? AREA.h : AREA.w);
        const double MARGIN_A = HORIZONTAL ? s.marginTop : s.marginLeft;
        const double MARGIN_B = HORIZONTAL ? s.marginBottom : s.marginRight;
        crossAvail -= MARGIN_A + MARGIN_B;

        const double CROSS_CLAMPED = std::min(CROSS_SIZE, std::max(0.0, crossAvail));

        double crossPos = HORIZONTAL ? AREA.y : AREA.x;
        crossPos += MARGIN_A;
        if (s.crossAlign == eAlign::Centre)
            crossPos += (crossAvail - CROSS_CLAMPED) / 2.0;
        else if (s.crossAlign == eAlign::End)
            crossPos += crossAvail - CROSS_CLAMPED;

        // The cross-axis offset was never applied: in a row, `offset_y` was dead (and in
        // a column, `offset_x`), so an element could not be nudged across the band.
        crossPos += HORIZONTAL ? s.offsetY : s.offsetX;

        SResolvedItem item;
        item.id       = s.id;
        item.kind     = s.kind;
        item.region   = region;
        item.artScale = s.scale;

        if (HORIZONTAL)
            item.rect = {mainPos, crossPos, extent, CROSS_CLAMPED};
        else
            item.rect = {crossPos, mainPos, CROSS_CLAMPED, extent};

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

            item.draw.w     = NinePatch::layoutWidth(G);
            item.draw.h     = NinePatch::layoutHeight(G);
            item.overflowed = G.degraded;
        }

        PLACED[&s] = item;
    }

    // Emit in the caller's list order, not placement order: ids are element indices and
    // consumers walk them positionally.
    for (const auto &s : specs)
    {
        const auto IT = PLACED.find(&s);
        if (IT != PLACED.end())
            out.push_back(IT->second);
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

        for (size_t k = 0; k < result.items.size(); ++k)
            if (result.items[k].kind == eItemKind::Title && result.titleIndex < 0)
                result.titleIndex = (int)k;
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
