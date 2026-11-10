// Tests the pure Lua parser: strict types, integer ranges and atomic validation.
// Links only config.hpp/luaApi.hpp and Lua 5.5; no Hyprland.

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include "luaApi.hpp"

#include <cstdio>
#include <functional>
#include <string>

static int failures = 0;

static void expectOk(const char *name, const char *lua, const std::function<void(const SConfig &)> &check = {})
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    if (luaL_dostring(L, lua) != LUA_OK)
    {
        std::printf("FAIL %s: Lua error: %s\n", name, lua_tostring(L, -1));
        ++failures;
        lua_close(L);
        return;
    }

    Hyprdecor::Lua::SParseContext CTX;
    CTX.resolveTexture = [](const std::string &base, const std::vector<std::string> &) {
        return base == "missing" ? std::string{} : base + ".png";
    };

    try
    {
        const SConfig CFG = Hyprdecor::Lua::parseSetup(L, CTX);
        if (check)
            check(CFG);
    }
    catch (const std::exception &e)
    {
        std::printf("FAIL %s: unexpected error: %s\n", name, e.what());
        ++failures;
    }

    lua_close(L);
}

static void expectError(const char *name, const char *lua)
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    if (luaL_dostring(L, lua) != LUA_OK)
    {
        std::printf("FAIL %s: Lua error: %s\n", name, lua_tostring(L, -1));
        ++failures;
        lua_close(L);
        return;
    }

    Hyprdecor::Lua::SParseContext CTX;
    CTX.resolveTexture = [](const std::string &base, const std::vector<std::string> &) {
        return base == "missing" ? std::string{} : base + ".png";
    };

    try
    {
        (void)Hyprdecor::Lua::parseSetup(L, CTX);
        std::printf("FAIL %s: expected an error\n", name);
        ++failures;
    }
    catch (const std::exception &)
    {
        // expected
    }

    lua_close(L);
}

int main()
{
    expectOk("minimal", R"(
        return { defaults = { border = { texture = "frame" } } }
    )", [](const SConfig &C) {
        if (C.textureBase != "frame")
            std::printf("FAIL minimal: texture\n"), ++failures;
        if (C.titleSize != 10)
            std::printf("FAIL minimal: title size default\n"), ++failures;
    });

    expectOk("valid values", R"(
        return { defaults = {
            scale = 2,
            border = { texture = "frame", linear_filtering = false },
            title = { size = 12, enabled = true, font = "Sans" },
            bar = { color = 0x88333333, padding = 7 },
        } }
    )");

    expectError("wrong bool type", R"(
        return { defaults = { border = { texture = "frame" }, title = { enabled = "yes" } } }
    )");
    expectError("wrong number type", R"(
        return { defaults = { border = { texture = "frame" }, title = { size = "big" } } }
    )");
    expectError("wrong string type", R"(
        return { defaults = { border = { texture = "frame" }, title = { font = 12 } } }
    )");
    expectError("fractional int", R"(
        return { defaults = { border = { texture = "frame" }, title = { size = 1.5 } } }
    )");
    expectError("out-of-range int", R"(
        return { defaults = { border = { texture = "frame" }, title = { size = 1000000000 } } }
    )");
    expectError("fractional scale", R"(
        return { defaults = { scale = 1.5, border = { texture = "frame" } } }
    )");
    expectError("negative colour", R"(
        return { defaults = { border = { texture = "frame" }, bar = { color = -1 } } }
    )");
    expectError("oversized colour", R"(
        return { defaults = { border = { texture = "frame" }, bar = { color = 4294967296 } } }
    )");
    expectError("unknown action", R"(
        return { defaults = { border = { texture = "frame" }, elements = {
            { image = "eye.png", action = "nope" }
        } } }
    )");
    expectError("missing texture", R"(
        return { defaults = { border = { texture = "missing" } } }
    )");

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
