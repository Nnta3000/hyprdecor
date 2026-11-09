#include "luaApi.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include "plugin.hpp"
#include "luaRead.hpp"
#include "itemstate.hpp"

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

// Spacing/margin reading lives in luaRead.hpp, which has no Hyprland dependency so the
// tests exercise the same code. `SSided` is alias'd here to keep the call sites short.
using SSignedSides = LuaRead::SSided;

static bool readSignedSides(lua_State *L, int borderIdx, const char *key, SSignedSides &out)
{
    return LuaRead::readSided(L, borderIdx, key, out);
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

    // ---- scale (applies to everything) ------------------------------------
    // Read FIRST, so `border` and every element below can inherit it. A per-element
    // `scale` still wins; this is only the fallback.
    if (hasField(L, DEFAULTS, "scale"))
    {
        const double SC = numberField(L, DEFAULTS, "scale", 1);
        if (SC < 1 || SC != (int)SC)
            return raise(L, "hyprdecor.setup: 'defaults.scale' must be a positive integer");
        NEXT.defaultScale = (int)SC;
    }

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
        else if (NEXT.defaultScale > 0)
            // No border-specific scale: inherit the global one.
            NEXT.artScale = NEXT.defaultScale;

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
        if (hasField(L, TITLE, "font_unfocused"))
        {
            const auto FONT = stringField(L, TITLE, "font_unfocused");
            if (!FONT)
                return raise(L, "hyprdecor.setup: 'defaults.title.font_unfocused' must be a string");
            if (FONT->empty())
                return raise(L, "hyprdecor.setup: 'defaults.title.font_unfocused' must not be empty "
                                "(omit the key to reuse 'font')");
            NEXT.titleFontUnfocused = *FONT;
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

        if (hasField(L, TITLE, "baseline"))
        {
            const auto B = stringField(L, TITLE, "baseline");
            if (!B)
                return raise(L, "hyprdecor.setup: 'defaults.title.baseline' must be a string");
            if (*B != "font" && *B != "line")
                return raise(L, "hyprdecor.setup: 'defaults.title.baseline' \"{}\" is not valid (font | line)", *B);
            NEXT.titleBaseline = *B;
        }

        if (hasField(L, TITLE, "baseline_offset"))
            NEXT.titleBaselineOffset = numberField(L, TITLE, "baseline_offset", 0);

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
            if (MARGIN.left)
                NEXT.titleMarginLeft = *MARGIN.left;
            if (MARGIN.right)
                NEXT.titleMarginRight = *MARGIN.right;
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

        if (hasField(L, BAR, "resize_grab_width"))
        {
            const double RW = numberField(L, BAR, "resize_grab_width", 8);
            if (RW < 0)
                return raise(L, "hyprdecor.setup: 'defaults.bar.resize_grab_width' must be >= 0");
            NEXT.resizeGrabWidth = RW;
        }
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
        if (hasField(L, BAR, "retro_shader"))
        {
            const auto NAME = stringField(L, BAR, "retro_shader");
            if (NAME)
                NEXT.retroShader = *NAME;
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

            // `type` defaults to "image" so every existing element keeps working.
            if (hasField(L, ITEM, "type"))
            {
                const auto TYPE = stringField(L, ITEM, "type");
                if (!TYPE)
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].type' must be a string", i);
                if (*TYPE == "image")
                    E.type = SElement::eType::Image;
                else if (*TYPE == "button")
                    E.type = SElement::eType::Button;
                else if (*TYPE == "spacer")
                    E.type = SElement::eType::Spacer;
                else if (*TYPE == "text")
                    E.type = SElement::eType::Text;
                else
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].type' must be image, button, spacer or text (got \"{}\")", i,
                                  *TYPE);
            }

            // Only the drawn types need a file.
            const bool NEEDSFILE = E.type == SElement::eType::Image || E.type == SElement::eType::Button;
            const auto IMAGE     = stringField(L, ITEM, "image");
            if (NEEDSFILE && (!IMAGE || IMAGE->empty()))
                return raise(L, "hyprdecor.setup: 'defaults.elements[{}].image' is required for type \"{}\"", i,
                             E.type == SElement::eType::Button ? "button" : "image");
            if (IMAGE)
                E.path = *IMAGE;

            if (E.type == SElement::eType::Text && hasField(L, ITEM, "text"))
            {
                const auto TXT = stringField(L, ITEM, "text");
                if (!TXT)
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].text' must be a string", i);
                E.text = *TXT;
            }

            // A text item must be given a size: layout never measures fonts.
            if (E.type == SElement::eType::Text && !hasField(L, ITEM, "fill") && !hasField(L, ITEM, "width") && !hasField(L, ITEM, "height"))
                return raise(L, "hyprdecor.setup: 'defaults.elements[{}].text' needs 'fill' or an explicit 'width'/'height' "
                                "(layout does not measure fonts)", i);

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

            if (hasField(L, ITEM, "visible_when"))
            {
                if (!tableField(L, ITEM, "visible_when", std::format("defaults.elements[{}].visible_when", i).c_str()))
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].visible_when' must be a table", i);

                // Absolute index: tableField pushed the table and the loop reads fields
                // out of it, so a relative -1 would drift.
                const int WHEN = lua_gettop(L);

                // Each value must be a boolean. A wrong type is an error, never a silent
                // "false", because that would hide the item for no visible reason.
                static const std::pair<const char *, std::optional<bool> SElement::*> KEYS[] = {
                    {"focused", &SElement::whenFocused},       {"floating", &SElement::whenFloating},
                    {"maximised", &SElement::whenMaximised},   {"fullscreen", &SElement::whenFullscreen},
                    {"pinned", &SElement::whenPinned},
                };

                for (const auto &[NAME, MEMBER] : KEYS)
                {
                    if (!hasField(L, WHEN, NAME))
                        continue;
                    lua_getfield(L, WHEN, NAME);
                    if (!lua_isboolean(L, -1))
                    {
                        lua_pop(L, 1);
                        return raise(L, "hyprdecor.setup: 'defaults.elements[{}].visible_when.{}' must be a boolean", i, NAME);
                    }
                    E.*MEMBER = lua_toboolean(L, -1) != 0;
                    lua_pop(L, 1);
                }

                lua_pop(L, 1);
            }

            if (hasField(L, ITEM, "collapse"))
                E.collapse = boolField(L, ITEM, "collapse", false);

            if (hasField(L, ITEM, "interactive"))
                E.interactive = boolField(L, ITEM, "interactive", false);

            if (hasField(L, ITEM, "action"))
            {
                const auto ACTION = stringField(L, ITEM, "action");
                if (!ACTION)
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].action' must be a string", i);
                static const std::map<std::string, SElement::eAction> ACTIONS = {
                    {"close", SElement::eAction::Close},       {"maximize", SElement::eAction::Maximize}, {"fullscreen", SElement::eAction::Fullscreen},
                    {"pin", SElement::eAction::Pin},           {"float", SElement::eAction::Float},       {"minimize", SElement::eAction::Minimize},
                    {"retro_filter", SElement::eAction::RetroFilter},
                };

                const auto IT = ACTIONS.find(*ACTION);
                if (IT == ACTIONS.end())
                    return raise(L, "hyprdecor.setup: 'defaults.elements[{}].action' \"{}\" is not a known action "
                                    "(close, maximize, fullscreen, pin, float, minimize, retro_filter)", i, *ACTION);

                E.action = IT->second;

                // An action implies a hit target: a button you cannot hover is dead art.
                E.interactive = true;
            }

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

    // ---- themes ----------------------------------------------------------
    // themes = { name = { border = { texture, scale, linear_filtering, repeat,
    //                               thickness, inset } } }
    if (tableField(L, CFG, "themes", "themes"))
    {
        const int THEMES = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, THEMES))
        {
            if (!lua_isstring(L, -2) || !lua_istable(L, -1))
                return raise(L, "hyprdecor.setup: 'themes' keys must map to tables");

            const std::string NAME = lua_tostring(L, -2);
            const int THEME = lua_absindex(L, -1);
            STheme T;

            const char *BORDERKEY = hasField(L, THEME, "border") ? "border" : hasField(L, THEME, "frame") ? "frame" : nullptr;
            if (BORDERKEY)
            {
                if (!tableField(L, THEME, BORDERKEY, std::format("themes.{}.{}", NAME, BORDERKEY).c_str()))
                    return raise(L, "hyprdecor.setup: 'themes.{}' must be a table", NAME);

                const int BORDER = lua_gettop(L);

                if (hasField(L, BORDER, "texture"))
                {
                    const auto TEXTURE = stringField(L, BORDER, "texture");
                    if (!TEXTURE || TEXTURE->empty())
                        return raise(L, "hyprdecor.setup: 'themes.{}.{}.texture' must be a non-empty string", NAME, BORDERKEY);

                    const std::string ACTIVE = gPlugin->resolveTexturePath(*TEXTURE, {"_active", "_focused", "active", ""});
                    if (ACTIVE.empty())
                        return raise(L, "hyprdecor.setup: theme texture not found: \"{}\"", *TEXTURE);

                    T.textureBase = *TEXTURE;
                }

                if (hasField(L, BORDER, "scale"))
                {
                    const double TS = numberField(L, BORDER, "scale", 1);
                    if (TS < 1 || TS != (int)TS)
                        return raise(L, "hyprdecor.setup: 'themes.{}.{}.scale' must be a positive integer", NAME, BORDERKEY);
                    T.artScale = (int)TS;
                }
                if (hasField(L, BORDER, "linear_filtering"))
                    T.linearFiltering = boolField(L, BORDER, "linear_filtering", true);
                if (hasField(L, BORDER, "repeat"))
                    T.repeat = boolField(L, BORDER, "repeat", false);
                if (hasField(L, BORDER, "thickness") && !readThickness(L, BORDER, T.thickness))
                    return raise(L, "hyprdecor.setup: 'themes.{}.{}.thickness' must be a number or a table {{ left, top, right, bottom }}", NAME, BORDERKEY);
                if (hasField(L, BORDER, "inset") && !readThicknessNamed(L, BORDER, "inset", T.inset))
                    return raise(L, "hyprdecor.setup: 'themes.{}.{}.inset' must be a number or a table {{ left, top, right, bottom }}", NAME, BORDERKEY);

                lua_pop(L, 1); // BORDER
            }

            NEXT.themes[NAME] = T;
            lua_pop(L, 1); // value; key stays for lua_next
        }
        lua_pop(L, 1); // THEMES
    }

    // ---- rules -----------------------------------------------------------
    // rules = { { class = "kitty", theme = "terminal" }, ... }
    if (tableField(L, CFG, "rules", "rules"))
    {
        const int RULES = lua_gettop(L);
        const lua_Integer N = luaL_len(L, RULES);
        for (lua_Integer i = 1; i <= N; ++i)
        {
            lua_geti(L, RULES, i);
            if (!lua_istable(L, -1))
                return raise(L, "hyprdecor.setup: 'rules[{}]' must be a table", i);

            const int RULE = lua_absindex(L, -1);
            const auto CLASS = stringField(L, RULE, "class");
            const auto THEME = stringField(L, RULE, "theme");
            if (!CLASS || CLASS->empty())
                return raise(L, "hyprdecor.setup: 'rules[{}].class' must be a non-empty string", i);
            if (!THEME || THEME->empty())
                return raise(L, "hyprdecor.setup: 'rules[{}].theme' must be a non-empty string", i);
            if (!NEXT.themes.contains(*THEME))
                return raise(L, "hyprdecor.setup: 'rules[{}]' refers to unknown theme \"{}\"", i, *THEME);

            NEXT.rules.push_back({*CLASS, *THEME});
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // RULES
    }

    // ---- commit -----------------------------------------------------------
    // Only after everything validated: setup() applies completely or not at all.
    NEXT.applies = gPlugin->cfg.applies + 1;
    gPlugin->cfg = NEXT;

    // Was declared and reported by `status` but never assigned, so status always said
    // "setup() applied: no" even when the config had clearly taken effect.
    gPlugin->setup_applied = true;

    Log::logger->log(Log::DEBUG, std::format("{}setup(): texture=\"{}\" thickness L{} T{} R{} B{} artScale={} font=\"{}\" font_unfocused=\"{}\"",
                                             TAG, gPlugin->cfg.textureBase, gPlugin->cfg.thickness.left, gPlugin->cfg.thickness.top,
                                             gPlugin->cfg.thickness.right, gPlugin->cfg.thickness.bottom, gPlugin->cfg.artScale,
                                             gPlugin->cfg.titleFont, gPlugin->cfg.titleFontUnfocused));

    // Deferred: applying the configuration drives layout and damage, which must not
    // happen re-entrantly inside a Lua callback.
    gPlugin->requestUpdate();

    lua_settop(L, 0);
    lua_pushboolean(L, 1);
    return 1;
}
