#include "luaApi.hpp"
#include "plugin.hpp"

#include <cstdio>
#include <format>
#include <stdexcept>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

int Hyprdecor::Lua::lSetup(lua_State *L)
{
    // lua_error() longjmps and would skip the destructors of anything still alive in this
    // frame, so the message is copied into a plain buffer and every C++ object (including
    // the caught exception) is destroyed before the Lua error is raised.
    char message[1024] = {};

    try
    {
        SParseContext CTX;
        CTX.resolveTexture = [](const std::string &base, const std::vector<std::string> &suffixes) {
            return gPlugin->resolveTexturePath(base, suffixes);
        };

        SConfig NEXT = parseSetup(L, CTX);
        NEXT.applies = gPlugin->cfg.applies + 1;
        gPlugin->cfg = NEXT;
        gPlugin->setup_applied = true;

        Log::logger->log(Log::DEBUG, std::format("{}setup(): texture=\"{}\" thickness L{} T{} R{} B{} artScale={} font=\"{}\" font_unfocused=\"{}\"",
                                                 "[HYPRDECOR] ", gPlugin->cfg.textureBase, gPlugin->cfg.thickness.left, gPlugin->cfg.thickness.top,
                                                 gPlugin->cfg.thickness.right, gPlugin->cfg.thickness.bottom, gPlugin->cfg.artScale,
                                                 gPlugin->cfg.titleFont, gPlugin->cfg.titleFontUnfocused));

        // Deferred: applying the configuration drives layout and damage, which must not
        // happen re-entrantly inside a Lua callback.
        gPlugin->requestUpdate();

        lua_settop(L, 0);
        lua_pushboolean(L, 1);
        return 1;
    }
    catch (const std::exception &e)
    {
        std::snprintf(message, sizeof(message), "%s", e.what());
    }
    catch (...)
    {
        std::snprintf(message, sizeof(message), "hyprdecor.setup: unexpected internal error");
    }

    lua_pushstring(L, message);
    return lua_error(L);
}
