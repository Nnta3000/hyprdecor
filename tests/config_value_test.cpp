// Unit tests for how setup() reads Lua values.
//
// The bug this guards against: booleans were read with a number-only helper, so
// every `enabled = true/false` in a Lua config was silently ignored and the default
// was used instead. The parser and the Lua value handling are therefore tested
// directly, with a real Lua state.

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cmath>
#include <cstdio>
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

// --- copies of the readers in src/luaApi.cpp (kept in sync deliberately: they are
// --- tiny and testing them requires no Hyprland types) ---------------------------

static bool boolField(lua_State *L, int tableIdx, const char *key, bool fallback)
{
    if (!lua_istable(L, tableIdx))
        return fallback;

    lua_getfield(L, tableIdx, key);

    bool VALUE = fallback;
    if (lua_isboolean(L, -1))
        VALUE = lua_toboolean(L, -1) != 0;
    else if (lua_isnumber(L, -1))
        VALUE = lua_tonumber(L, -1) != 0;

    lua_pop(L, 1);
    return VALUE;
}

static double numberField(lua_State *L, int tableIdx, const char *key, double fallback)
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

static void run(lua_State *L, const std::string &code)
{
    lua_settop(L, 0);
    if (luaL_dostring(L, code.c_str()) != LUA_OK)
    {
        std::printf("  FAIL: lua error: %s\n", lua_tostring(L, -1));
        ++g_failures;
        lua_settop(L, 0);
    }
}

static void testBoolField()
{
    std::printf("\n== boolField: Lua booleans and numbers\n");

    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    run(L, "cfg = { v = false }");
    lua_getglobal(L, "cfg");
    CHECK(boolField(L, lua_gettop(L), "v", true) == false, "false must stay false (was falling back to the default)");

    run(L, "cfg = { v = true }");
    lua_getglobal(L, "cfg");
    CHECK(boolField(L, lua_gettop(L), "v", false) == true, "true must stay true");

    run(L, "cfg = { v = 0 }");
    lua_getglobal(L, "cfg");
    CHECK(boolField(L, lua_gettop(L), "v", true) == false, "0 must mean false");

    run(L, "cfg = { v = 1 }");
    lua_getglobal(L, "cfg");
    CHECK(boolField(L, lua_gettop(L), "v", false) == true, "1 must mean true");

    run(L, "cfg = { }");
    lua_getglobal(L, "cfg");
    CHECK(boolField(L, lua_gettop(L), "v", true) == true, "absent keeps the fallback (true)");

    run(L, "cfg = { v = 'yes' }");
    lua_getglobal(L, "cfg");
    CHECK(boolField(L, lua_gettop(L), "v", true) == true, "a string is not a boolean; fallback wins");

    lua_close(L);
}

static void testNumberField()
{
    std::printf("\n== numberField: scale validation inputs\n");

    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    run(L, "cfg = { scale = 2 }");
    lua_getglobal(L, "cfg");
    CHECK(numberField(L, lua_gettop(L), "scale", 1) == 2, "integer scale");

    run(L, "cfg = { scale = 1.5 }");
    lua_getglobal(L, "cfg");
    CHECK(std::abs(numberField(L, lua_gettop(L), "scale", 1) - 1.5) < 1e-9, "fractional scale must be visible to validation");

    run(L, "cfg = { scale = true }");
    lua_getglobal(L, "cfg");
    CHECK(numberField(L, lua_gettop(L), "scale", 7) == 7, "a boolean is not a number; fallback wins");

    lua_close(L);
}

int main()
{
    std::printf("hyprdecor config-value tests\n");
    testBoolField();
    testNumberField();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
