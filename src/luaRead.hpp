// Lua readers for the margin/spacing fields, with no Hyprland dependency so the tests
// exercise the SAME code setup() uses.
//
// This exists because the parser and its test had drifted: the test reimplemented the
// readers, so when `title.margin` silently dropped `left` and `right`, the test still
// passed. One implementation, tested directly, is the only way that stays honest.

#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <optional>
#include <string>

namespace LuaRead
{

// A spacing value that may legitimately be negative (a negative margin pulls an item
// outwards). nullopt means "not specified", which is different from 0.
struct SSided
{
    std::optional<double> left, top, right, bottom;

    bool any() const { return left || top || right || bottom; }
};

// Reads `container[key]` as either a number (all four sides) or
// { left, top, right, bottom }. Returns false when absent or malformed, so the caller
// can raise with the exact config path.
//
// A side present but not a number is a hard error, not a silent zero: a typo must never
// look like a deliberate 0.
inline bool readSided(lua_State *L, int containerIdx, const char *key, SSided &out, bool *malformed = nullptr)
{
    if (malformed)
        *malformed = false;

    lua_getfield(L, containerIdx, key);
    const int TYPE = lua_type(L, -1);

    if (TYPE == LUA_TNUMBER)
    {
        const double V = lua_tonumber(L, -1);
        lua_pop(L, 1);
        out = {V, V, V, V};
        return true;
    }

    if (TYPE != LUA_TTABLE)
    {
        lua_pop(L, 1);
        return false;
    }

    const int IDX = lua_absindex(L, -1);

    bool bad = false;
    auto SIDE = [&](const char *name) -> std::optional<double> {
        lua_getfield(L, IDX, name);
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            return std::nullopt;
        }
        if (!lua_isnumber(L, -1))
        {
            lua_pop(L, 1);
            bad = true;
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

    if (bad)
    {
        if (malformed)
            *malformed = true;
        return false;
    }

    return out.any();
}

}
