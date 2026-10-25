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

    const auto M = orangeFrame();

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

    // C1 is behaviour-preserving: `end` items are anchored to the end edge
    // INDEPENDENTLY, exactly as the old elements were, so their own offsets separate
    // them. C3 turns this into stacking flow.
    const auto &MAXI = R.items[1];
    CHECK_RECT(MAXI.rect, 453 - 13, MAXI.rect.y, 13, 14, "end item touches the end edge");

    const auto &CLOSE = R.items[2];
    CHECK_RECT(CLOSE.rect, 453 - 13, CLOSE.rect.y, 13, 14, "both end items anchor to the same edge");

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

    Layout::SItemSpec title;
    title.id      = 1;
    title.kind    = Layout::eItemKind::Title;
    title.isTitle = true;
    specs.push_back(title);

    specs.push_back(imageItem(2, 13, 14, Layout::eAlign::End, 0));

    auto R = Layout::compute(M, specs);

    // start uses 13, end uses 13, so the gap is 433 - 13 - 13 = 407.
    const auto &T = R.items[1];
    CHECK_RECT(T.rect, 20 + 13, 0, 433 - 13 - 13, 37, "title spans the free gap, full band height");
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
    title.isTitle = true;
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

int main()
{
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
