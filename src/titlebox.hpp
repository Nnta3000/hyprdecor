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

// Fill mode. `leftEdge`/`rightEdge` are the band-local extents already pushed in by the
// neighbouring elements (x from the left, x from the left for both). Returns an empty
// rectangle when the neighbours touch or cross, so nothing is drawn.
inline SRect layoutFill(double bandX, double bandY, [[maybe_unused]] double bandW, double bandH, double leftEdge, double rightEdge,
                        double gapTop, double gapBottom)
{
    const double AVAILABLE = rightEdge - leftEdge;
    if (AVAILABLE <= 0)
        return {bandX + leftEdge, bandY, 0, 0};

    const double H = bandH - (gapTop + gapBottom);
    if (H <= 0)
        return {bandX, bandY, 0, 0};

    return {bandX + leftEdge, bandY + gapTop, AVAILABLE, H};
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
