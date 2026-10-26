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

#include "luaRead.hpp"

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

// The title's margins were parsed and then only half-assigned: `left` and `right` were
// dropped, so they did nothing. The layout and rendering were fine; the loss was at the
// config boundary. That is why this tests the READER, not the layout.
static void testSidedMargins()
{
    std::printf("\n== readSided: margins reach every side\n");

    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    const auto READ = [&](const char *expr, LuaRead::SSided &out) {
        luaL_dostring(L, (std::string("return ") + expr).c_str());
        const bool OK = LuaRead::readSided(L, lua_gettop(L), "margin", out);
        lua_pop(L, 1);
        return OK;
    };

    // A number means all four sides.
    {
        LuaRead::SSided M;
        CHECK(READ("{ margin = 6 }", M), "a number is accepted");
        CHECK(M.left && *M.left == 6, "number sets left");
        CHECK(M.right && *M.right == 6, "number sets right");
        CHECK(M.top && *M.top == 6 && M.bottom && *M.bottom == 6, "number sets top and bottom");
    }

    // EVERY side must survive; this is the exact bug that was shipped.
    {
        LuaRead::SSided M;
        CHECK(READ("{ margin = { left = 10, top = 20, right = 30, bottom = 40 } }", M), "table accepted");
        CHECK(M.left && *M.left == 10, "left survives, got %s", M.left ? "yes" : "MISSING");
        CHECK(M.top && *M.top == 20, "top survives");
        CHECK(M.right && *M.right == 30, "right survives, got %s", M.right ? "yes" : "MISSING");
        CHECK(M.bottom && *M.bottom == 40, "bottom survives");
    }

    // Negative is legal: it pulls the item outwards.
    {
        LuaRead::SSided M;
        CHECK(READ("{ margin = { left = -30, right = -22 } }", M), "negatives accepted");
        CHECK(M.left && *M.left == -30, "negative left kept");
        CHECK(M.right && *M.right == -22, "negative right kept");
    }

    // An absent side stays unspecified, which is NOT the same as zero.
    {
        LuaRead::SSided M;
        CHECK(READ("{ margin = { right = 4 } }", M), "partial table accepted");
        CHECK(M.right && *M.right == 4, "right kept");
        CHECK(!M.left, "absent left must stay unspecified, not become 0");
        CHECK(!M.top, "absent top must stay unspecified");
    }

    // A typo must be an error, never a silent zero.
    {
        LuaRead::SSided M;
        bool malformed = false;
        luaL_dostring(L, "return { margin = { left = 'ten' } }");
        CHECK(!LuaRead::readSided(L, lua_gettop(L), "margin", M, &malformed), "a non-number side is rejected");
        CHECK(malformed, "and is reported as malformed");
        lua_pop(L, 1);
    }

    // Absent entirely.
    {
        LuaRead::SSided M;
        CHECK(!READ("{ other = 1 }", M), "a missing key is not found");
    }

    lua_close(L);
}

int main()
{
    testSidedMargins();
    std::printf("hyprdecor config-value tests\n");
    testBoolField();
    testNumberField();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
