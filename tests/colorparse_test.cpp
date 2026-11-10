// Standalone test for src/colorparse.hpp. No Hyprland, no GPU.
//   cmake --build build --target colorparse_test && ./build/colorparse_test

#include "colorparse.hpp"

#include <cstdio>

static int failures = 0;

static void expect(const char *input, std::optional<int64_t> want)
{
    const auto GOT = ColorParse::parse(input);
    if (GOT == want)
        return;

    ++failures;
    std::printf("FAIL  parse(\"%s\"): want %s%lx, got %s%lx\n", input, want ? "0x" : "nullopt ", want ? (long)*want : 0L, GOT ? "0x" : "nullopt ",
                GOT ? (long)*GOT : 0L);
}

int main()
{
    // rgba(): alpha is the LAST byte in the text but the FIRST in the result.
    expect("rgba(ff000080)", 0x80ff0000);
    expect("rgba(11223344)", 0x44112233);
    expect("rgba(000000ff)", 0xff000000);
    expect("  rgba(FF000080)  ", 0x80ff0000);

    // rgb(): opaque.
    expect("rgb(00ff00)", 0xff00ff00);
    expect("rgb(123456)", 0xff123456);

    // 0xAARRGGBB is taken as written.
    expect("0x88333333", 0x88333333);
    expect("0xFFFFFFFF", 0xFFFFFFFFLL);
    expect("0xff0000", 0x00ff0000);

    // Decimal.
    expect("0", 0);
    expect("4294967295", 0xFFFFFFFFLL);

    // Everything below used to be accepted, or crashed the stricter paths, or became colour 0.
    expect("", std::nullopt);
    expect("rgba(ff0000)", std::nullopt);   // wrong digit count for rgba
    expect("rgb(ff0000ff)", std::nullopt);  // wrong digit count for rgb
    expect("rgba(ff0000", std::nullopt);    // unterminated
    expect("rgba(gg000000)", std::nullopt); // not hex
    expect("foo(112233)", std::nullopt);
    expect("0xzz", std::nullopt);
    expect("0x", std::nullopt);
    expect("0x123456789", std::nullopt);    // more than 8 digits
    expect("12abc", std::nullopt);          // stoll() returned 12 here
    expect("-1", std::nullopt);
    expect("4294967296", std::nullopt);
    expect("red", std::nullopt);

    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }

    std::printf("colorparse: all checks passed\n");
    return 0;
}
