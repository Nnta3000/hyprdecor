#include "renderPassElement.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include "hyprWindowDecorator.hpp"

CRenderPassElement::CRenderPassElement(const CRenderPassElement::SBarData &data_) : data(data_)
{
    ;
}

std::vector<UP<IPassElement>> CRenderPassElement::draw()
{
    // 0.56: the monitor being rendered lives in the renderer's render data.
    if (data.deco && g_pHyprRenderer)
        data.deco->renderPass(g_pHyprRenderer->m_renderData.pMonitor.lock(), data.a);

    return {};
}

bool CRenderPassElement::needsLiveBlur()
{
    if (!data.deco || !gPlugin)
        return false;

    // Never assume the config value exists: getConfigValue() returns nullptr when
    // the option is unregistered or the config manager is not committed yet, and
    // this runs on every frame.
    const auto VALUE = HyprlandAPI::getConfigValue(gPlugin->m_pHandle, "decoration:blur:enabled");
    if (!VALUE)
        return false;

    const auto PTR = (Hyprlang::INT *const *)VALUE->getDataStaticPtr();
    if (!PTR || !*PTR)
        return false;

    if (!**PTR)
        return false;

    CHyprColor color = data.deco->m_bForcedBarColor.value_or(gPlugin->bar_color);
    color.a *= data.a;
    return gPlugin->bar_blur && color.a < 1.F;
}

std::optional<CBox> CRenderPassElement::boundingBox()
{
    if (!data.deco || !g_pHyprRenderer)
        return std::nullopt;

    const auto PMONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!PMONITOR)
        return std::nullopt;

    // Temporary fix: expand the bar bb a bit, otherwise occlusion gets too aggressive.
    return data.deco->assignedBoxGlobal().translate(-PMONITOR->m_position).expand(10);
}

bool CRenderPassElement::needsPrecomputeBlur()
{
    return false;
}
