#pragma once

// Lua-facing API for hyprdecor.
//
// Registers hl.plugin.hyprdecor.<name> callbacks via HyprlandAPI::addLuaFunction.
// Built against Lua 5.5, the runtime Hyprland itself links (see ROADMAP.md).

struct lua_State;

namespace Hyprdecor::Lua {
    // Reads a struct field. Returns nullptr when absent or not a string.
    const char *stringField(lua_State *L, int tableIdx, const char *key);
    double      numberField(lua_State *L, int tableIdx, const char *key, double fallback);

    // Pushes t[key] when it is a table and returns true (caller must pop).
    // Raises a Lua error when the field exists but is not a table.
    bool tableField(lua_State *L, int tableIdx, const char *key, const char *path);

    // hl.plugin.hyprdecor.setup({...})
    int lSetup(lua_State *L);
}
