#pragma once

// Lua-facing API for hyprdecor.
//
// Registers hl.plugin.hyprdecor.<name> callbacks via HyprlandAPI::addLuaFunction.
// Built against Lua 5.5, the runtime Hyprland itself links (see ROADMAP.md).

#include "config.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct lua_State;

namespace Hyprdecor::Lua {
    // `stringField`, `boolField` and `numberField` return the fallback only when the key is
    // absent. A key that is present with the wrong type throws SSetupError (ROADMAP D8).
    std::optional<std::string> stringField(lua_State *L, int tableIdx, const char *key);
    double      numberField(lua_State *L, int tableIdx, const char *key, double fallback);
    bool        boolField(lua_State *L, int tableIdx, const char *key, bool fallback);

    // Texture resolution is injected so parsing has no gPlugin/Hyprland dependency.
    struct SParseContext
    {
        std::function<std::string(const std::string &, const std::vector<std::string> &)> resolveTexture;
    };

    SConfig parseSetup(lua_State *L, const SParseContext &CTX);

    // Pushes t[key] when it is a table and returns true (caller must pop).
    // Raises a Lua error when the field exists but is not a table.
    bool tableField(lua_State *L, int tableIdx, const char *key, const char *path);

    // hl.plugin.hyprdecor.setup({...})
    int lSetup(lua_State *L);
}
