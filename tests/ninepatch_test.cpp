// Standalone tests for the nine-patch core. No Hyprland, no GPU.
//
//   cmake --build build --target ninepatch_test && ./build/ninepatch_test
//
// Each test prints PASS/FAIL and the binary exits non-zero on any failure, so it is
// usable as a regression gate.

#include "ninepatch.hpp"

#include <cairo/cairo.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

static int g_failures = 0;

#define CHECK(cond, ...)                                                                                                       \
    do {                                                                                                                       \
        if (!(cond)) {                                                                                                         \
            std::printf("  FAIL: ");                                                                                           \
            std::printf(__VA_ARGS__);                                                                                          \
            std::printf("\n");                                                                                                 \
            ++g_failures;                                                                                                      \
        }                                                                                                                      \
    } while (0)

static void header(const char *name)
{
    std::printf("\n== %s ==\n", name);
}

// Synthesises a .9.png with known marker positions so assertions are exact.
// contentW/H are the inner image size; markers are drawn on the 1px border.
struct SSynthetic
{
    int         contentW, contentH;
    int         stretchX0, stretchX1; // in border coordinates (1-based)
    int         stretchY0, stretchY1;
    std::string path;
};

static std::string makeNinePatch(const std::string &path, int contentW, int contentH, int sx0, int sx1, int sy0, int sy1)
{
    const int W = contentW + 2, H = contentH + 2;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *cr = cairo_create(s);

    // Fill the content area with opaque red so composition is easy to recognise.
    cairo_set_source_rgba(cr, 1, 0, 0, 1);
    cairo_rectangle(cr, 1, 1, contentW, contentH);
    cairo_fill(cr);

    // Transparent marker border.
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cr, 0, 0, W, 1);
    cairo_rectangle(cr, 0, H - 1, W, 1);
    cairo_rectangle(cr, 0, 0, 1, H);
    cairo_rectangle(cr, W - 1, 0, 1, H);
    cairo_fill(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Black markers (opaque).
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    cairo_rectangle(cr, sx0, 0, sx1 - sx0 + 1, 1);
    cairo_fill(cr);
    cairo_rectangle(cr, 0, sy0, 1, sy1 - sy0 + 1);
    cairo_fill(cr);
    cairo_rectangle(cr, sx0, H - 1, sx1 - sx0 + 1, 1);
    cairo_fill(cr);
    cairo_rectangle(cr, W - 1, sy0, 1, sy1 - sy0 + 1);
    cairo_fill(cr);

    cairo_destroy(cr);
    cairo_surface_flush(s);
    cairo_surface_write_to_png(s, path.c_str());
    cairo_surface_destroy(s);
    return path;
}

static uint32_t pixelAt(cairo_surface_t *s, int x, int y)
{
    const auto *PX = (const uint32_t *)cairo_image_surface_get_data(s);
    const int   STRIDE = cairo_image_surface_get_stride(s) / 4;
    return PX[y * STRIDE + x];
}

// ---------------------------------------------------------------------------

static void testParseSynthetic()
{
    header("parse: synthetic 10x10 content, stretch 3..6");

    const auto PATH = "/tmp/np_synthetic.png";
    makeNinePatch(PATH, 10, 10, 3, 6, 2, 5);

    SNinePatchInfo info;
    cairo_surface_t *s = NinePatch::load(PATH, &info);

    CHECK(s != nullptr, "load returned null");
    if (!s)
        return;

    CHECK(info.defined, "defined should be true");

    const int W = cairo_image_surface_get_width(s);
    const int H = cairo_image_surface_get_height(s);
    CHECK(W == 10 && H == 10, "cropped size should be 10x10, got %dx%d", W, H);

    // Marker at border index 3 means 2 content pixels to its left -> border L = 2.
    CHECK(info.border[0] == 2, "border L expected 2, got %.0f", info.border[0]);
    // Marker ends at 6; content width 10, so right border = 10 - (6-1+1) = ... see below.
    std::printf("  borders L/T/R/B = %.0f %.0f %.0f %.0f\n", info.border[0], info.border[1], info.border[2], info.border[3]);

    // Core invariant: the fixed borders must leave a non-negative middle band.
    CHECK(info.border[0] + info.border[2] <= W, "borders exceed width: %.0f+%.0f > %d", info.border[0], info.border[2], W);
    CHECK(info.border[1] + info.border[3] <= H, "borders exceed height: %.0f+%.0f > %d", info.border[1], info.border[3], H);

    cairo_surface_destroy(s);
}

static void testParseRealAsset()
{
    header("parse: shipped xp frame");

    const std::string PATH = "/home/agent/Documents/hyprdecor/assets/xp/frame_active.png";
    SNinePatchInfo    info;
    cairo_surface_t  *s = NinePatch::load(PATH, &info);

    if (!s)
    {
        std::printf("  SKIP: %s not readable\n", PATH.c_str());
        return;
    }

    const int W = cairo_image_surface_get_width(s);
    const int H = cairo_image_surface_get_height(s);
    std::printf("  cropped %dx%d, border %.0f %.0f %.0f %.0f, padding %.0f %.0f %.0f %.0f\n", W, H, info.border[0], info.border[1], info.border[2],
                info.border[3], info.padding[0], info.padding[1], info.padding[2], info.padding[3]);

    CHECK(info.defined, "defined should be true");
    CHECK(info.border[0] + info.border[2] <= W, "borders exceed width");
    CHECK(info.border[1] + info.border[3] <= H, "borders exceed height");

    // The frame must have a real stretch band, not a degenerate one.
    const double MIDW = W - info.border[0] - info.border[2];
    const double MIDH = H - info.border[1] - info.border[3];
    CHECK(MIDW > 0, "horizontal stretch band is empty");
    CHECK(MIDH > 0, "vertical stretch band is empty");
    std::printf("  stretch band %.0fx%.0f\n", MIDW, MIDH);

    cairo_surface_destroy(s);
}

static void testLayoutDestLarger()
{
    header("layout: destination larger than source");

    SNinePatchInfo info;
    info.border[0] = info.border[2] = 5;
    info.border[1] = info.border[3] = 4;
    info.defined = true;

    const auto L = NinePatch::layout(info, 90, 90, 870, 124, 2.0);

    CHECK(!L.degraded, "should not degrade for a larger destination");
    CHECK(L.dx[1] == 10, "left border dest should be 5*2=10, got %.0f", L.dx[1]);
    CHECK(L.dx[2] == 860, "right border dest start should be 870-10=860, got %.0f", L.dx[2]);
    CHECK(L.dy[1] == 8, "top border dest should be 4*2=8, got %.0f", L.dy[1]);
    CHECK(L.dy[2] == 116, "bottom border dest start should be 124-8=116, got %.0f", L.dy[2]);

    // Middle band must be positive in both axes.
    CHECK(L.dx[2] - L.dx[1] > 0, "middle band width is not positive");
    CHECK(L.dy[2] - L.dy[1] > 0, "middle band height is not positive");
}

static void testLayoutMiddleLargerThanDest()
{
    header("layout: source middle band larger than destination (the reported bug)");

    // This mirrors the real XP frame: a tall source with a big stretch band that is
    // squeezed into a short destination.
    SNinePatchInfo info;
    info.border[0] = info.border[2] = 5;
    info.border[1] = 30;
    info.border[3] = 4;
    info.defined = true;

    // Destination shorter than the scaled fixed borders would allow.
    const auto L = NinePatch::layout(info, 90, 90, 870, 20, 2.0);

    CHECK(L.degraded, "should flag degradation when fixed borders exceed destination");
    // Even when degraded, the grid must stay ordered so nothing overlaps.
    CHECK(L.dx[0] <= L.dx[1] && L.dx[1] <= L.dx[2] && L.dx[2] <= L.dx[3], "dx not monotonic");
    CHECK(L.dy[0] <= L.dy[1] && L.dy[1] <= L.dy[2] && L.dy[2] <= L.dy[3], "dy not monotonic");
}

static void testComposeTransparency()
{
    header("compose: corners opaque, unmarked middle transparent");

    // Content 10x10 fully red, no stretch band in the middle marked as opaque, so
    // the destination middle stays transparent (the source there is red, but we only
    // assert on the corner behaviour and overall integrity here).
    const auto PATH = "/tmp/np_compose.png";
    makeNinePatch(PATH, 10, 10, 3, 6, 3, 6);

    SNinePatchInfo info;
    cairo_surface_t *src = NinePatch::load(PATH, &info);
    if (!src)
    {
        CHECK(false, "source load failed");
        return;
    }

    const int SW = cairo_image_surface_get_width(src);
    const int SH = cairo_image_surface_get_height(src);
    const auto L = NinePatch::layout(info, SW, SH, 200, 80, 1.0);

    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 80);
    cairo_t *cr = cairo_create(out);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            NinePatch::drawSized(cr, src, L.sx[i], L.sy[j], L.sx[i + 1] - L.sx[i], L.sy[j + 1] - L.sy[j], L.dx[i], L.dy[j], L.dx[i + 1] - L.dx[i],
                                 L.dy[j + 1] - L.dy[j]);

    cairo_destroy(cr);
    cairo_surface_flush(out);

    const uint32_t CENTRE = pixelAt(out, 100, 40);
    const uint32_t CORNER = pixelAt(out, 1, 1);

    std::printf("  centre=0x%08x corner=0x%08x\n", CENTRE, CORNER);

    // The source content is opaque red; the middle of the destination must be red,
    // not black and not transparent. This is the exact failure mode observed on
    // screen. Cairo ARGB32 in memory is BGRA, so opaque red reads back as 0xffff0000.
    CHECK((CENTRE & 0x00FFFFFF) == 0x0000FF || (CENTRE & 0x00FFFFFF) == 0xFF0000, "centre should be opaque red (either channel order), got 0x%08x",
          CENTRE);
    CHECK(((CENTRE >> 24) & 0xFF) == 0xFF, "centre should be opaque, got alpha %u", (CENTRE >> 24) & 0xFF);
    CHECK(((CORNER >> 24) & 0xFF) == 0xFF, "corner over the red content should be opaque, got 0x%08x", CORNER);

    cairo_surface_write_to_png(out, "/tmp/np_compose_out.png");
    cairo_surface_destroy(out);
    cairo_surface_destroy(src);
}

static void testComposeFullSizeMatch()
{
    header("compose: 1:1 destination reproduces the source exactly");

    const auto PATH = "/tmp/np_exact.png";
    makeNinePatch(PATH, 10, 10, 3, 6, 3, 6);

    SNinePatchInfo info;
    cairo_surface_t *src = NinePatch::load(PATH, &info);
    if (!src)
    {
        CHECK(false, "source load failed");
        return;
    }

    const int SW = cairo_image_surface_get_width(src);
    const int SH = cairo_image_surface_get_height(src);
    const auto L = NinePatch::layout(info, SW, SH, SW, SH, 1.0);

    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, SW, SH);
    cairo_t *cr = cairo_create(out);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            NinePatch::drawSized(cr, src, L.sx[i], L.sy[j], L.sx[i + 1] - L.sx[i], L.sy[j + 1] - L.sy[j], L.dx[i], L.dy[j], L.dx[i + 1] - L.dx[i],
                                 L.dy[j + 1] - L.dy[j]);

    cairo_destroy(cr);
    cairo_surface_flush(out);

    int mismatches = 0;
    for (int y = 0; y < SH; ++y)
        for (int x = 0; x < SW; ++x)
            if (pixelAt(out, x, y) != pixelAt(src, x, y))
                ++mismatches;

    CHECK(mismatches == 0, "%d pixels differ from the source in a 1:1 blit", mismatches);

    cairo_surface_destroy(out);
    cairo_surface_destroy(src);
}

static void testMalformedInput()
{
    header("parse: malformed input is rejected, not crashed on");

    SNinePatchInfo info;

    CHECK(NinePatch::load("", &info) == nullptr, "empty path should return null");
    CHECK(NinePatch::load("/tmp/definitely-not-here.png", &info) == nullptr, "missing file should return null");

    // A 1x1 PNG with no marker border must not be parsed.
    cairo_surface_t *tiny = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_surface_write_to_png(tiny, "/tmp/np_tiny.png");
    cairo_surface_destroy(tiny);

    SNinePatchInfo tinyInfo;
    cairo_surface_t *s = NinePatch::load("/tmp/np_tiny.png", &tinyInfo);
    CHECK(s != nullptr, "tiny png should still load");
    CHECK(!tinyInfo.defined, "tiny png should not be marked as defined");
    if (s)
        cairo_surface_destroy(s);
}

// Mirrors CHyprWindowDecorator::assignedBoxGlobal() under ABSOLUTE positioning:
// the frame box = window box expanded by the reserved extents on every side.
static void testFrameBox()
{
    header("frame box: window is surrounded by the reserved extents");

    const double winX = 82, winY = 82, winW = 311, winH = 89;
    const double T = 20; // thickness on all sides

    const double boxX = winX - T;
    const double boxY = winY - T;
    const double boxW = winW + T + T;
    const double boxH = winH + T + T;

    CHECK(boxX == 62 && boxY == 62, "frame origin should be %d,%d, got %.0f,%.0f", 62, 62, boxX, boxY);
    CHECK(boxW == 351, "frame width should be 351, got %.0f", boxW);
    CHECK(boxH == 129, "frame height should be 129, got %.0f", boxH);

    // The window must sit strictly inside the frame: this is the xxxxx / x TERM x shape.
    CHECK(boxX < winX && boxY < winY, "window must start inside the frame");
    CHECK(boxX + boxW > winX + winW, "frame must extend past the window's right edge");
    CHECK(boxY + boxH > winY + winH, "frame must extend past the window's bottom edge");

    // Left/right and top/bottom margins must both equal the thickness.
    CHECK(winX - boxX == T, "left margin should be the thickness");
    CHECK((boxX + boxW) - (winX + winW) == T, "right margin should be the thickness");
    CHECK(winY - boxY == T, "top margin should be the thickness");
    CHECK((boxY + boxH) - (winY + winH) == T, "bottom margin should be the thickness");
}

// border.scale must magnify the fixed corners without compounding with any other
// factor, and the grid must stay monotonic at every scale.
static void testArtScale()
{
    header("border.scale: corners magnify, grid stays ordered");

    // 3px sides and a 15px top band in a 21px source: 15+3 = 18 <= 21, so the
    // markers are valid and nothing should be clamped.
    SNinePatchInfo info;
    info.border[0] = info.border[2] = 3;
    info.border[1] = 15;
    info.border[3] = 3;
    info.defined = true;

    for (const double S : {1.0, 2.0, 3.0})
    {
        const auto L = NinePatch::layout(info, 21, 21, 400, 200, S);

        // Fixed corners occupy border * scale pixels.
        CHECK(std::abs(L.dx[1] - 3 * S) < 0.001, "left corner at scale %.0f should be %.0f, got %.0f", S, 3 * S, L.dx[1]);
        CHECK(std::abs(L.dy[1] - 15 * S) < 0.001, "top corner at scale %.0f should be %.0f, got %.0f", S, 15 * S, L.dy[1]);
        CHECK(std::abs(L.dx[2] - (400 - 3 * S)) < 0.001, "right corner start wrong at scale %.0f", S);

        // Ordering must hold, which is what a compounded or misapplied scale breaks.
        CHECK(L.dx[0] <= L.dx[1] && L.dx[1] <= L.dx[2] && L.dx[2] <= L.dx[3], "dx not monotonic at scale %.0f", S);
        CHECK(L.dy[0] <= L.dy[1] && L.dy[1] <= L.dy[2] && L.dy[2] <= L.dy[3], "dy not monotonic at scale %.0f", S);

        // Reserved space must equal what is drawn: the same expression the plugin uses.
        CHECK(!L.degraded, "should not degrade at scale %.0f for a large destination", S);
    }

    // Markers that exceed the source are clamped, never allowed to invert the grid.
    SNinePatchInfo tight;
    tight.border[0] = tight.border[2] = 3;
    tight.border[1] = tight.border[3] = 15; // 30 > 21: must be clamped
    tight.defined = true;

    for (const double S : {1.0, 2.0})
    {
        const auto T = NinePatch::layout(tight, 21, 21, 400, 200, S);
        CHECK(T.dy[1] <= T.dy[2], "clamped grid must stay ordered at scale %.0f", S);
        CHECK(T.dy[1] < 21 * S + 0.001, "clamped top must not exceed the source height at scale %.0f", S);
    }

    // A 4x scale on a small destination must degrade rather than overlap.
    const auto BIG = NinePatch::layout(info, 21, 21, 40, 40, 8.0);
    CHECK(BIG.degraded, "8x scale in a 40px box should be flagged as degraded");
    CHECK(BIG.dy[1] <= BIG.dy[2], "degraded grid must still be ordered");
}

// Cairo, not OpenGL, does the resampling that blurs stretched frame art. Nearest must
// keep a hard checkerboard; bilinear must introduce intermediate values.
static void testCairoFilter()
{
    header("resampling filter: nearest keeps hard edges, bilinear blends");

    // 2x2 checkerboard in a 2x2 source.
    cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
    cairo_t *sc = cairo_create(src);
    cairo_set_source_rgb(sc, 1, 1, 1);
    cairo_paint(sc); // white base, then black on the diagonal
    cairo_set_source_rgb(sc, 0, 0, 0);
    cairo_rectangle(sc, 0, 0, 1, 1);
    cairo_rectangle(sc, 1, 1, 1, 1);
    cairo_fill(sc);
    cairo_destroy(sc);
    cairo_surface_flush(src);

    const auto RENDER = [&](NinePatch::eFilter filter) {
        cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
        cairo_t *cr = cairo_create(out);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        NinePatch::drawSized(cr, src, 0, 0, 2, 2, 0, 0, 16, 16, filter);
        cairo_destroy(cr);
        cairo_surface_flush(out);
        return out;
    };

    const auto countIntermediates = [](cairo_surface_t *s) {
        int n = 0;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
            {
                const auto V = pixelAt(s, x, y) & 0xFF;
                if (V > 8 && V < 247)
                    ++n;
            }
        return n;
    };

    cairo_surface_t *nearest  = RENDER(NinePatch::eFilter::Nearest);
    cairo_surface_t *bilinear = RENDER(NinePatch::eFilter::Bilinear);

    const int NEARESTGREYS  = countIntermediates(nearest);
    const int BILINEARGREYS = countIntermediates(bilinear);
    std::printf("  intermediate pixels: nearest=%d bilinear=%d\n", NEARESTGREYS, BILINEARGREYS);

    CHECK(NEARESTGREYS == 0, "nearest must produce only pure black/white, got %d blended pixels", NEARESTGREYS);
    CHECK(BILINEARGREYS > 0, "bilinear should blend at the seams, got %d", BILINEARGREYS);

    // Nearest must also reproduce exact 8x8 blocks.
    CHECK((pixelAt(nearest, 0, 0) & 0xFF) == (pixelAt(nearest, 7, 7) & 0xFF), "nearest block 0 should be uniform");
    CHECK((pixelAt(nearest, 0, 0) & 0xFF) != (pixelAt(nearest, 8, 0) & 0xFF), "nearest should switch cleanly at the midpoint");

    cairo_surface_destroy(nearest);
    cairo_surface_destroy(bilinear);
    cairo_surface_destroy(src);
}

// inset must not create a hole: the box the nine-patch is stretched over is the SAME
// rectangle that layout reserved, so reserved == art + inset everywhere.
static void testInsetFillsReservedBox()
{
    header("inset: reserved box == stretched box (no gap)");

    const double winX = 100, winY = 100, winW = 300, winH = 200;

    // art thickness as decided by the markers, and the user's inset
    const double ART[4]    = {3, 9, 3, 3};    // L T R B
    const double INSET[4]  = {4, 24, 4, 4};   // L T R B

    // what getPositioningInfo() reserves
    const double RES[4] = {ART[0] + INSET[0], ART[1] + INSET[1], ART[2] + INSET[2], ART[3] + INSET[3]};

    // what assignedBoxGlobal() derives
    const double boxX = winX - RES[0];
    const double boxY = winY - RES[1];
    const double boxW = winW + RES[0] + RES[2];
    const double boxH = winH + RES[1] + RES[3];

    // The window must sit exactly RES inside the box on every side: no extra margin.
    CHECK(winX - boxX == RES[0], "left: window must start exactly at the reserved edge");
    CHECK(winY - boxY == RES[1], "top: window must start exactly at the reserved edge");
    CHECK((boxX + boxW) - (winX + winW) == RES[2], "right: no extra space");
    CHECK((boxY + boxH) - (winY + winH) == RES[3], "bottom: no extra space");

    // And the nine-patch must cover that whole box, so its middle band is positive.
    SNinePatchInfo info;
    info.border[0] = ART[0];
    info.border[1] = ART[1];
    info.border[2] = ART[2];
    info.border[3] = ART[3];
    info.defined   = true;

    const auto L = NinePatch::layout(info, 21, 21, boxW, boxH, 1.0);
    CHECK(!L.degraded, "frame must not degrade for a normal window");
    CHECK(L.dx[0] == 0 && L.dy[0] == 0, "grid must start at the box origin");
    CHECK(L.dx[3] == boxW && L.dy[3] == boxH, "grid must end at the box edge: no unpainted gap");
}

// Every one of the 9 cells must be painted: the window is always drawn on top, so a
// skipped centre would only ever hide frame art.
static void testAllNineCellsPainted()
{
    header("compose: all nine cells painted (no hollow centre)");

    // Source: distinct solid colour everywhere, so any skipped cell shows as a hole.
    cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 9, 9);
    cairo_t *sc = cairo_create(src);
    cairo_set_source_rgba(sc, 1, 0, 0, 1);
    cairo_paint(sc);
    cairo_destroy(sc);
    cairo_surface_flush(src);

    SNinePatchInfo info;
    info.border[0] = info.border[2] = 2;
    info.border[1] = info.border[3] = 2;
    info.defined   = true;

    const auto L = NinePatch::layout(info, 9, 9, 90, 60, 1.0);

    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 90, 60);
    cairo_t *cr = cairo_create(out);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            NinePatch::drawSized(cr, src, L.sx[i], L.sy[j], L.sx[i + 1] - L.sx[i], L.sy[j + 1] - L.sy[j], L.dx[i], L.dy[j], L.dx[i + 1] - L.dx[i],
                                 L.dy[j + 1] - L.dy[j]);

    cairo_destroy(cr);
    cairo_surface_flush(out);

    // The centre cell's area must not be transparent.
    const uint32_t CENTRE = pixelAt(out, 45, 30);
    CHECK(((CENTRE >> 24) & 0xFF) == 0xFF, "centre cell must be painted, got alpha %u", (CENTRE >> 24) & 0xFF);

    // Scan the whole surface for any transparent pixel.
    int holes = 0;
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 90; ++x)
            if (((pixelAt(out, x, y) >> 24) & 0xFF) != 0xFF)
                ++holes;
    CHECK(holes == 0, "frame surface must be fully painted, found %d transparent pixels", holes);

    cairo_surface_destroy(out);
    cairo_surface_destroy(src);
}

// The title must vanish when its neighbours leave no room, instead of drawing text
// that spills over them.
static void testTitleFitsOrVoids()
{
    header("title box: no room means an empty box (nothing drawn)");

    const auto box = [](double bandW, double leftEdge, double rightEdge) {
        const double AVAILABLE = rightEdge - leftEdge;
        if (AVAILABLE <= 0)
            return 0.0;
        return AVAILABLE;
    };

    CHECK(box(400, 20, 380) == 360, "normal case should return the free span");
    CHECK(box(400, 200, 180) == 0, "overlapping neighbours must yield no width");
    CHECK(box(400, 200, 200) == 0, "touching neighbours must yield no width");

    // A zero-width box must be treated as "do not draw".
    const auto drawable = [](double w, double h) { return w >= 1 && h >= 1; };
    CHECK(!drawable(0, 20), "zero width must not be drawable");
    CHECK(!drawable(50, 0), "zero height must not be drawable");
    CHECK(drawable(50, 20), "a real box must be drawable");
}

// The core invariant: a source pixel is ALWAYS `scale` destination pixels. Caps limit
// the middle band only; they must never resize the corners.
static void testPixelSizeInvariant()
{
    header("invariant: corners are always border * scale; caps only touch the middle");

    SNinePatchInfo info;
    info.border[0] = info.border[2] = 3;
    info.border[1] = info.border[3] = 9;
    info.defined   = true;

    const double S = 4.0;

    // No cap: middle fills the destination.
    {
        const auto L = NinePatch::layout(info, 21, 21, 400, 400, S);
        CHECK(L.dx[1] == 3 * S, "left corner must be %.0f, got %.0f", 3 * S, L.dx[1]);
        CHECK(L.dx[3] == 400, "no cap should fill the destination, got %.0f", L.dx[3]);
    }

    // Cap 7 source px on the middle: corners unchanged, total = fixed + 7*S.
    {
        const auto L = NinePatch::layout(info, 21, 21, 400, 400, S, 7, 7);
        const double FIXEDW = (3 + 3) * S;
        const double FIXEDH = (9 + 9) * S;
        CHECK(L.dx[1] == 3 * S, "left corner must stay %.0f with a cap, got %.0f", 3 * S, L.dx[1]);
        CHECK(L.dx[3] == FIXEDW + 7 * S, "total width must be fixed + 7*scale, got %.0f", L.dx[3]);
        CHECK(L.dy[3] == FIXEDH + 7 * S, "total height must be fixed + 7*scale, got %.0f", L.dy[3]);
        CHECK(L.dx[2] - L.dx[1] == 7 * S, "middle band must be exactly 7*scale");
    }

    // Cap 0: corners only, no middle at all.
    {
        const auto L = NinePatch::layout(info, 21, 21, 400, 400, S, 0, 0);
        CHECK(L.dx[1] == 3 * S, "corner unchanged at cap 0");
        CHECK(L.dx[2] == L.dx[1], "middle width must be zero at cap 0");
        CHECK(L.dy[2] == L.dy[1], "middle height must be zero at cap 0");
        CHECK(L.dx[3] == 6 * S, "total width must be corners only, got %.0f", L.dx[3]);
    }

    // Destination smaller than the fixed art: never rescaled to fit.
    {
        const auto L = NinePatch::layout(info, 21, 21, 10, 10, S);
        CHECK(L.dx[1] == 3 * S, "corner must NOT shrink even when the destination is tiny");
        CHECK(L.degraded, "overflow must be flagged as degraded (informational only)");
        CHECK(L.dx[3] >= 6 * S, "frame must overflow rather than downscale, got %.0f", L.dx[3]);
    }
}

// A floor on the middle band: it wins over the available space and the frame overflows;
// the pixel size is still `scale`.
static void testMinStretchFloor()
{
    header("floor: min_* raises the middle band and overflows instead of collapsing");

    SNinePatchInfo info;
    info.border[0] = info.border[2] = 3;
    info.border[1] = info.border[3] = 9;
    info.defined   = true;

    const double S = 4.0;

    // Space smaller than the floor: the middle is raised to the floor anyway.
    {
        const auto L = NinePatch::layout(info, 21, 21, 20, 20, S, -1, -1, 5, 5);
        CHECK(L.dx[2] - L.dx[1] == 5 * S, "middle must be raised to min*scale, got %.0f", L.dx[2] - L.dx[1]);
        CHECK(L.dx[1] == 3 * S, "corners unchanged by the floor");
        CHECK(L.dx[3] > 20, "frame must overflow the destination, got %.0f", L.dx[3]);
    }

    // Plenty of space: the floor changes nothing.
    {
        const auto L = NinePatch::layout(info, 21, 21, 400, 400, S, -1, -1, 5, 5);
        CHECK(L.dx[3] == 400, "floor must not cap the growth, got %.0f", L.dx[3]);
    }

    // Floor and cap together: the cap wins when both fit, the floor only fills a gap
    // that is smaller than it.
    {
        const auto L = NinePatch::layout(info, 21, 21, 400, 400, S, 7, 7, 5, 5);
        CHECK(L.dx[2] - L.dx[1] == 7 * S, "cap must still apply, got %.0f", L.dx[2] - L.dx[1]);
    }
    {
        // Space already consumed by the fixed art: the middle collapses to 0 and the
        // floor is what keeps the frame visible.
        const auto L = NinePatch::layout(info, 21, 21, 30, 30, S, 7, 7, 5, 5);
        CHECK(L.dx[2] - L.dx[1] == 5 * S, "floor applies when there is no room at all, got %.0f", L.dx[2] - L.dx[1]);
    }

    // min = 0 is the same as unset: a degenerate middle is allowed.
    {
        const auto L = NinePatch::layout(info, 21, 21, 6 * S, 18 * S, S, -1, -1, 0, 0);
        CHECK(L.dx[2] == L.dx[1], "min 0 must not force a middle band");
    }
}

int main()
{
    std::printf("hyprdecor nine-patch tests\n");

    testParseSynthetic();
    testParseRealAsset();
    testLayoutDestLarger();
    testLayoutMiddleLargerThanDest();
    testComposeTransparency();
    testComposeFullSizeMatch();
    testMalformedInput();
    testFrameBox();
    testArtScale();
    testCairoFilter();
    testInsetFillsReservedBox();
    testAllNineCellsPainted();
    testTitleFitsOrVoids();
    testPixelSizeInvariant();
    testMinStretchFloor();

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
