#include "renderPassElement.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include "hyprWindowDecorator.hpp"
#include <hyprland/src/config/ConfigManager.hpp>

CRenderPassElement::CRenderPassElement(const CRenderPassElement::SBarData &data_) : data(data_)
{
    ;
}

std::vector<UP<IPassElement>> CRenderPassElement::draw(Render::CRenderContext &ctx)
{
    // 0.57: the monitor being rendered lives in the context handed to draw(), not in a
    // global renderer field. Keeping it here means every draw call below can pass `ctx`
    // straight through.
    if (data.deco && g_pHyprRenderer)
        data.deco->renderPass(ctx, data.pMonitor, data.a);

    return {};
}

bool CRenderPassElement::needsLiveBlur([[maybe_unused]] Render::CRenderContext &ctx)
{
    if (!data.deco || !gPlugin)
        return false;

    // Never assume the config value exists: getConfigValue() returns nullptr when
    // the option is unregistered or the config manager is not committed yet, and
    // this runs on every frame.
    // 0.57 deprecates HyprlandAPI::getConfigValue in favour of the config manager's
    // typed reply, which also avoids Hyprlang internals leaking into the plugin.
    const auto REPLY = Config::mgr()->getConfigValue("decoration:blur:enabled");
    if (!REPLY.dataptr || !*REPLY.dataptr)
        return false;

    if (!*(const bool *)*REPLY.dataptr)
        return false;

    CHyprColor color = data.deco->m_bForcedBarColor.value_or(gPlugin->bar_color);
    color.a *= data.a;
    return gPlugin->bar_blur && color.a < 1.F;
}

std::optional<CBox> CRenderPassElement::boundingBox(Render::CRenderContext &ctx)
{
    if (!data.deco || !g_pHyprRenderer)
        return std::nullopt;

    const auto PMONITOR = ctx.m_data.pMonitor.lock();
    if (!PMONITOR)
        return std::nullopt;

    // Temporary fix: expand the bar bb a bit, otherwise occlusion gets too aggressive.
    return data.deco->assignedBoxGlobal().translate(-PMONITOR->m_position).expand(10);
}

bool CRenderPassElement::needsPrecomputeBlur([[maybe_unused]] Render::CRenderContext &ctx)
{
    return false;
}
