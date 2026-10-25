#include "luaApi.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include "plugin.hpp"
#include "config.hpp"
#include "hyprWindowDecorator.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <format>
#include <map>
#include <optional>
#include <string>

using namespace Hyprdecor::Lua;

// Hyprland's Lua config manager runs Lua 5.5; building against 5.4 headers and
// calling into a 5.5 state is memory corruption, so pin the version at compile time.
static_assert(LUA_VERSION_NUM == 505, "hyprdecor must be built against Lua 5.5 (the runtime Hyprland links)");

namespace {
    constexpr const char *TAG = "[HYPRDECOR] ";

    // Raises a Lua error, aborting the rest of the config evaluation. Deliberate: a
    // half-applied decoration is worse than a loud failure.
    //
    // lua_error() is a longjmp: it skips destructors of every live object in this
    // frame. The message is therefore copied into a fixed buffer and pushed, and the
    // longjmp happens with nothing else alive.
    [[noreturn]] void raiseF(lua_State *L, const char *message) noexcept
    {
        lua_pushstring(L, message);
        lua_error(L); // does not return
        std::abort();
    }

    // Builds the message in a nested scope so its std::string/format temporaries are
    // destroyed before raiseF() longjmps.
    template <typename... Args>
    int raise(lua_State *L, std::format_string<Args...> fmt, Args &&...args)
    {
        char buf[1024]{};
        {
            const std::string MESSAGE = std::format(fmt, std::forward<Args>(args)...);
            std::snprintf(buf, sizeof(buf), "%s", MESSAGE.c_str());
        }
        raiseF(L, buf);
    }

    bool hasField(lua_State *L, int tableIdx, const char *key)
    {
        if (!lua_istable(L, tableIdx))
            return false;
        lua_getfield(L, tableIdx, key);
        const bool PRESENT = !lua_isnil(L, -1);
        lua_pop(L, 1);
        return PRESENT;
    }
}

std::optional<std::string> Hyprdecor::Lua::stringField(lua_State *L, int tableIdx, const char *key)
{
    if (!lua_istable(L, tableIdx))
        return std::nullopt;

    lua_getfield(L, tableIdx, key);
    if (!lua_isstring(L, -1))
    {
        lua_pop(L, 1);
        return std::nullopt;
    }

    const char *RAW = lua_tostring(L, -1);
    std::string OUT = RAW ? RAW : "";
    lua_pop(L, 1);
    return OUT;
}

bool Hyprdecor::Lua::boolField(lua_State *L, int tableIdx, const char *key, bool fallback)
{
    if (!lua_istable(L, tableIdx))
        return fallback;

    lua_getfield(L, tableIdx, key);

    bool VALUE = fallback;
    if (lua_isboolean(L, -1))
        VALUE = lua_toboolean(L, -1) != 0;
    else if (lua_isnumber(L, -1))
        VALUE = lua_tonumber(L, -1) != 0;
    // anything else (nil, string, table) keeps the fallback

    lua_pop(L, 1);
    return VALUE;
}

double Hyprdecor::Lua::numberField(lua_State *L, int tableIdx, const char *key, double fallback)
{
    if (!lua_istable(L, tableIdx))
        return fallback;

    lua_getfield(L, tableIdx, key);
    if (!lua_isnumber(L, -1))
    {
        lua_pop(L, 1);
        return fallback;
    }

    const double VALUE = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return VALUE;
}

bool Hyprdecor::Lua::tableField(lua_State *L, int tableIdx, const char *key, const char *path)
{
    if (!lua_istable(L, tableIdx))
        raise(L, "hyprdecor.setup: '{}' must be a table", path);

    lua_getfield(L, tableIdx, key);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }

    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        raise(L, "hyprdecor.setup: '{}' must be a table", path);
    }

    return true;
}

// Reads `thickness`, which accepts either a number (all sides) or a table
// { left, top, right, bottom }. Sides left unset stay -1 and are resolved later from
// the nine-patch markers.
static bool readThicknessNamed(lua_State *L, int borderIdx, const char *key, SFrameThickness &out)
{
    lua_getfield(L, borderIdx, key);
    const int TYPE = lua_type(L, -1);

    if (TYPE == LUA_TNUMBER)
    {
        const auto V = lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (V < 0)
            return false;
        out = SFrameThickness{V, V, V, V};
        return true;
    }

    if (TYPE == LUA_TTABLE)
    {
        const int IDX = lua_absindex(L, -1);
        const auto SIDE = [&](const char *name) {
            lua_getfield(L, IDX, name);
            const bool HAS = lua_isnumber(L, -1);
            const double V = HAS ? lua_tonumber(L, -1) : -1;
            lua_pop(L, 1);
            return V;
        };
        out.left   = SIDE("left");
        out.top    = SIDE("top");
        out.right  = SIDE("right");
        out.bottom = SIDE("bottom");
        lua_pop(L, 1);
        return out.left >= 0 || out.top >= 0 || out.right >= 0 || out.bottom >= 0;
    }

    lua_pop(L, 1);
    return false;
}

static bool readThickness(lua_State *L, int borderIdx, SFrameThickness &out)
{
    return readThicknessNamed(L, borderIdx, "thickness", out);
}

// Reads { left, top, right, bottom } where anything, negatives included, is meaningful.
// A separate reader because readThicknessNamed uses -1 as the "unset" sentinel, which
// would make a legitimate -1 gap indistinguishable from an absent one. Thickness must
// stay non-negative, so it keeps the strict reader.
struct SSignedSides
{
    std::optional<double> left, top, right, bottom;
};

static bool readSignedSides(lua_State *L, int borderIdx, const char *key, SSignedSides &out)
{
    lua_getfield(L, borderIdx, key);
    const int TYPE = lua_type(L, -1);

    if (TYPE == LUA_TNUMBER)
    {
        const double V = lua_tonumber(L, -1);
        lua_pop(L, 1);
        out = {V, V, V, V};
        return true;
    }

    if (TYPE == LUA_TTABLE)
    {
        const int IDX = lua_absindex(L, -1);
        const auto SIDE = [&](const char *name) -> std::optional<double> {
            lua_getfield(L, IDX, name);
            if (lua_isnil(L, -1))
            {
                lua_pop(L, 1);
                return std::nullopt;
            }
            if (!lua_isnumber(L, -1))
            {
                lua_pop(L, 1);
                return std::nullopt;
            }
            const double V = lua_tonumber(L, -1);
            lua_pop(L, 1);
            return V;
        };

        out.left   = SIDE("left");
        out.top    = SIDE("top");
        out.right  = SIDE("right");
        out.bottom = SIDE("bottom");
        lua_pop(L, 1);
        return out.left || out.top || out.right || out.bottom;
    }

    lua_pop(L, 1);
    return false;
}

// hl.plugin.hyprdecor.setup({ defaults = { border = {...}, title = {...} } })
//
// Every documented field is validated. Anything malformed is a hard error: setup()
// either applies completely or not at all.
int Hyprdecor::Lua::lSetup(lua_State *L)
{
    if (lua_gettop(L) < 1 || !lua_istable(L, 1))
        return raise(L, "hyprdecor.setup: expected a table argument");

    const int CFG = lua_absindex(L, 1);

    // Start from the documented defaults every time, so a reload never inherits values
    // from a previous setup() call.
    SConfig NEXT;

    lua_getfield(L, CFG, "defaults");
    if (lua_isnil(L, -1))
        return raise(L, "hyprdecor.setup: missing required table 'defaults'");

    const int DEFAULTS = lua_gettop(L);
    if (!lua_istable(L, DEFAULTS))
        return raise(L, "hyprdecor.setup: 'defaults' must be a table");

    // ---- border -----------------------------------------------------------
    if (tableField(L, DEFAULTS, "border", "defaults.border"))
    {
        const int BORDER = lua_gettop(L);

        if (!hasField(L, BORDER, "texture"))
            return raise(L, "hyprdecor.setup: 'defaults.border.texture' is required");

        const auto TEXTURE = stringField(L, BORDER, "texture");
        if (!TEXTURE)
            return raise(L, "hyprdecor.setup: 'defaults.border.texture' must be a string");
        if (TEXTURE->empty())
            return raise(L, "hyprdecor.setup: 'defaults.border.texture' must not be empty");

        // Validate the file now, so a typo fails at config time rather than silently
        // leaving windows undecorated.
        const std::string ACTIVE = gPlugin->resolveTexturePath(*TEXTURE, {"_active", "_focused", "active", ""});
        if (ACTIVE.empty())
        {
            if (!gPlugin->m_szLastResolveError.empty())
                return raise(L, "hyprdecor.setup: cannot access texture \"{}\": {}", *TEXTURE, gPlugin->m_szLastResolveError);

            return raise(L, "hyprdecor.setup: texture not found: \"{}\" "
                            "(tried {}_active.png, {}_focused.png, {}active.png, {}.png). "
                            "Check that the path is absolute and that the compositor's user can read it.",
                            *TEXTURE, *TEXTURE, *TEXTURE, *TEXTURE, *TEXTURE);
        }

        NEXT.textureBase = *TEXTURE;

        if (hasField(L, BORDER, "thickness") && !readThickness(L, BORDER, NEXT.thickness))
            return raise(L, "hyprdecor.setup: 'defaults.border.thickness' must be a number or a table {{ left, top, right, bottom }}");

        if (hasField(L, BORDER, "inset") && !readThicknessNamed(L, BORDER, "inset", NEXT.inset))
            return raise(L, "hyprdecor.setup: 'defaults.border.inset' must be a number or a table {{ left, top, right, bottom }}");

        if (hasField(L, BORDER, "scale"))
        {
            const double ART_SCALE = numberField(L, BORDER, "scale", 1);
            if (ART_SCALE < 1 || ART_SCALE != (int)ART_SCALE)
                return raise(L, "hyprdecor.setup: 'defaults.border.scale' must be a positive integer");
            NEXT.artScale = (int)ART_SCALE;
        }

        if (hasField(L, BORDER, "linear_filtering"))
            NEXT.linearFiltering = boolField(L, BORDER, "linear_filtering", true);

        if (hasField(L, BORDER, "repeat"))
            NEXT.repeat = boolField(L, BORDER, "repeat", false);

        lua_pop(L, 1);
    }

    // ---- title ------------------------------------------------------------
    if (tableField(L, DEFAULTS, "title", "defaults.title"))
    {
        const int TITLE = lua_gettop(L);

        if (hasField(L, TITLE, "enabled"))
            NEXT.titleEnabled = boolField(L, TITLE, "enabled", true);
        if (hasField(L, TITLE, "size"))
            NEXT.titleSize = (int)numberField(L, TITLE, "size", 10);

        if (hasField(L, TITLE, "mode"))
        {
            const auto MODE = stringField(L, TITLE, "mode");
            if (!MODE)
                return raise(L, "hyprdecor.setup: 'defaults.title.mode' must be a string");
            if (*MODE == "fill")
                NEXT.titleMode = SConfig::eTitleMode::Fill;
            else if (*MODE == "box")
                NEXT.titleMode = SConfig::eTitleMode::Box;
            else
                return raise(L, "hyprdecor.setup: 'defaults.title.mode' must be \"fill\" or \"box\" (got \"{}\")", *MODE);
        }

        if (hasField(L, TITLE, "anchor"))
        {
            const auto ANCHOR = stringField(L, TITLE, "anchor");
            if (!ANCHOR)
                return raise(L, "hyprdecor.setup: 'defaults.title.anchor' must be a string");

            static const std::map<std::string, SConfig::eTitleAnchor> ANCHORS = {
                {"top-left", SConfig::eTitleAnchor::TopLeft},           {"top-centre", SConfig::eTitleAnchor::TopCentre},
                {"top-center", SConfig::eTitleAnchor::TopCentre},       {"top-right", SConfig::eTitleAnchor::TopRight},
                {"centre-left", SConfig::eTitleAnchor::CentreLeft},     {"center-left", SConfig::eTitleAnchor::CentreLeft},
                {"centre", SConfig::eTitleAnchor::Centre},              {"center", SConfig::eTitleAnchor::Centre},
                {"centre-right", SConfig::eTitleAnchor::CentreRight},   {"center-right", SConfig::eTitleAnchor::CentreRight},
                {"bottom-left", SConfig::eTitleAnchor::BottomLeft},     {"bottom-centre", SConfig::eTitleAnchor::BottomCentre},
                {"bottom-center", SConfig::eTitleAnchor::BottomCentre}, {"bottom-right", SConfig::eTitleAnchor::BottomRight},
            };

            const auto IT = ANCHORS.find(*ANCHOR);
            if (IT == ANCHORS.end())
                return raise(L, "hyprdecor.setup: 'defaults.title.anchor' is not a valid anchor (got \"{}\")", *ANCHOR);
            NEXT.titleAnchor = IT->second;
        }

        // `offset = { x, y }`, strict: a missing component is 0, a wrong type is an error.
        if (hasField(L, TITLE, "offset"))
        {
            lua_getfield(L, TITLE, "offset");
            if (lua_type(L, -1) != LUA_TTABLE)
            {
                lua_pop(L, 1);
                return raise(L, "hyprdecor.setup: 'defaults.title.offset' must be a table {{ x, y }}");
            }

            const int OFFSET = lua_gettop(L);
            const auto COMP = [&](const char *name) -> std::optional<double> {
                lua_getfield(L, OFFSET, name);
                if (lua_isnil(L, -1))
                {
                    lua_pop(L, 1);
                    return 0.0;
                }
                if (!lua_isnumber(L, -1))
                {
                    lua_pop(L, 1);
                    return std::nullopt;
                }
                const double V = lua_tonumber(L, -1);
                lua_pop(L, 1);
                return V;
            };

            const auto OX = COMP("x");
            const auto OY = OX ? COMP("y") : std::nullopt;
            lua_pop(L, 1);
            if (!OX || !OY)
                return raise(L, "hyprdecor.setup: 'defaults.title.offset.x' and '.y' must be numbers");
            NEXT.titleOffsetX = *OX;
            NEXT.titleOffsetY = *OY;
        }

        if (hasField(L, TITLE, "width"))
            NEXT.titleWidth = numberField(L, TITLE, "width", -1);
        if (hasField(L, TITLE, "height"))
            NEXT.titleHeight = numberField(L, TITLE, "height", -1);

        // fill mode only: horizontal alignment of the text inside the box.
        if (hasField(L, TITLE, "align"))
        {
            const auto ALIGN = stringField(L, TITLE, "align");
            if (!ALIGN)
                return raise(L, "hyprdecor.setup: 'defaults.title.align' must be a string");
            if (*ALIGN == "left" || *ALIGN == "centre" || *ALIGN == "center" || *ALIGN == "right")
                NEXT.titleAlign = (*ALIGN == "center") ? "centre" : *ALIGN;
            else
                return raise(L, "hyprdecor.setup: 'defaults.title.align' must be left, centre or right (got \"{}\")", *ALIGN);
        }
        if (hasField(L, TITLE, "font"))
        {
            const auto FONT = stringField(L, TITLE, "font");
            if (FONT)
                NEXT.titleFont = *FONT;
        }
        if (hasField(L, TITLE, "placement"))
        {
            const auto PLACE = stringField(L, TITLE, "placement");
            if (PLACE)
                NEXT.titlePlacement = *PLACE;
        }
        if (hasField(L, TITLE, "color"))
            NEXT.textColor = (uint64_t)numberField(L, TITLE, "color", 0xFFFFFFFF);
        if (hasField(L, TITLE, "inset"))
            NEXT.titleInset = numberField(L, TITLE, "inset", 0);
        // `gap` accepts a number (all four) or { left, top, right, bottom }.
        // `gap` is ONE signed number: the distance to the neighbours on the main axis.
        // A table is rejected rather than reinterpreted, because a per-side gap made
        // `bottom` resize the box while `top` moved it — two different operations behind
        // one name. Vertical placement is height + cross_align + margin.
        if (hasField(L, TITLE, "gap"))
        {
            lua_getfield(L, TITLE, "gap");
            if (!lua_isnumber(L, -1))
            {
                lua_pop(L, 1);
                return raise(L, "hyprdecor.setup: 'defaults.title.gap' must be a number (the distance to the neighbours)");
            }
            NEXT.titleGap = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }

        if (hasField(L, TITLE, "cross_align"))
        {
            const auto CA = stringField(L, TITLE, "cross_align");
            if (!CA)
                return raise(L, "hyprdecor.setup: 'defaults.title.cross_align' must be a string");
            if (*CA == "start")
                NEXT.titleCrossAlign = "start";
            else if (*CA == "centre" || *CA == "center")
                NEXT.titleCrossAlign = "centre";
            else if (*CA == "end")
                NEXT.titleCrossAlign = "end";
            else
                return raise(L, "hyprdecor.setup: 'defaults.title.cross_align' must be start, centre or end (got \"{}\")", *CA);
        }

        if (hasField(L, TITLE, "margin"))
        {
            SSignedSides MARGIN;
            if (!readSignedSides(L, TITLE, "margin", MARGIN))
                return raise(L, "hyprdecor.setup: 'defaults.title.margin' must be a number or a table {{ top, bottom }}");
            if (MARGIN.top)
                NEXT.titleMarginTop = *MARGIN.top;
            if (MARGIN.bottom)
                NEXT.titleMarginBottom = *MARGIN.bottom;
        }
        if (hasField(L, TITLE, "text_offset_x"))
            NEXT.textOffsetX = numberField(L, TITLE, "text_offset_x", 0);
        if (hasField(L, TITLE, "text_offset_y"))
            NEXT.textOffsetY = numberField(L, TITLE, "text_offset_y", 0);
        if (hasField(L, TITLE, "ellipsize"))
            NEXT.titleEllipsize = boolField(L, TITLE, "ellipsize", true);

        // Optional backdrop, nested so it can never drift from the text.
        if (tableField(L, TITLE, "background", "defaults.title.background"))
        {
            const int BG = lua_gettop(L);

            const auto BGIMAGE = stringField(L, BG, "image");
            if (!BGIMAGE || BGIMAGE->empty())
                return raise(L, "hyprdecor.setup: 'defaults.title.background.image' is required");

            NEXT.titleBgEnabled = boolField(L, BG, "enabled", true);
            NEXT.titleBgPath    = *BGIMAGE;

            lua_pop(L, 1);
        }

        lua_pop(L, 1);
    }

    // ---- bar --------------------------------------------------------------
    if (tableField(L, DEFAULTS, "bar", "defaults.bar"))
    {
        const int BAR = lua_gettop(L);

        if (hasField(L, BAR, "color"))
            NEXT.barColor = (uint64_t)numberField(L, BAR, "color", 0x88333333);
        if (hasField(L, BAR, "blur"))
            NEXT.blur = boolField(L, BAR, "blur", false);
        if (hasField(L, BAR, "padding"))
            NEXT.padding = (int)numberField(L, BAR, "padding", 7);
        if (hasField(L, BAR, "gap"))
            NEXT.regionGap = numberField(L, BAR, "gap", 0);
        if (hasField(L, BAR, "stack_end"))
            NEXT.regionStackEnd = boolField(L, BAR, "stack_end", false);
        if (hasField(L, BAR, "button_padding"))
            NEXT.buttonPadding = (int)numberField(L, BAR, "button_padding", 5);
        if (hasField(L, BAR, "buttons_alignment"))
        {
            const auto ALIGN = stringField(L, BAR, "buttons_alignment");
            if (ALIGN)
                NEXT.buttonsAlignment = *ALIGN;
        }
        if (hasField(L, BAR, "part_of_window"))
            NEXT.partOfWindow = boolField(L, BAR, "part_of_window", true);
        if (hasField(L, BAR, "precedence_over_border"))
            NEXT.precedenceOverBorder = boolField(L, BAR, "precedence_over_border", false);
        if (hasField(L, BAR, "render_above"))
            NEXT.renderAbove = boolField(L, BAR, "render_above", true);
        if (hasField(L, BAR, "on_double_click"))
        {
            const auto CMD = stringField(L, BAR, "on_double_click");
            if (CMD)
                NEXT.onDoubleClick = *CMD;
        }

        lua_pop(L, 1);
    }

    // ---- icon -------------------------------------------------------------
    if (tableField(L, DEFAULTS, "icon", "defaults.icon"))
    {
        const int ICON = lua_gettop(L);

        if (hasField(L, ICON, "enabled"))
            NEXT.appIconEnabled = boolField(L, ICON, "enabled", false);
        if (hasField(L, ICON, "offset_x"))
            NEXT.appIconOffsetX = numberField(L, ICON, "offset_x", 0);
        if (hasField(L, ICON, "offset_y"))
            NEXT.appIconOffsetY = numberField(L, ICON, "offset_y", 0);

        lua_pop(L, 1);
    }

    // ---- elements ---------------------------------------------------------
    if (tableField(L, DEFAULTS, "elements", "defaults.elements"))
    {
        const int LIST = lua_gettop(L);
        const int N = (int)lua_rawlen(L, LIST);

        for (int i = 1; i <= N; ++i)
        {
            lua_rawgeti(L, LIST, i);
            const int ITEM = lua_gettop(L);

            if (!lua_istable(L, ITEM))
                return raise(L, "hyprdecor.setup: 'defaults.elements[{}]' must be a table", i);

            SElement E;

            const auto IMAGE = stringField(L, ITEM, "image");
            if (!IMAGE || IMAGE->empty())
                return raise(L, "hyprdecor.setup: 'defaults.elements[{}].image' is required", i);
            E.path = *IMAGE;

            if (hasField(L, ITEM, "anchor"))
            {
                const auto ANCHOR = stringField(L, ITEM, "anchor");
                if (!ANCHOR)
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].anchor' must be a string", i);

                if (*ANCHOR == "top-left")
                    E.anchor = SElement::eAnchor::TopLeft;
                else if (*ANCHOR == "top-centre" || *ANCHOR == "top-center")
                    E.anchor = SElement::eAnchor::TopCentre;
                else if (*ANCHOR == "top-right")
                    E.anchor = SElement::eAnchor::TopRight;
                else
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].anchor' must be top-left, top-centre or top-right (got \"{}\")", i, *ANCHOR);
            }

            if (hasField(L, ITEM, "region"))
            {
                const auto REGION = stringField(L, ITEM, "region");
                if (!REGION)
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].region' must be a string", i);
                if (*REGION == "top")
                    E.region = SElement::eRegion::Top;
                else if (*REGION == "bottom")
                    E.region = SElement::eRegion::Bottom;
                else if (*REGION == "left")
                    E.region = SElement::eRegion::Left;
                else if (*REGION == "right")
                    E.region = SElement::eRegion::Right;
                else
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].region' must be top, bottom, left or right (got \"{}\")", i, *REGION);
            }

            if (hasField(L, ITEM, "filter"))
            {
                const auto FILTER = stringField(L, ITEM, "filter");
                if (!FILTER)
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].filter' must be a string", i);
                if (*FILTER == "nearest")
                    E.filter = false;
                else if (*FILTER == "linear" || *FILTER == "bilinear")
                    E.filter = true;
                else
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].filter' must be \"nearest\" or \"linear\"", i);
            }

            if (hasField(L, ITEM, "scale"))
            {
                const double ES = numberField(L, ITEM, "scale", 1);
                if (ES < 1 || ES != (int)ES)
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].scale' must be a positive integer", i);
                E.scale = (int)ES;
            }

            if (hasField(L, ITEM, "ninepatch"))
                E.ninepatch = boolField(L, ITEM, "ninepatch", false);

            if (hasField(L, ITEM, "stretch"))
            {
                const auto ST = stringField(L, ITEM, "stretch");
                if (!ST)
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].stretch' must be a string", i);
                if (*ST == "both")
                    E.stretchVertical = true;
                else if (*ST == "horizontal")
                    E.stretchVertical = false;
                else
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].stretch' must be \"horizontal\" or \"both\"", i);
            }

            if (hasField(L, ITEM, "interactive"))
                E.interactive = boolField(L, ITEM, "interactive", false);

            if (hasField(L, ITEM, "fill"))
            {
                lua_getfield(L, ITEM, "fill");
                if (lua_isboolean(L, -1))
                    E.fill = lua_toboolean(L, -1) != 0;
                else if (lua_isnumber(L, -1))
                {
                    // A number is a weight, per the spec.
                    E.fill   = true;
                    E.weight = lua_tonumber(L, -1);
                }
                else
                {
                    lua_pop(L, 1);
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].fill' must be a boolean or a weight", i);
                }
                lua_pop(L, 1);
            }

            if (hasField(L, ITEM, "cross_align"))
            {
                const auto CA = stringField(L, ITEM, "cross_align");
                if (!CA)
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].cross_align' must be a string", i);
                if (*CA == "start" || *CA == "centre" || *CA == "center" || *CA == "end")
                    E.crossAlign = (*CA == "center") ? "centre" : *CA;
                else
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].cross_align' must be start, centre or end (got \"{}\")", i, *CA);
            }

            if (hasField(L, ITEM, "margin"))
            {
                SSignedSides MG;
                if (!readSignedSides(L, ITEM, "margin", MG))
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].margin' must be a number or a table", i);
                if (MG.left)
                    E.marginLeft = *MG.left;
                if (MG.top)
                    E.marginTop = *MG.top;
                if (MG.right)
                    E.marginRight = *MG.right;
                if (MG.bottom)
                    E.marginBottom = *MG.bottom;
            }

            if (hasField(L, ITEM, "max_stretch"))
                E.maxStretch = numberField(L, ITEM, "max_stretch", -1);
            if (hasField(L, ITEM, "min_stretch"))
                E.minStretch = numberField(L, ITEM, "min_stretch", -1);

            E.offsetX = numberField(L, ITEM, "offset_x", 0);
            E.offsetY = numberField(L, ITEM, "offset_y", 0);
            E.width   = hasField(L, ITEM, "width") ? numberField(L, ITEM, "width", -1) : -1;
            E.height  = hasField(L, ITEM, "height") ? numberField(L, ITEM, "height", -1) : -1;

            NEXT.elements.push_back(E);

            lua_pop(L, 1); // ITEM
        }

        lua_pop(L, 1); // LIST
    }

    // ---- commit -----------------------------------------------------------
    // Only after everything validated: setup() applies completely or not at all.
    NEXT.applies = gPlugin->cfg.applies + 1;
    gPlugin->cfg = NEXT;

    // Was declared and reported by `status` but never assigned, so status always said
    // "setup() applied: no" even when the config had clearly taken effect.
    gPlugin->setup_applied = true;

    Log::logger->log(Log::DEBUG, std::format("{}setup(): texture=\"{}\" thickness L{} T{} R{} B{} artScale={} font=\"{}\"",
                                             TAG, gPlugin->cfg.textureBase, gPlugin->cfg.thickness.left, gPlugin->cfg.thickness.top,
                                             gPlugin->cfg.thickness.right, gPlugin->cfg.thickness.bottom, gPlugin->cfg.artScale,
                                             gPlugin->cfg.titleFont));

    // Deferred: applying the configuration drives layout and damage, which must not
    // happen re-entrantly inside a Lua callback.
    gPlugin->requestUpdate();

    lua_settop(L, 0);
    lua_pushboolean(L, 1);
    return 1;
}
