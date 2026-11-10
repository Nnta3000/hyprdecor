#define WLR_USE_UNSTABLE

#include <unistd.h>

#include <any>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleEffectContainer.hpp>

#include <algorithm>
#include <format>

#include "hyprWindowDecorator.hpp"
#include "plugin.hpp"
#include "compat.hpp"
#include "luaApi.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION()
{
    return HYPRLAND_API_VERSION;
}

static void onNewWindow([[maybe_unused]] void *self, std::any data)
{
    // data is guaranteed
    const auto PWINDOW = std::any_cast<PHLWINDOW>(data);

    if (!PWINDOW)
        return;

    // 0.57 removed m_X11DoesntWantBorders: suppression is now the window rule
    // `decorate`, which draw() already honours. Registering unconditionally is correct,
    // and the duplicate check below still prevents two of our decorations on one window.
    {
        if (std::ranges::any_of(PWINDOW->presentation().decorations(), [](const auto &d)
                                { return d && d->getDisplayName() == "Hyprdecor"; }))
        {
            Log::logger->log(Log::DEBUG, "[HYPRDECOR] window already has a hyprdecor decoration, skipping");
            return;
        }

        // 0.57 takes a SHARED pointer (decorations live in presentation() as SP<>).
        auto bar = makeShared<CHyprWindowDecorator>(PWINDOW);
        HyprlandAPI::addWindowDecoration(gPlugin->m_pHandle, PWINDOW, std::move(bar));

        // Track the window, never the decorator: the decorator is owned by Hyprland
        // and its address is not stable.
        gPlugin->m_vWindows.push_back(PWINDOW);

    }
}

// window.destroy carries a PHLWINDOWREF which may already be expired. Expired entries
// are pruned here; the live match is dropped by pointer comparison.
static void onWindowDestroyed([[maybe_unused]] void *self, PHLWINDOWREF ref)
{
    if (!gPlugin)
        return;

    const auto TARGET = ref.lock();
    std::erase_if(gPlugin->m_vWindows, [&](const auto &tracked)
                  {
                      const auto LOCKED = tracked.lock();
                      return !LOCKED || (TARGET && LOCKED == TARGET);
                  });
}

static void onPreConfigReload()
{
    gPlugin->m_vButtons.clear();
}

static void onUpdateWindowRules(PHLWINDOW window)
{
    if (auto bar = CPlugin::decoratorFor(window))
    {
        bar->updateRules();
        window->presentation().updateDecorations();
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle)
{
    gPlugin = std::make_unique<CPlugin>(handle);

    Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] loaded version {} ({} built {})", HYPRDECOR_VERSION, HYPRDECOR_GIT_COMMIT,
                                             HYPRDECOR_BUILD_DATE));

    const std::string HASH = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH)
    {
        HyprlandAPI::addNotification(gPlugin->m_pHandle, "[hyprdecor] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        gPlugin.reset();
        throw std::runtime_error("[hyprdecor] Version mismatch");
    }

    gPlugin->m_nobarRuleIdx = Desktop::Rule::windowEffects()->registerEffect("hyprdecor:no_bar");
    gPlugin->m_barColorRuleIdx = Desktop::Rule::windowEffects()->registerEffect("hyprdecor:bar_color");
    gPlugin->m_titleColorRuleIdx = Desktop::Rule::windowEffects()->registerEffect("hyprdecor:title_color");
    gPlugin->m_themeRuleIdx = Desktop::Rule::windowEffects()->registerEffect("hyprdecor:theme");

    // Structured Lua API: hl.plugin.hyprdecor.setup({...}).
    // Loading the .so does not create this namespace; only addLuaFunction does.
    HyprlandAPI::addLuaFunction(gPlugin->m_pHandle, "hyprdecor", "setup", Hyprdecor::Lua::lSetup);

    // Event::bus() replaced the deprecated registerCallbackDynamic. Listeners must be kept
    // alive for the plugin's lifetime, so they are stored on the plugin object.
    gPlugin->m_vListeners.emplace_back(Event::bus()->m_events.window.open.listen([&](PHLWINDOW w)
                                                                               { onNewWindow(nullptr, std::any(w)); }));
    gPlugin->m_vListeners.emplace_back(Event::bus()->m_events.window.updateRules.listen([&](PHLWINDOW w)
                                                                                      { onUpdateWindowRules(w); }));
    gPlugin->m_vListeners.emplace_back(Event::bus()->m_events.window.destroy.listen([&](PHLWINDOWREF r)
                                                                                      { onWindowDestroyed(nullptr, r); }));
    gPlugin->m_vListeners.emplace_back(Event::bus()->m_events.config.preReload.listen([]()
                                                                                     { onPreConfigReload(); }));
    gPlugin->m_vListeners.emplace_back(Event::bus()->m_events.config.reloaded.listen([]()
                                                                                     { gPlugin->update(); }));

    // Runs deferred work queued from Lua callbacks (CPlugin::requestUpdate()).
    gPlugin->m_vListeners.emplace_back(Event::bus()->m_events.tick.listen([]()
                                                                        { gPlugin->processPendingUpdate(); }));

    // Diagnostics: `hyprctl hyprdecor status`
    //
    // 0.57 replaced the old SHyprCtlCommand struct with IPC::Socket1::SCommand, whose
    // handler returns a SResponse built from the parsed SRequest, and whose match mode is
    // an enum rather than a bool.
    {
        IPC::Socket1::SCommand CMD;
        CMD.name = "hyprdecor";
        CMD.match = IPC::Socket1::COMMAND_MATCH_PREFIX;
        CMD.handler = [](const IPC::Socket1::SRequest &request) -> IPC::Socket1::SResponse {
            if (request.command.find("status") != std::string::npos || request.command.empty())
                return IPC::Socket1::SResponse(gPlugin ? gPlugin->statusString() : "hyprdecor not loaded\n");
            return IPC::Socket1::SResponse("usage: hyprctl hyprdecor status\n");
        };
        gPlugin->m_pStatusCommand = HyprlandAPI::registerHyprCtlCommand(gPlugin->m_pHandle, CMD);
    }

    // add deco to existing windows
    for (auto &w : Compat::windows())
    {
        if (w->isHidden() || !w->mapped())
            continue;

        onNewWindow(nullptr /* unused */, std::any(w));
    }

    // No reloadConfig() here: it re-enters the config manager while we are already
    // inside it, and setup() schedules the update itself. Just make sure one runs.
    gPlugin->requestUpdate();

    return {"hyprdecor", "Nine-patch window decorations", "CapsAdmin", HYPRDECOR_VERSION " (" HYPRDECOR_GIT_COMMIT ")"};
}

APICALL EXPORT void PLUGIN_EXIT()
{
    // Decorations are owned by Hyprland and their vtables point into this .so. They
    // MUST be removed before the library is unloaded, otherwise the next frame calls
    // into unmapped memory.
    if (gPlugin)
    {
        for (const auto &ref : gPlugin->m_vWindows)
        {
            const auto W = ref.lock();
            if (!W)
                continue;

            if (auto deco = CPlugin::decoratorFor(W))
                W->presentation().removeDecoration(deco);
        }
        gPlugin->m_vWindows.clear();
    }

    if (gPlugin) {
        for (auto &m : Compat::monitors()) {
            if (m)
                m->m_scheduledRecalc = true;
        }

        // Only touch a render pass if a render is actually in flight.
        if (g_pHyprRenderer)
            Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).removeAllOfType("CRenderPassElement");

        Desktop::Rule::windowEffects()->unregisterEffect(gPlugin->m_barColorRuleIdx);
        Desktop::Rule::windowEffects()->unregisterEffect(gPlugin->m_titleColorRuleIdx);
        Desktop::Rule::windowEffects()->unregisterEffect(gPlugin->m_themeRuleIdx);
        Desktop::Rule::windowEffects()->unregisterEffect(gPlugin->m_nobarRuleIdx);
    }

    gPlugin.reset();
}
