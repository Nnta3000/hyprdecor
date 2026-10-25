// Title box placement, with no Hyprland dependency so it can be tested standalone.
//
// The title has two explicit modes, mirroring the way elements are positioned:
//
//   Fill : the box is whatever the left- and right-anchored elements leave between
//          them, with `gap` as the distance from those elements.
//   Box  : placed like an element, anchor + offset, then sized by the stretch band.
//
// Keeping this arithmetic out of the renderer means the two modes can be tested
// without a GPU, and the renderer only ever sees a rectangle.

#pragma once

#include <algorithm>
#include <cmath>

namespace TitleBox
{

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

struct SRect
{
    double x = 0, y = 0, w = 0, h = 0;

    bool drawable() const { return w >= 1 && h >= 1; }
};

enum class eCross : uint8_t {
    Start,
    Centre,
    End,
};

// Fill mode, main axis. `leftEdge`/`rightEdge` are the band-local extents already pushed
// in by the neighbouring elements. Returns an empty rectangle when they touch or cross,
// so nothing is drawn.
//
// There is deliberately no vertical gap parameter. A gap needs two things to sit
// BETWEEN, and vertically the box's neighbours are the band's own edges, so "gap.bottom"
// could only ever mean "resize", while "gap.top" meant "move". Vertical placement of a
// short box is `height` + `cross_align` + `margin` instead (see layoutCross).
inline SRect layoutFill(double bandX, double bandY, [[maybe_unused]] double bandW, double bandH, double leftEdge, double rightEdge)
{
    const double AVAILABLE = rightEdge - leftEdge;
    if (AVAILABLE <= 0)
        return {bandX + leftEdge, bandY, 0, 0};

    if (bandH <= 0)
        return {bandX, bandY, 0, 0};

    return {bandX + leftEdge, bandY, AVAILABLE, bandH};
}

// Cross axis: where a box of height `boxH` sits inside `bandH`, honouring the margins.
// A box at least as tall as the band is clamped to it, so margins can never push the
// title outside the band.
inline SRect layoutCross(double bandY, double bandH, double boxH, eCross cross, double marginTop, double marginBottom)
{
    const double AVAILABLE = std::max(0.0, bandH - marginTop - marginBottom);
    const double H         = std::min(boxH > 0 ? boxH : AVAILABLE, AVAILABLE);

    if (H <= 0)
        return {0, bandY, 0, 0};

    double y = bandY + marginTop;
    if (cross == eCross::Centre)
        y = bandY + marginTop + (AVAILABLE - H) / 2.0;
    else if (cross == eCross::End)
        y = bandY + bandH - marginBottom - H;

    return {0, std::round(y), 0, std::round(H)};
}

// Box mode: anchor picks a corner/edge of the band, `offset` moves from it.
inline SRect layoutPlaced(double bandX, double bandY, double bandW, double bandH, double w, double h, eAnchor anchor, double offsetX,
                          double offsetY)
{
    double x = bandX + offsetX;
    double y = bandY + offsetY;

    switch (anchor)
    {
        case eAnchor::TopCentre:
        case eAnchor::Centre:
        case eAnchor::BottomCentre: x = bandX + (bandW - w) / 2.0 + offsetX; break;
        case eAnchor::TopRight:
        case eAnchor::CentreRight:
        case eAnchor::BottomRight: x = bandX + bandW - w + offsetX; break;
        default: break;
    }

    switch (anchor)
    {
        case eAnchor::CentreLeft:
        case eAnchor::Centre:
        case eAnchor::CentreRight: y = bandY + (bandH - h) / 2.0 + offsetY; break;
        case eAnchor::BottomLeft:
        case eAnchor::BottomCentre:
        case eAnchor::BottomRight: y = bandY + bandH - h + offsetY; break;
        default: break;
    }

    return {std::round(x), std::round(y), std::max(0.0, std::round(w)), std::max(0.0, std::round(h))};
}

// Is a point inside an element's box? Shared by drawing and hit-testing so a pointer
// can never hit an element somewhere it is not drawn. A minimum size of one pixel keeps
// zero-sized or degenerate elements clickable instead of silently impossible to reach.
inline bool hit(const SRect &r, double x, double y)
{
    return x >= r.x && x < r.x + std::max(1.0, r.w) && y >= r.y && y < r.y + std::max(1.0, r.h);
}

// Horizontal alignment of the text inside a box placed with `anchor`, so that
// anchor = "centre" centres the text too, instead of silently leaving it left.
inline double textAlignFor(eAnchor anchor)
{
    switch (anchor)
    {
        case eAnchor::TopRight:
        case eAnchor::CentreRight:
        case eAnchor::BottomRight: return 1.0;
        case eAnchor::TopCentre:
        case eAnchor::Centre:
        case eAnchor::BottomCentre: return 0.5;
        default: return 0.0;
    }
}

}
