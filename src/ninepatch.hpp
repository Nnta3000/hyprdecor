#pragma once

// Nine-patch parsing and composition.
//
// This header is deliberately free of Hyprland dependencies so it can be compiled
// and tested standalone (see tests/). Anything that needs Hyprland types belongs in
// util.hpp instead.

#include <cairo/cairo.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

struct SNinePatchInfo
{
    float border[4] = {0, 0, 0, 0};  // L, T, R, B
    float padding[4] = {0, 0, 0, 0}; // L, T, R, B (content)
    bool defined = false;
};

namespace NinePatch {
    // Reads the 1px marker border, fills `info`, and returns the image with the
    // marker border cropped off. Returns nullptr if the file cannot be read.
    // When `info` is null no metadata is parsed and the raw image is returned.
    inline cairo_surface_t *load(const std::string &path, SNinePatchInfo *info = nullptr)
    {
        if (path.empty())
            return nullptr;

        cairo_surface_t *raw = cairo_image_surface_create_from_png(path.c_str());
        if (cairo_surface_status(raw) != CAIRO_STATUS_SUCCESS)
        {
            cairo_surface_destroy(raw);
            return nullptr;
        }

        if (!info)
            return raw;

        const int W = cairo_image_surface_get_width(raw);
        const int H = cairo_image_surface_get_height(raw);

        // Too small to carry a 1px marker border on all four sides.
        if (W < 3 || H < 3)
            return raw;

        cairo_surface_flush(raw);
        const auto *DATA = (const uint32_t *)cairo_image_surface_get_data(raw);
        const int STRIDE = cairo_image_surface_get_stride(raw) / 4;

        // Markers are opaque black. Cairo ARGB32 is premultiplied, so opaque black
        // is simply 0xFF000000.
        const auto ISBLACK = [&](int x, int y) { return DATA[y * STRIDE + x] == 0xFF000000; };

        // [start, end] of the marker run, or {-1,-1}.
        const auto RUN = [](int lo, int hi, auto &&test) {
            int first = -1, last = -1;
            for (int i = lo; i < hi; ++i)
                if (test(i))
                {
                    if (first == -1)
                        first = i;
                    last = i;
                }
            return std::pair<int, int>{first, last};
        };

        // The marker border occupies index 0 / W-1 / 0 / H-1, and the content runs
        // 1 .. W-2. The stretchable/padding spans are measured *inside* the content,
        // so an offset of -1 converts a marker index into a content-relative value.
        const auto [TOPX0, TOPX1] = RUN(1, W - 1, [&](int x) { return ISBLACK(x, 0); });
        if (TOPX0 != -1)
        {
            info->border[0] = (float)(TOPX0 - 1);
            info->border[2] = (float)((W - 2) - TOPX1);
        }

        const auto [LEFTY0, LEFTY1] = RUN(1, H - 1, [&](int y) { return ISBLACK(0, y); });
        if (LEFTY0 != -1)
        {
            info->border[1] = (float)(LEFTY0 - 1);
            info->border[3] = (float)((H - 2) - LEFTY1);
        }

        const auto [BOTX0, BOTX1] = RUN(1, W - 1, [&](int x) { return ISBLACK(x, H - 1); });
        if (BOTX0 != -1)
        {
            info->padding[0] = (float)(BOTX0 - 1);
            info->padding[2] = (float)((W - 2) - BOTX1);
        }

        const auto [RIGHTY0, RIGHTY1] = RUN(1, H - 1, [&](int y) { return ISBLACK(W - 1, y); });
        if (RIGHTY0 != -1)
        {
            info->padding[1] = (float)(RIGHTY0 - 1);
            info->padding[3] = (float)((H - 2) - RIGHTY1);
        }

        info->defined = true;

        cairo_surface_t *cropped = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W - 2, H - 2);
        cairo_t *cr = cairo_create(cropped);
        cairo_set_source_surface(cr, raw, -1, -1);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(raw);

        return cropped;
    }

    // Source/destination cells for a 3x3 grid.
    struct SLayout
    {
        double sx[4];
        double sy[4];
        double dx[4];
        double dy[4];
        // True when the fixed corners do not even fit in the destination: the frame
        // cannot be drawn faithfully and must be scaled as a whole instead.
        bool degraded = false;
    };

    // Computes the grid.
    //
    // INVARIANT: a source pixel is always `scale` destination pixels. The fixed corners
    // are therefore ALWAYS border * scale and are never resized to fit; only the middle
    // band varies. `maxMiddleW/H` cap that band, in source pixels:
    //
    //   -1 or less : no cap, the middle fills whatever space is available
    //    0         : no middle at all, only the fixed corners/edges are drawn
    //    N         : the middle may grow to N * scale, and no further
    //
    // `destW/destH` is the space available; the returned grid may be smaller than it
    // when a cap applies. Callers must draw at the returned size, not at dest.
    inline SLayout layout(const SNinePatchInfo &info, double srcW, double srcH, double destW, double destH, double scale,
                          double maxMiddleW = -1, double maxMiddleH = -1)
    {
        SLayout l{};

        double bl = info.border[0], bt = info.border[1], br = info.border[2], bb = info.border[3];

        // Markers that exceed the source are the author's error; clamp the source spans.
        if (bl + br > srcW)
        {
            const double T = srcW > 0 ? srcW / (bl + br) : 0;
            bl *= T;
            br *= T;
        }
        if (bt + bb > srcH)
        {
            const double T = srcH > 0 ? srcH / (bt + bb) : 0;
            bt *= T;
            bb *= T;
        }

        l.sx[0] = 0;
        l.sx[1] = bl;
        l.sx[2] = srcW - br;
        l.sx[3] = srcW;

        l.sy[0] = 0;
        l.sy[1] = bt;
        l.sy[2] = srcH - bb;
        l.sy[3] = srcH;

        // Fixed parts: never resized.
        const double DL = bl * scale, DR = br * scale;
        const double DT = bt * scale, DB = bb * scale;

        // Middle: the only stretchable part, capped in SOURCE pixels.
        double middleW = std::max(0.0, destW - DL - DR);
        double middleH = std::max(0.0, destH - DT - DB);

        if (maxMiddleW >= 0)
            middleW = std::min(middleW, maxMiddleW * scale);
        if (maxMiddleH >= 0)
            middleH = std::min(middleH, maxMiddleH * scale);

        // Degraded is now purely informational: the destination is smaller than the
        // fixed art, so the frame overflows and the caller should clip it. We no longer
        // rescale, because rescaling would change the pixel size.
        l.degraded = (destW < DL + DR) || (destH < DT + DB);

        l.dx[0] = 0;
        l.dx[1] = DL;
        l.dx[2] = DL + middleW;
        l.dx[3] = DL + middleW + DR;

        l.dy[0] = 0;
        l.dy[1] = DT;
        l.dy[2] = DT + middleH;
        l.dy[3] = DT + middleH + DB;

        return l;
    }

    // Total size the grid will occupy, which may be smaller than the destination when a
    // cap applies and larger when the fixed art alone exceeds it.
    inline double layoutWidth(const SLayout &l) { return l.dx[3]; }
    inline double layoutHeight(const SLayout &l) { return l.dy[3]; }

    // True when the destination is too small for the frame to be drawn faithfully.
    inline bool needsDegradedScale(const SNinePatchInfo &info, double destW, double destH, double scale)
    {
        return (info.border[0] + info.border[2]) * scale > destW || (info.border[1] + info.border[3]) * scale > destH;
    }

    // Resampling used when the art is stretched. This is where blur actually happens:
    // the frame is composed into a canvas already scaled to device pixels, so Cairo
    // resamples the source art long before OpenGL samples the finished texture. A
    // GL_NEAREST texture filter therefore cannot make already-blurred pixels sharp.
    enum class eFilter : uint8_t {
        Nearest = 0, // pixel art: hard edges
        Bilinear,    // smooth
    };

    inline cairo_filter_t toCairoFilter(eFilter f)
    {
        return f == eFilter::Nearest ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_BILINEAR;
    }

    // Draws a source region into a destination region, scaling to fit.
    inline void drawSized(cairo_t *cr, cairo_surface_t *surface, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh,
                          eFilter filter = eFilter::Bilinear)
    {
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
            return;

        cairo_save(cr);
        cairo_rectangle(cr, dx, dy, dw, dh);
        cairo_clip(cr);
        cairo_translate(cr, dx, dy);
        cairo_scale(cr, dw / sw, dh / sh);

        // The filter must be set AFTER the scale. Cairo bakes the sampling transform
        // into the pattern when it is created, so setting it before cairo_scale() has
        // no effect (verified: bilinear produced 0 blended pixels in that order).
        cairo_set_source_surface(cr, surface, -sx, -sy);
        cairo_pattern_set_filter(cairo_get_source(cr), toCairoFilter(filter));

        cairo_paint(cr);
        cairo_restore(cr);
    }

    // Draws a source region tiled into a destination region.
    inline void drawRepeated(cairo_t *cr, cairo_surface_t *surface, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh,
                             eFilter filter = eFilter::Bilinear)
    {
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
            return;

        cairo_save(cr);
        cairo_rectangle(cr, dx, dy, dw, dh);
        cairo_clip(cr);

        cairo_surface_t *tile = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)sw, (int)sh);
        cairo_t *tcr = cairo_create(tile);
        cairo_set_source_surface(tcr, surface, -sx, -sy);
        cairo_paint(tcr);
        cairo_destroy(tcr);

        // Translate first: the pattern bakes in the transform at creation time, the
        // same ordering constraint as drawSized().
        cairo_translate(cr, dx, dy);

        cairo_pattern_t *pat = cairo_pattern_create_for_surface(tile);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
        cairo_pattern_set_filter(pat, toCairoFilter(filter));
        cairo_set_source(cr, pat);
        cairo_paint(cr);

        cairo_pattern_destroy(pat);
        cairo_surface_destroy(tile);
        cairo_restore(cr);
    }
}
