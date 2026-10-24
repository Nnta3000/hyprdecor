#include "luaApi.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include "plugin.hpp"
#include "hyprWindowDecorator.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <format>
#include <string>

using namespace Hyprdecor::Lua;

// Hyprland's Lua config manager runs Lua 5.5; building against 5.4 headers and
// calling into a 5.5 state is memory corruption, so pin the version at compile time.
static_assert(LUA_VERSION_NUM == 505, "hyprdecor must be built against Lua 5.5 (the runtime Hyprland links)");

namespace {
    constexpr const char *TAG = "[HYPRDECOR] ";

    // Raises a Lua error. This aborts the rest of the config evaluation, which is
    // deliberate: a half-applied decoration is worse than a loud failure.
    int raise(lua_State *L, const std::string &message)
    {
        lua_pushlstring(L, message.data(), message.size());
        return lua_error(L);
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

const char *Hyprdecor::Lua::stringField(lua_State *L, int tableIdx, const char *key)
{
    if (!lua_istable(L, tableIdx))
        return nullptr;

    lua_getfield(L, tableIdx, key);
    if (!lua_isstring(L, -1))
    {
        lua_pop(L, 1);
        return nullptr;
    }

    // Copy before popping: never hand out a pointer into Lua's string table.
    thread_local std::string scratch;
    scratch = lua_tostring(L, -1);
    lua_pop(L, 1);
    return scratch.c_str();
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
        raise(L, std::format("hyprdecor.setup: '{}' must be a table", path));

    lua_getfield(L, tableIdx, key);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }

    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        raise(L, std::format("hyprdecor.setup: '{}' must be a table", path));
    }

    return true;
}

// hl.plugin.hyprdecor.setup({ defaults = { border = {...} } })
//
// Every documented field is validated. Anything missing or malformed is a hard
// error: setup() either applies completely or not at all.
int Hyprdecor::Lua::lSetup(lua_State *L)
{
    if (lua_gettop(L) < 1 || !lua_istable(L, 1))
        return raise(L, "hyprdecor.setup: expected a table argument");

    const int CFG = lua_absindex(L, 1);

    lua_getfield(L, CFG, "defaults");
    if (lua_isnil(L, -1))
        return raise(L, "hyprdecor.setup: missing required table 'defaults'");

    const int DEFAULTS = lua_gettop(L);
    if (!lua_istable(L, DEFAULTS))
        return raise(L, "hyprdecor.setup: 'defaults' must be a table");

    if (!tableField(L, DEFAULTS, "border", "defaults.border"))
        return raise(L, "hyprdecor.setup: missing required table 'defaults.border'");

    const int BORDER = lua_gettop(L);

    if (!hasField(L, BORDER, "texture"))
        return raise(L, "hyprdecor.setup: 'defaults.border.texture' is required");

    const char *TEXTURE = stringField(L, BORDER, "texture");
    if (!TEXTURE)
        return raise(L, "hyprdecor.setup: 'defaults.border.texture' must be a string");

    // Copy immediately: the temporary lives in a reused scratch buffer.
    const std::string BASE(TEXTURE);

    if (BASE.empty())
        return raise(L, "hyprdecor.setup: 'defaults.border.texture' must not be empty");

    Log::logger->log(Log::DEBUG, std::format("{}setup(): this={} received texture=\"{}\"", TAG, (void *)gPlugin.get(), BASE));

    // Validate the file now, so a typo fails at config time instead of silently
    // leaving windows undecorated.
    const std::string ACTIVE = gPlugin->resolveTexturePath(BASE, {"_active", "_focused", "active", ""});
    if (ACTIVE.empty())
    {
        if (!gPlugin->m_szLastResolveError.empty())
            return raise(L, std::format("hyprdecor.setup: cannot access texture \"{}\": {}",
                                        BASE, gPlugin->m_szLastResolveError));

        return raise(L, std::format("hyprdecor.setup: texture not found: \"{}\" "
                                    "(tried {}_active.png, {}_focused.png, {}active.png, {}.png). "
                                    "Check that the path is absolute and that the compositor's user can read it.",
                                    BASE, BASE, BASE, BASE, BASE));
    }

    if (!hasField(L, BORDER, "thickness"))
        return raise(L, "hyprdecor.setup: 'defaults.border.thickness' is required");

    const double THICKNESS = numberField(L, BORDER, "thickness", -1);
    if (THICKNESS < 0)
        return raise(L, "hyprdecor.setup: 'defaults.border.thickness' must be a non-negative number");

    const double ALPHA = numberField(L, BORDER, "alpha", 1.0);
    if (ALPHA < 0.0 || ALPHA > 1.0)
        return raise(L, "hyprdecor.setup: 'defaults.border.alpha' must be between 0.0 and 1.0");

    // Commit only after everything validated.
    gPlugin->ninepatch_texture = BASE;
    gPlugin->setup_border_thickness = (int)THICKNESS;
    gPlugin->setup_border_alpha = (float)ALPHA;
    gPlugin->setup_applied = true;

    Log::logger->log(Log::DEBUG, std::format("{}setup(): this={} committed texture=\"{}\" resolved=\"{}\" thickness={} alpha={}", TAG, (void *)gPlugin.get(), BASE, ACTIVE, (int)THICKNESS, ALPHA));

    // Deferred: applying the configuration drives layout and damage, which must
    // not happen re-entrantly inside a Lua callback.
    gPlugin->requestUpdate();

    lua_settop(L, 0);
    lua_pushboolean(L, 1);
    return 1;
}
