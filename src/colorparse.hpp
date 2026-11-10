// Colour parsing for window-rule effect values, with no Hyprland dependency so it can be
// tested standalone (tests/colorparse_test.cpp).

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ColorParse
{

namespace detail
{
    /** Parses all of `text` as an unsigned number in `base`; nullopt if anything is left over. */
    inline std::optional<uint64_t> parseUnsigned(std::string_view text, int base)
    {
        if (text.empty())
            return std::nullopt;

        uint64_t     value = 0;
        const char  *end   = text.data() + text.size();
        const auto   RES   = std::from_chars(text.data(), end, value, base);
        if (RES.ec != std::errc{} || RES.ptr != end)
            return std::nullopt;

        return value;
    }

    inline std::string_view trim(std::string_view text)
    {
        const auto START = text.find_first_not_of(" \t");
        if (START == std::string_view::npos)
            return {};
        return text.substr(START, text.find_last_not_of(" \t") - START + 1);
    }
}

/**
 * Parses a colour into the 0xAARRGGBB integer that CHyprColor and SConfig use.
 *
 * Accepted forms (surrounding whitespace is ignored):
 *   rgba(rrggbbaa)   exactly 8 hex digits; NOTE the alpha comes LAST here and first in the result
 *   rgb(rrggbb)      exactly 6 hex digits; opaque
 *   0xAARRGGBB       up to 8 hex digits, taken as written (so 0xFF0000 has alpha 0)
 *   4278190080       decimal, 0 to 4294967295
 *
 * Anything else, including trailing garbage such as "12abc" or a negative number, returns
 * nullopt. Callers must report that: silently substituting 0 turns a typo into an
 * invisible, fully transparent colour.
 */
inline std::optional<int64_t> parse(const std::string &input)
{
    const auto IN = detail::trim(input);

    if (IN.ends_with(')'))
    {
        const bool HASALPHA = IN.starts_with("rgba(");
        if (!HASALPHA && !IN.starts_with("rgb("))
            return std::nullopt;

        const auto HEX = IN.substr(HASALPHA ? 5 : 4, IN.size() - (HASALPHA ? 5 : 4) - 1);
        if (HEX.size() != (HASALPHA ? 8u : 6u))
            return std::nullopt;

        const auto VALUE = detail::parseUnsigned(HEX, 16);
        if (!VALUE)
            return std::nullopt;

        if (HASALPHA)
            return (int64_t)(((*VALUE & 0xFF) << 24) | (*VALUE >> 8));
        return (int64_t)(0xFF000000u | *VALUE);
    }

    if (IN.starts_with("0x"))
    {
        const auto HEX = IN.substr(2);
        if (HEX.size() > 8)
            return std::nullopt;

        const auto VALUE = detail::parseUnsigned(HEX, 16);
        return VALUE ? std::optional<int64_t>((int64_t)*VALUE) : std::nullopt;
    }

    const auto VALUE = detail::parseUnsigned(IN, 10);
    if (!VALUE || *VALUE > 0xFFFFFFFFu)
        return std::nullopt;

    return (int64_t)*VALUE;
}

}
