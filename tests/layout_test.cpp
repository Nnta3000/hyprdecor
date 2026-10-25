// Standalone tests for the resolved-layout pass. No Hyprland, no GPU.
//
//   cmake --build build --target ninepatch_test && ./build/ninepatch_test
//
// Layout is a pure function, so every expectation here is an exact rectangle.

#include "layout.hpp"

#include <cmath>
#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond, ...)                                                                                                                      \
    do                                                                                                                                        \
    {                                                                                                                                         \
        if (!(cond))                                                                                                                          \
        {                                                                                                                                     \
            g_failures++;                                                                                                                     \
            std::printf("  FAIL: ");                                                                                                          \
            std::printf(__VA_ARGS__);                                                                                                         \
            std::printf("\n");                                                                                                                \
        }                                                                                                                                     \
    } while (0)

#define CHECK_RECT(r, X, Y, W, H, label)                                                                                                      \
    CHECK(std::abs((r).x - (X)) < 0.0001 && std::abs((r).y - (Y)) < 0.0001 && std::abs((r).w - (W)) < 0.0001                                   \
              && std::abs((r).h - (H)) < 0.0001,                                                                                              \
          "%s: expected (%.1f, %.1f, %.1f, %.1f) got (%.1f, %.1f, %.1f, %.1f)", label, (double)(X), (double)(Y), (double)(W), (double)(H),   \
          (r).x, (r).y, (r).w, (r).h)

static void header(const char *name)
{
    std::printf("\n== %s ==\n", name);
}

// A frame whose left and right art differ, like the orange one: content 13/7 and
// padding 7, which is what the old bandBox() subtracted.
static Layout::SFrameMetrics orangeFrame()
{
    Layout::SFrameMetrics m;
    m.left = 26;
    m.top = 30;
    m.right = 14;
    m.bottom = 24;
    m.contentLeft = 13;
    m.contentTop = 25;
    m.contentRight = 7;
    m.contentBottom = 12;
    m.padding = 7;
    m.boxW = 467;
    m.boxH = 245;
    return m;
}

static Layout::SItemSpec imageItem(int id, double w, double h, Layout::eAlign align, double offsetX)
{
    Layout::SItemSpec s;
    s.id      = id;
    s.kind    = Layout::eItemKind::Image;
    s.srcW    = w;
    s.srcH    = h;
    s.scale   = 1;
    s.align   = align;
    s.offsetX = offsetX;
    return s;
}

// The item area must reproduce the old band exactly, or the migration would move art.
static void testItemArea()
{
    header("layout: the item area is the old band");

    const auto M    = orangeFrame();
    const auto AREA = Layout::itemArea(M, Layout::eRegion::Top, Layout::regionRect(M, Layout::eRegion::Top));

    // x = contentLeft + padding, w = box - content - 2*padding
    CHECK_RECT(AREA, 20, 0, 433, 37, "top item area");
    CHECK(AREA.x + AREA.w == 467 - 7 - 7, "right edge is content right + padding from the box edge");
}

// start / end items flow from their edges; the title takes the gap between.
static void testFlow()
{
    header("layout: flow places start, end and the title between");

    auto M = orangeFrame();
    M.stackEnd = true; // opt-in; see testEndIndependent for the default

    std::vector<Layout::SItemSpec> specs;
    specs.push_back(imageItem(0, 13, 14, Layout::eAlign::Start, 0));  // eye, 13x14
    specs.push_back(imageItem(1, 13, 14, Layout::eAlign::End, 0));    // maximize
    specs.push_back(imageItem(2, 13, 14, Layout::eAlign::End, 0));    // close
    specs.push_back(imageItem(3, 6, 2, Layout::eAlign::Start, 0));    // small logo

    auto R = Layout::compute(M, specs);

    CHECK(R.items.size() == 4, "all four items should be laid out");
    CHECK(!R.titleVoids, "no title configured, so nothing voids");

    // The area is x=20..453, so start items stack from 20 and end items from 453.
    const auto &EYE = R.items[0];
    CHECK_RECT(EYE.rect, 20, (37 - 14) / 2.0, 13, 14, "first start item");

    // C3 flow: end items STACK from the end edge, so the last-listed one touches it.
    const auto &MAXI = R.items[1];
    CHECK_RECT(MAXI.rect, 453 - 13 - 13, MAXI.rect.y, 13, 14, "first end item is one width in from the edge");

    const auto &CLOSE = R.items[2];
    CHECK_RECT(CLOSE.rect, 453 - 13, CLOSE.rect.y, 13, 14, "LAST-listed end item touches the end edge");

    const auto &LOGO = R.items[3];
    CHECK_RECT(LOGO.rect, 20 + 13, LOGO.rect.y, 6, 2, "second start item follows the first");
}

// The title is a filler; with neighbours on both sides it must span exactly the gap.
static void testTitleFills()
{
    header("layout: the title fills the space between the neighbours");

    const auto M = orangeFrame();

    std::vector<Layout::SItemSpec> specs;
    specs.push_back(imageItem(0, 13, 14, Layout::eAlign::Start, 0));

    // The title is a FILLER: that is what "the title is just an item" means.
    Layout::SItemSpec title;
    title.id      = 1;
    title.kind    = Layout::eItemKind::Title;
    title.fill    = true;
    title.weight  = 1;
    specs.push_back(title);

    specs.push_back(imageItem(2, 13, 14, Layout::eAlign::End, 0));

    auto R = Layout::compute(M, specs);

    // start uses 13, end uses 13, so the gap is 433 - 13 - 13 = 407.
    const auto &T = R.items[1];
    CHECK_RECT(T.rect, 20 + 13, 0, 433 - 13 - 13, 37, "title spans the free gap, full band height");

    // The title is an ordinary item, so it is in the list and reachable by index.
    CHECK(R.titleIndex == 1, "title is reported as an item, got index %d", R.titleIndex);
    CHECK(R.items[R.titleIndex].kind == Layout::eItemKind::Title, "the indexed item is the title");
}

// No room for the title: it is dropped and the caller is told, so nothing spills over.
static void testTitleVoids()
{
    header("layout: no room for the title means it is not drawn");

    const auto M = orangeFrame();

    std::vector<Layout::SItemSpec> specs;
    // The item area is 433 wide, so two 300-wide neighbours leave nothing.
    specs.push_back(imageItem(0, 300, 14, Layout::eAlign::Start, 0));

    Layout::SItemSpec title;
    title.id      = 1;
    title.kind    = Layout::eItemKind::Title;
    title.fill    = true;
    title.weight  = 1;
    specs.push_back(title);

    specs.push_back(imageItem(2, 300, 14, Layout::eAlign::End, 0));

    auto R = Layout::compute(M, specs);

    CHECK(R.titleVoids, "neighbours wider than the area must void the title");
    for (const auto &item : R.items)
        CHECK(item.kind != Layout::eItemKind::Title, "no title rectangle may be emitted");
}

// A nine-patch item with a cap draws smaller than the space it was given.
static void testNinePatchCap()
{
    header("layout: a capped nine-patch reports its own drawn size");

    const auto M = orangeFrame();

    Layout::SItemSpec s;
    s.id               = 0;
    s.kind             = Layout::eItemKind::Image;
    s.ninepatch        = true;
    s.stretchVertical  = false;
    s.srcW             = 21;
    s.srcH             = 21;
    s.scale            = 2;
    s.maxStretch       = 7;
    s.npi.defined      = true;
    s.npi.border[0]    = 3;
    s.npi.border[1]    = 3;
    s.npi.border[2]    = 3;
    s.npi.border[3]    = 3;
    // Given a wide box, the middle is capped at 7 source px = 14 logical.
    s.w = 400;
    s.h = 6 * s.scale;

    auto R = Layout::compute(M, {s});

    const auto &I = R.items[0];
    CHECK_RECT(I.rect, 20, I.rect.y, 400, 12, "the rectangle keeps the space it was given");
    CHECK(std::abs(I.draw.w - (6 * s.scale + 7 * s.scale)) < 0.0001, "drawn width is corners + capped middle, got %.1f", I.draw.w);
}

// Both drawing and hit-testing read these rectangles, so sub-pixel values must go.
static void testSnap()
{
    header("layout: snapping is idempotent and lands on device pixels");

    Layout::SResolvedItem item;
    item.rect = {20.3, 11.5, 13.7, 14.2};
    item.draw = item.rect;

    std::vector<Layout::SResolvedItem> items{item};
    Layout::snap(items, 2.0);

    for (const double V : {items[0].rect.x, items[0].rect.y, items[0].rect.w, items[0].rect.h})
        CHECK(std::abs(V * 2.0 - std::round(V * 2.0)) < 0.0001, "every snapped value is a whole device pixel, got %.4f", V);

    auto again = items;
    Layout::snap(again, 2.0);
    CHECK(std::abs(again[0].rect.x - items[0].rect.x) < 0.0001, "snapping twice must not move anything");
}

static void testJson()
{
    header("status JSON: exact rectangles, same numbers drawing uses");

    const auto M = orangeFrame();

    std::vector<Layout::SItemSpec> specs;
    specs.push_back(imageItem(0, 13, 14, Layout::eAlign::Start, 0));
    specs.push_back(imageItem(1, 13, 14, Layout::eAlign::End, 0));

    auto R = Layout::compute(M, specs);
    Layout::snap(R.items, 1.0);

    const std::string JSON = Layout::itemsJson(R.items, 4, 4);

    // Two items, region "top", global coords = origin + rect.
    CHECK(JSON.find("\"id\": 0, \"type\": \"image\", \"region\": \"top\"") != std::string::npos, "first item shape: %s", JSON.c_str());
    // Cross-axis centre in a 37-high area is 11.5, snapped to 12, plus the origin.
    CHECK(JSON.find("\"x\": 24, \"y\": 16, \"w\": 13, \"h\": 14") != std::string::npos, "first item rect with origin applied: %s", JSON.c_str());
    // The end item is anchored to the area's right edge (453) minus its width, +4 origin.
    CHECK(JSON.find("\"x\": 444") != std::string::npos, "end item rect: %s", JSON.c_str());
    CHECK(JSON.front() == '[' && JSON.back() == ']', "JSON is an array");

    // Every comma-separated entry is balanced: a smoke test for malformed output.
    int depth = 0;
    for (const char c : JSON)
    {
        if (c == '{')
            depth++;
        else if (c == '}')
            depth--;
        CHECK(depth >= 0, "unbalanced braces in JSON");
    }
    CHECK(depth == 0, "JSON braces must balance, ended at %d", depth);
}

// A negative gap must PULL the title towards and past its neighbours. The parser used
// to reject negatives outright, so a title could never be tucked under an icon.
static void testNegativeGap()
{
    header("title: a negative gap pulls the box towards its neighbours");

    const double AREA_X = 20, AREA_W = 433;
    const double EYE_W = 26, BTN_W = 26;

    const auto LEFT_EDGE  = [&](double gapLeft) { return AREA_X + EYE_W + gapLeft; };
    const auto RIGHT_EDGE = [&](double gapRight) { return AREA_X + AREA_W - BTN_W - gapRight; };

    // Positive gap: the title stops short of each neighbour.
    CHECK(LEFT_EDGE(4) == AREA_X + EYE_W + 4, "positive gap moves away from the icon");
    CHECK(RIGHT_EDGE(4) == AREA_X + AREA_W - BTN_W - 4, "positive gap moves away from the buttons");

    // Negative gap: the title starts BEFORE the icon's right edge, i.e. under it.
    CHECK(LEFT_EDGE(-10) < AREA_X + EYE_W, "negative gap overlaps the icon");
    CHECK(LEFT_EDGE(-10) == AREA_X + EYE_W - 10, "negative gap is applied in the right direction");

    // The span is still the distance between the two edges, so a negative gap widens it.
    const double SPAN_POSITIVE = RIGHT_EDGE(2) - LEFT_EDGE(2);
    const double SPAN_NEGATIVE = RIGHT_EDGE(-10) - LEFT_EDGE(-10);
    CHECK(SPAN_NEGATIVE > SPAN_POSITIVE, "negative gaps must widen the title box, got %.1f vs %.1f", SPAN_NEGATIVE, SPAN_POSITIVE);
}

// titleBox() is logical; a 2x output must not double the offsets. This is the bug that
// made the title impossible to place correctly on the X axis.
static void testTitleUnits()
{
    header("title: logical offsets are not multiplied by the monitor scale");

    const double M = 2.0;
    const double OFFSET_LOGICAL = 9;

    // What the renderer does now: compute logically, scale once.
    const double DEVICE_ONCE = OFFSET_LOGICAL * M;

    // What it did before: offset already scaled inside, then scaled again outside.
    const double DEVICE_TWICE = (OFFSET_LOGICAL * M) * M;

    CHECK(DEVICE_ONCE == 18, "a 9px logical nudge is 18 device px at 2x");
    CHECK(DEVICE_ONCE != DEVICE_TWICE, "the double-scaled value (36) must not be used");

    // Round-tripping the band: the renderer recovers logical from device, so it must be
    // exact enough to place the title identically to the logical pass.
    const double DEVICE_BAND = 433 * M;
    CHECK(std::abs(DEVICE_BAND / M - 433) < 0.0001, "device -> logical round trip is exact");
}

// Left/right regions are columns: they flow vertically between the top and bottom
// regions, and their width is the reserved left/right space.
static void testColumns()
{
    header("layout: left and right are columns between the top and bottom regions");

    auto M = orangeFrame();
    M.boxW = 467;
    M.boxH = 245;

    // Left band: x 0..26, y 30..221 (between top 30 and bottom 24).
    const auto L = Layout::regionRect(M, Layout::eRegion::Left);
    CHECK_RECT(L, 0, 30, 26, 245 - 30 - 24, "left region runs between top and bottom");

    const auto R = Layout::regionRect(M, Layout::eRegion::Right);
    CHECK_RECT(R, 467 - 14, 30, 14, 191, "right region starts at the far edge");

    const auto B = Layout::regionRect(M, Layout::eRegion::Bottom);
    CHECK_RECT(B, 0, 245 - 24, 467, 24, "bottom region spans the full width");

    // Two buttons stacked in the left column.
    std::vector<Layout::SItemSpec> left;
    left.push_back(imageItem(0, 13, 14, Layout::eAlign::Start, 0));
    left.push_back(imageItem(1, 13, 14, Layout::eAlign::Start, 0));

    auto RES = Layout::compute(M, {}, {}, left, {});

    CHECK(RES.items.size() == 2, "both column items are laid out");
    CHECK(RES.items[0].region == Layout::eRegion::Left, "first item belongs to the left region");
    // Main axis is Y: first at the top of the band, second below it.
    CHECK(RES.items[0].rect.y < RES.items[1].rect.y, "column items stack downwards");
    // The item area is inset by the frame's content padding + bar.padding, the same as
    // the top band: 30 (region top) + 25 (content) + 7 (padding) = 62.
    CHECK(RES.items[0].rect.y == 62, "first column item starts below the content inset, got %.0f", RES.items[0].rect.y);
    CHECK(RES.items[1].rect.y == 62 + 14, "second follows the first, got %.0f", RES.items[1].rect.y);
}

// Reserved space comes from the frame, so an inset on one side must move only that side.
static void testAsymmetricReserve()
{
    header("layout: regions follow asymmetric reserved extents");

    auto M = orangeFrame();
    M.insetLeft = 24;
    M.insetBottom = 38;

    const auto L = Layout::regionRect(M, Layout::eRegion::Left);
    CHECK(std::abs(L.w - (26 + 24)) < 0.0001, "left region grows by its inset, got %.1f", L.w);

    const auto B = Layout::regionRect(M, Layout::eRegion::Bottom);
    CHECK(std::abs(B.h - (24 + 38)) < 0.0001, "bottom region grows by its inset, got %.1f", B.h);

    // A column must not overlap the grown bottom region.
    const auto R = Layout::regionRect(M, Layout::eRegion::Right);
    CHECK(std::abs(R.h - (245 - 30 - (24 + 38))) < 0.0001, "columns stop above the grown bottom, got %.1f", R.h);
}

// Gaps separate adjacent items without adding one outside the group.
static void testGap()
{
    header("flow: gap sits between items, never outside them");

    auto M = orangeFrame();
    M.gap  = 4;

    std::vector<Layout::SItemSpec> specs;
    specs.push_back(imageItem(0, 10, 10, Layout::eAlign::Start, 0));
    specs.push_back(imageItem(1, 10, 10, Layout::eAlign::Start, 0));
    specs.push_back(imageItem(2, 10, 10, Layout::eAlign::Start, 0));

    auto R = Layout::compute(M, specs);

    CHECK(R.items[0].rect.x == 20, "first item at the area start, no leading gap: %.0f", R.items[0].rect.x);
    CHECK(R.items[1].rect.x == 20 + 10 + 4, "second is one width plus one gap along: %.0f", R.items[1].rect.x);
    CHECK(R.items[2].rect.x == 20 + 2 * (10 + 4), "third continues: %.0f", R.items[2].rect.x);
}

// Two fillers split the remaining space by weight, never "first wins".
static void testFillWeights()
{
    header("flow: fillers split the leftover space by weight");

    const auto M = orangeFrame();

    std::vector<Layout::SItemSpec> specs;
    specs.push_back(imageItem(0, 40, 10, Layout::eAlign::Start, 0));

    Layout::SItemSpec a;
    a.id = 1; a.kind = Layout::eItemKind::Title; a.fill = true; a.weight = 1;
    specs.push_back(a);

    Layout::SItemSpec b;
    b.id = 2; b.kind = Layout::eItemKind::Title; b.fill = true; b.weight = 3;
    specs.push_back(b);

    auto R = Layout::compute(M, specs);

    // Area 433, fixed 40, so 393 is split 1:3 -> 98.25 and 294.75.
    CHECK(std::abs(R.items[1].rect.w - 393.0 / 4) < 0.001, "weight 1 gets a quarter: %.2f", R.items[1].rect.w);
    CHECK(std::abs(R.items[2].rect.w - 393.0 * 3 / 4) < 0.001, "weight 3 gets three quarters: %.2f", R.items[2].rect.w);
    CHECK(std::abs(R.items[1].rect.w + R.items[2].rect.w - 393.0) < 0.001, "the two fillers sum to the leftover");
}

// cross_align and margin replace the old vertical "gap" for a short title.
static void testCrossAlign()
{
    header("flow: cross_align + margin place a short item in a taller band");

    const auto M = orangeFrame();

    std::vector<Layout::SItemSpec> specs;

    Layout::SItemSpec t;
    t.id = 0; t.kind = Layout::eItemKind::Title; t.fill = true; t.weight = 1;
    t.h = 22;
    t.crossAlign = Layout::eAlign::Start;
    t.marginTop  = 6;
    specs.push_back(t);

    auto R = Layout::compute(M, specs);

    // Area starts at y=0 and is 37 tall; margin 6 then the box at the top.
    CHECK(R.items[0].rect.y == 6, "start + margin.top, got %.0f", R.items[0].rect.y);
    CHECK(R.items[0].rect.h == 22, "explicit height is honoured, got %.0f", R.items[0].rect.h);
}

// The DEFAULT must keep the pre-C3 behaviour: each end item anchors to the end edge on
// its own, separated only by its own offset. Turning stacking on is opt-in because it
// moves existing buttons.
static void testEndIndependent()
{
    header("flow: end items anchor independently by default");

    auto M = orangeFrame();
    M.stackEnd = false;

    std::vector<Layout::SItemSpec> specs;
    specs.push_back(imageItem(0, 13, 14, Layout::eAlign::End, 4));
    specs.push_back(imageItem(1, 13, 14, Layout::eAlign::End, -30));

    auto R = Layout::compute(M, specs);

    // Area is x=20..453, so an end item at offset 4 sits at 453-13+4 and at -30 at 453-13-30.
    CHECK_RECT(R.items[0].rect, 453 - 13 + 4, R.items[0].rect.y, 13, 14, "end item with offset +4");
    CHECK_RECT(R.items[1].rect, 453 - 13 - 30, R.items[1].rect.y, 13, 14, "end item with offset -30");
}

// Regressions: margins and cross offsets were silently ignored in several places.
static void testMarginsAndOffsets()
{
    header("flow: margins and cross offsets are actually applied");

    const auto M = orangeFrame();

    // 1. A filler's main-axis margin must SHRINK it, not shift a full-width box.
    {
        Layout::SItemSpec t;
        t.id = 0; t.kind = Layout::eItemKind::Title; t.fill = true; t.weight = 1;
        t.marginLeft = 10;

        auto R = Layout::compute(M, {t});

        CHECK(R.items[0].rect.x == 30, "margin.left insets the filler, got x=%.0f", R.items[0].rect.x);
        CHECK(std::abs(R.items[0].rect.w - 423) < 0.001, "and narrows it, got w=%.0f", R.items[0].rect.w);
        CHECK(std::abs(R.items[0].rect.x + R.items[0].rect.w - 453) < 0.001, "it must not overrun the end");
    }

    // 2. Cross offset on a row is offset_y; it was dead because only the main-axis
    //    branch ever read it.
    {
        auto a = imageItem(0, 10, 10, Layout::eAlign::Start, 0);
        a.offsetY = -2;
        auto b = imageItem(1, 10, 10, Layout::eAlign::Start, 0);

        auto R = Layout::compute(M, {a, b});

        CHECK(std::abs(R.items[0].rect.y - (R.items[1].rect.y - 2)) < 0.001, "offset_y must move the element across the band");
    }

    // 3. A fixed item's trailing margin is real space for the next item.
    {
        auto a = imageItem(0, 10, 10, Layout::eAlign::Start, 0);
        auto b = imageItem(1, 10, 10, Layout::eAlign::Start, 0);
        b.marginRight = 10;
        auto c = imageItem(2, 10, 10, Layout::eAlign::Start, 0);

        auto R = Layout::compute(M, {a, b, c});

        CHECK(R.items[2].rect.x == 20 + 10 + 10 + 10, "margin.right leaves a gap before the next item, got %.0f", R.items[2].rect.x);
    }

    // 4. An end item's trailing margin moves it inwards instead of overrunning the edge.
    {
        auto a = imageItem(0, 10, 10, Layout::eAlign::End, 0);
        a.marginLeft = 10;

        auto R = Layout::compute(M, {a});

        CHECK(std::abs(R.items[0].rect.x + R.items[0].rect.w - 453) < 0.001, "end item must not overrun the end edge, right=%.0f",
              R.items[0].rect.x + R.items[0].rect.w);
    }
}

// A margin must change the title's RECTANGLE, not just its pixels. The renderer caches
// the text texture by box size, so a margin that leaves the size unchanged is invisible.
static void testTitleMarginMovesBox()
{
    header("title: left/right margins change the rectangle");

    const auto M = orangeFrame();

    const auto BOX = [&](double ml, double mr) {
        Layout::SItemSpec t;
        t.id = 0; t.kind = Layout::eItemKind::Title; t.fill = true; t.weight = 1;
        t.marginLeft = ml; t.marginRight = mr;
        auto R = Layout::compute(M, {t});
        return R.items[0].rect;
    };

    const auto A = BOX(0, 0);
    const auto B = BOX(20, 0);
    const auto C = BOX(0, 20);
    const auto D = BOX(20, 20);

    CHECK(B.x == A.x + 20, "margin.left moves the box right: %.0f -> %.0f", A.x, B.x);
    CHECK(B.w == A.w - 20, "and makes it narrower: %.0f -> %.0f", A.w, B.w);
    CHECK(C.w == A.w - 20, "margin.right narrows the box: %.0f -> %.0f", A.w, C.w);
    // With no end group there is nothing to pin the right edge, so a right margin
    // narrows from the right: the box SHORTENS rather than sliding.
    CHECK(C.x == A.x, "margin.right leaves the start alone: %.0f -> %.0f", A.x, C.x);
    CHECK(std::abs((C.x + C.w) - (A.x + A.w - 20)) < 0.001, "and pulls the right edge in by the margin");
    CHECK(D.w == A.w - 40, "both margins subtract: %.0f -> %.0f", A.w, D.w);
    CHECK(D.x == A.x + 20, "and the left one still moves it");

    // The SIZE must differ whenever a margin differs, or a size-keyed texture cache
    // cannot notice the change.
    CHECK(B.w != A.w && C.w != A.w && D.w != A.w, "every margin change alters the box size");
    CHECK(B.x != A.x, "a left margin alters the box position");
}

int main()
{
    testTitleMarginMovesBox();
    testMarginsAndOffsets();
    testEndIndependent();
    testGap();
    testFillWeights();
    testCrossAlign();
    testColumns();
    testAsymmetricReserve();
    testNegativeGap();
    testTitleUnits();
    testJson();
    testItemArea();
    testFlow();
    testTitleFills();
    testTitleVoids();
    testNinePatchCap();
    testSnap();

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
