#define WLR_USE_UNSTABLE

#include <unistd.h>

#include <any>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleEffectContainer.hpp>

#include <algorithm>

#include "hyprWindowDecorator.hpp"
#include "plugin.hpp"
#include "compat.hpp"
#include "luaApi.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION()
{
    return HYPRLAND_API_VERSION;
}

static void onNewWindow(void *self, std::any data)
{
    // data is guaranteed
    const auto PWINDOW = std::any_cast<PHLWINDOW>(data);

    if (!PWINDOW)
        return;

    if (!PWINDOW->m_X11DoesntWantBorders)
    {
        if (std::ranges::any_of(PWINDOW->m_windowDecorations, [](const auto &d)
                                { return d && d->getDisplayName() == "Hyprdecor"; }))
        {
            Log::logger->log(Log::DEBUG, "[HYPRDECOR] window already has a hyprdecor decoration, skipping");
            return;
        }

        auto bar = makeUnique<CHyprWindowDecorator>(PWINDOW);
        auto barRaw = bar.get();
        barRaw->m_self = barRaw;
        HyprlandAPI::addWindowDecoration(gPlugin->m_pHandle, PWINDOW, std::move(bar));

        // Track the window, never the decorator: the decorator is owned by Hyprland
        // and its address is not stable.
        gPlugin->m_vWindows.push_back(PWINDOW);

        // NOTE: do NOT force a relayout here. The window is already being laid out
        // with the decoration's extents when it opens; calling recalculateMonitor()
        // a frame later produced the "window fills the output" state.
    }
}

static void onCloseWindow(void *self, std::any data)
{
    // data is guaranteed
    const auto PWINDOW = std::any_cast<PHLWINDOW>(data);

    // Hyprland destroys the window's decorations itself. Only stop tracking the
    // window; never dereference a decorator pointer that may already be freed.
    std::erase_if(gPlugin->m_vWindows, [PWINDOW](const auto &ref)
                  { return ref.lock() == PWINDOW; });
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
        window->updateWindowDecos();
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle)
{
    gPlugin = std::make_unique<CPlugin>(handle);

    const std::string HASH = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH)
    {
        HyprlandAPI::addNotification(gPlugin->m_pHandle, "[hyprdecor] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hb] Version mismatch");
    }

    gPlugin->m_nobarRuleIdx = Desktop::Rule::windowEffects()->registerEffect("hyprdecor:no_bar");
    gPlugin->m_barColorRuleIdx = Desktop::Rule::windowEffects()->registerEffect("hyprdecor:bar_color");
    gPlugin->m_titleColorRuleIdx = Desktop::Rule::windowEffects()->registerEffect("hyprdecor:title_color");

    // Structured Lua API: hl.plugin.hyprdecor.setup({...}).
    // Loading the .so does not create this namespace; only addLuaFunction does.
    HyprlandAPI::addLuaFunction(gPlugin->m_pHandle, "hyprdecor", "setup", Hyprdecor::Lua::lSetup);

    // Event::bus() replaced the deprecated registerCallbackDynamic. Listeners must be kept
    // alive for the plugin's lifetime, so they are stored on the plugin object.
    gPlugin->m_vListeners.emplace_back(Event::bus()->m_events.window.open.listen([&](PHLWINDOW w)
                                                                               { onNewWindow(nullptr, std::any(w)); }));
    gPlugin->m_vListeners.emplace_back(Event::bus()->m_events.window.updateRules.listen([&](PHLWINDOW w)
                                                                                      { onUpdateWindowRules(w); }));
    gPlugin->m_vListeners.emplace_back(Event::bus()->m_events.config.preReload.listen([]()
                                                                                     { onPreConfigReload(); }));
    gPlugin->m_vListeners.emplace_back(Event::bus()->m_events.config.reloaded.listen([]()
                                                                                     { gPlugin->update(); }));

    // Runs deferred work queued from Lua callbacks (CPlugin::requestUpdate()).
    gPlugin->m_vListeners.emplace_back(Event::bus()->m_events.tick.listen([]()
                                                                        { gPlugin->processPendingUpdate(); }));

    // Diagnostics: `hyprctl hyprdecor status`
    static auto PSTATUS = HyprlandAPI::registerHyprCtlCommand(gPlugin->m_pHandle, SHyprCtlCommand{
                                                                                    .name = "hyprdecor",
                                                                                    .exact = false,
                                                                                    .fn = [](eHyprCtlOutputFormat, std::string arg) -> std::string {
                                                                                        if (arg.find("status") != std::string::npos || arg.empty())
                                                                                            return gPlugin ? gPlugin->statusString() : "hyprdecor not loaded\n";
                                                                                        return "usage: hyprctl hyprdecor status\n";
                                                                                    },
                                                                                });

    // add deco to existing windows
    for (auto &w : Compat::windows())
    {
        if (w->isHidden() || !w->m_isMapped)
            continue;

        onNewWindow(nullptr /* unused */, std::any(w));
    }

    // reloadConfig() emits config.reloaded, but that fires while the config is still
    // being evaluated, i.e. before hl.plugin.hyprdecor.setup() in the same file.
    // Relying on it alone left every update()-owned value uninitialised. Schedule a
    // deferred update that waits for the values to be committed.
    HyprlandAPI::reloadConfig();
    gPlugin->requestUpdate();

    return {"hyprdecor", "A plugin to add title bars to windows.", "Vaxry", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT()
{
    if (gPlugin) {
        for (auto &m : Compat::monitors()) {
            if (m)
                m->m_scheduledRecalc = true;
        }

        // Only touch a render pass if a render is actually in flight.
        if (g_pHyprRenderer)
            g_pHyprRenderer->currentPass().removeAllOfType("CRenderPassElement");

        Desktop::Rule::windowEffects()->unregisterEffect(gPlugin->m_barColorRuleIdx);
        Desktop::Rule::windowEffects()->unregisterEffect(gPlugin->m_titleColorRuleIdx);
        Desktop::Rule::windowEffects()->unregisterEffect(gPlugin->m_nobarRuleIdx);
    }

    gPlugin.reset();
}
