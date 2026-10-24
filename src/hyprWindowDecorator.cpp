#include "hyprWindowDecorator.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
// m_exclusiveLSes is private in InputManager. The macro must stay scoped to this
// translation unit: putting it in a header would rewrite InputManager.hpp for every
// includer.
#define private public
#include <hyprland/src/managers/input/InputManager.hpp>
#undef private
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <libdrm/drm_fourcc.h>
#include <pango/pangocairo.h>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "renderPassElement.hpp"
#include "plugin.hpp"
#include "compat.hpp"
#include "util.hpp"

CHyprWindowDecorator::CHyprWindowDecorator(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow)
{
    m_pWindow = pWindow;

    const auto PMONITOR = pWindow->m_monitor.lock();
    PMONITOR->m_scheduledRecalc = true;

    m_pAppIconTex = makeShared<Render::GL::CGLTexture>();

    // button events: Event::bus() typed signals replace the deprecated dynamic callbacks.
    // Each listener is owned by the event bus and removed when the plugin unloads.
    m_pMouseButtonCallback = Event::bus()->m_events.input.mouse.button.listen(
        [&](IPointer::SButtonEvent e, Event::SCallbackInfo &info)
        { onMouseButton(info, e); });
    m_pTouchDownCallback = Event::bus()->m_events.input.touch.down.listen(
        [&](ITouch::SDownEvent e, Event::SCallbackInfo &info)
        { onTouchDown(info, e); });
    m_pTouchUpCallback = Event::bus()->m_events.input.touch.up.listen(
        [&](ITouch::SUpEvent e, Event::SCallbackInfo &info)
        { onTouchUp(info, e); });

    // move events
    m_pTouchMoveCallback = Event::bus()->m_events.input.touch.motion.listen(
        [&](ITouch::SMotionEvent e, Event::SCallbackInfo &info)
        { onTouchMove(info, e); });
    m_pMouseMoveCallback = Event::bus()->m_events.input.mouse.move.listen(
        [&](Vector2D coords, [[maybe_unused]] Event::SCallbackInfo &info)
        { onMouseMove(coords); });

    m_pTextTex = makeShared<Render::GL::CGLTexture>();
    m_pButtonsTex = makeShared<Render::GL::CGLTexture>();

    m_pAppIconTex = makeShared<Render::GL::CGLTexture>();
    m_pBarFinalTex = makeShared<Render::GL::CGLTexture>();

    Animation::mgr()->createAnimation(gPlugin->bar_color, m_cRealBarColor, Config::animationTree()->getAnimationPropertyConfig("border"), pWindow, AVARDAMAGE_NONE);
    m_cRealBarColor->setUpdateCallback([&](auto)
                                       { damageEntire(); });
}

CHyprWindowDecorator::~CHyprWindowDecorator()
{
    // Nothing to unregister: CPlugin tracks windows, not decorators, and Hyprland
    // owns this object. Event listeners are released with their shared pointers.
}

// The one place that decides how much space this decoration reserves. Layout, drawing
// and hit-testing must all agree, otherwise the reserved area and the drawn frame
// disagree and clicks land in the wrong place.
SBoxExtents CHyprWindowDecorator::currentExtents() const
{
    if (!gPlugin->enabled || m_hidden)
        return {{0, 0}, {0, 0}};

    // Reserved space = frame art thickness + the user's inset. The nine-patch is
    // stretched over this same box (see assignedBoxGlobal), so the extra space is
    // filled with frame art, never left as a hole.
    return {{(double)(gPlugin->decoration_offset_left + gPlugin->inset_left),
             (double)(gPlugin->decoration_offset_top + gPlugin->inset_top)},
            {(double)(gPlugin->decoration_offset_right + gPlugin->inset_right),
             (double)(gPlugin->decoration_offset_bottom + gPlugin->inset_bottom)}};
}

SDecorationPositioningInfo CHyprWindowDecorator::getPositioningInfo()
{
    SDecorationPositioningInfo info;
    // A window frame surrounds the client on all four sides, so it must use ABSOLUTE.
    // STICKY is documented as "one edge allowed ... any other edge than the one
    // selected is ignored", which is why a STICKY decoration only ever received a top
    // strip of geometry (a title bar) instead of a rectangle around the window.
    info.policy = DECORATION_POSITION_ABSOLUTE;
    info.edges = DECORATION_EDGE_TOP | DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT;
    info.priority = gPlugin->bar_precedence_over_border ? 10005 : 5000;
    // Always reserve: the frame surrounds the window, and `inset` is real space, not a
    // switch. (The old decoration_inset flag meant "reserve nothing".)
    info.reserved = true;

    info.desiredExtents = currentExtents();

    // getPositioningInfo() is called many times per frame, per window: log only when
    // the values actually change, otherwise this floods the log and costs time on the
    // compositor's hot path.
    if (const auto W = m_pWindow.lock())
    {
        const auto WSIZE = W->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        if (WSIZE != m_vLastLoggedSize)
        {
            m_vLastLoggedSize = WSIZE;
            Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] positioning: window {}x{} reserved L{} T{} R{} B{} (policy {})",
                                                     WSIZE.x, WSIZE.y,
                                                     info.desiredExtents.topLeft.x, info.desiredExtents.topLeft.y,
                                                     info.desiredExtents.bottomRight.x, info.desiredExtents.bottomRight.y,
                                                     info.policy == DECORATION_POSITION_ABSOLUTE ? "ABSOLUTE" : "STICKY"));
        }
    }

    return info;
}

void CHyprWindowDecorator::onPositioningReply(const SDecorationPositioningReply &reply)
{
    if (reply.assignedGeometry.size() != m_bAssignedBox.size())
    {
        m_bWindowSizeChanged = true;
        m_bNeedsReposition = true;
    }

    m_bAssignedBox = reply.assignedGeometry;
}

std::string CHyprWindowDecorator::getDisplayName()
{
    return "Hyprdecor";
}

bool CHyprWindowDecorator::inputIsValid()
{
    if (!gPlugin->enabled)
        return false;

    if (!m_pWindow->m_workspace || !m_pWindow->m_workspace->isVisible() || !g_pInputManager->m_exclusiveLSes.empty() ||
        (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(m_pWindow->wlSurface()->resource())))
        return false;

    const auto HITTESTER = Desktop::viewState()->hitTest();
    const auto CURSOR = g_pInputManager->getMouseCoordsInternal();

    const auto WINDOWATCURSOR = HITTESTER.windowAt(CURSOR, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

    auto focusState = Desktop::focusState();
    auto window = focusState->window();
    auto monitor = focusState->monitor();

    if (WINDOWATCURSOR != m_pWindow && m_pWindow != window)
        return false;

    // check if input is on top or overlay shell layers
    auto PMONITOR = monitor;
    PHLLS foundSurface = nullptr;
    Vector2D surfaceCoords;

    // check top layer
    if (PMONITOR)
        HITTESTER.layerSurfaceAt(CURSOR, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &foundSurface);

    if (foundSurface)
        return false;
    // check overlay layer
    if (PMONITOR)
        HITTESTER.layerSurfaceAt(CURSOR, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &foundSurface);

    if (foundSurface)
        return false;

    return true;
}

void CHyprWindowDecorator::onMouseButton(Event::SCallbackInfo &info, IPointer::SButtonEvent e)
{
    if (!inputIsValid())
        return;

    // Only process if mouse is actually on the bar decoration
    if (!isMouseOnBar() && !m_bDragPending && !m_bDraggingThis)
        return;

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
    {
        handleUpEvent(info);
        return;
    }

    handleDownEvent(info, std::nullopt);
}

void CHyprWindowDecorator::onTouchDown(Event::SCallbackInfo &info, ITouch::SDownEvent e)
{
    // Don't do anything if you're already grabbed a window with another finger
    if (!inputIsValid() || e.touchID != 0)
        return;

    handleDownEvent(info, e);
}

void CHyprWindowDecorator::onTouchUp(Event::SCallbackInfo &info, [[maybe_unused]] ITouch::SUpEvent e)
{
    if (!m_bDragPending || !m_bTouchEv || e.touchID != m_touchId)
        return;

    handleUpEvent(info);
}

void CHyprWindowDecorator::onMouseMove([[maybe_unused]] Vector2D coords)
{
    if (m_bTouchEv || !validMapped(m_pWindow) || m_touchId != 0)
        return;

    if (!gPlugin->m_vButtons.empty())
        damageOnButtonHover();

    if (!m_bDragPending)
        return;

    m_bDragPending = false;
    handleMovement();
}

void CHyprWindowDecorator::onTouchMove([[maybe_unused]] Event::SCallbackInfo &info, ITouch::SMotionEvent e)
{
    if (!m_bDragPending || !m_bTouchEv || !validMapped(m_pWindow) || e.touchID != m_touchId)
        return;

    auto PMONITOR = m_pWindow->m_monitor.lock();
    PMONITOR = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();
    const auto COORDS = Vector2D(PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y);

    if (!m_bDraggingThis)
    {
        // Initial setup for dragging a window.
        g_pKeybindManager->m_dispatchers["setfloating"]("activewindow");
        g_pKeybindManager->m_dispatchers["resizewindowpixel"]("exact 50% 50%,activewindow");
        // pin it so you can change workspaces while dragging a window
        g_pKeybindManager->m_dispatchers["pin"]("activewindow");
    }
    g_pKeybindManager->m_dispatchers["movewindowpixel"](std::format("exact {} {},activewindow", (int)(COORDS.x - (assignedBoxGlobal().w / 2)), (int)COORDS.y));
    m_bDraggingThis = true;
}

void CHyprWindowDecorator::handleDownEvent(Event::SCallbackInfo &info, std::optional<ITouch::SDownEvent> touchEvent)
{
    m_bTouchEv = touchEvent.has_value();
    if (m_bTouchEv)
        m_touchId = touchEvent.value().touchID;

    const auto PWINDOW = m_pWindow.lock();

    auto COORDS = cursorRelativeToBar();
    if (m_bTouchEv)
    {
        ITouch::SDownEvent e = touchEvent.value();
        auto PMONITOR = Compat::monitorFromName(!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "");
        PMONITOR = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();
        COORDS = Vector2D(PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y) - assignedBoxGlobal().pos();
    }

    const std::string ON_DOUBLE_CLICK = gPlugin->on_double_click;

    if (!isPointOnBar(COORDS))
    {
        // Click is outside bar bounds - cleanup if we were dragging
        if (m_bDraggingThis)
        {
            if (m_bTouchEv)
                g_pKeybindManager->m_dispatchers["settiled"]("activewindow");
            g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
        }

        m_bDraggingThis = false;
        m_bDragPending = false;
        m_bTouchEv = false;
        return;
    }

    // We're clicking on the bar - cancel the event and handle it ourselves
    info.cancelled = true;
    m_bCancelledDown = true;

    if (Desktop::focusState()->window() != PWINDOW)
        Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);

    if (PWINDOW->m_isFloating)
        Desktop::windowState()->raise(PWINDOW);

    m_iButtonPressedIdx = indexToButton(COORDS);
    if (m_iButtonPressedIdx != -1)
    {
        damageEntire();
        return;
    }

    if (!ON_DOUBLE_CLICK.empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_lastMouseDown).count() < 400 /* Arbitrary delay I found suitable */)
    {
        g_pKeybindManager->m_dispatchers["exec"](ON_DOUBLE_CLICK);
        m_bDragPending = false;
    }
    else
    {
        m_lastMouseDown = Time::steadyNow();
        m_bDragPending = true;
    }
}

void CHyprWindowDecorator::handleUpEvent(Event::SCallbackInfo &info)
{
    if (m_pWindow.lock() != Desktop::focusState()->window())
        return;

    if (m_bCancelledDown)
        info.cancelled = true;

    m_bCancelledDown = false;

    if (m_bDraggingThis)
    {
        g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
        m_bDraggingThis = false;
        if (m_bTouchEv)
            g_pKeybindManager->m_dispatchers["settiled"]("activewindow");
    }

    m_bDragPending = false;
    m_bTouchEv = false;
    m_touchId = 0;

    if (m_iButtonPressedIdx != -1)
    {
        if (indexToButton(cursorRelativeToBar()) == m_iButtonPressedIdx)
        {
            g_pKeybindManager->m_dispatchers["exec"](gPlugin->m_vButtons[m_iButtonPressedIdx].cmd);
        }
        m_iButtonPressedIdx = -1;
        damageEntire();
    }
}

void CHyprWindowDecorator::handleMovement()
{
    g_pKeybindManager->m_dispatchers["mouse"]("1movewindow");
    m_bDraggingThis = true;
    return;
}

int CHyprWindowDecorator::indexToButton(Vector2D COORDS)
{
    const bool VERTICAL = gPlugin->decoration_title_placement == "left" || gPlugin->decoration_title_placement == "right";
    const auto DECOBOX = assignedBoxGlobal();
    const auto &NPI = m_bWindowHasFocus ? gPlugin->activeNinepatch : gPlugin->inactiveNinepatch;
    const auto P = gPlugin->decoration_padding;

    CBox topBarBox;
    if (gPlugin->decoration_title_placement == "top")
    {
        topBarBox = {(double)(gPlugin->decoration_offset_left + NPI.padding[0] + P), 0.0, (double)(DECOBOX.w - (gPlugin->decoration_offset_left + gPlugin->decoration_offset_right + NPI.padding[0] + NPI.padding[2] + 2 * P)), (double)(gPlugin->decoration_offset_top + NPI.padding[1] + P)};
    }
    else if (gPlugin->decoration_title_placement == "bottom")
    {
        topBarBox = {(double)(gPlugin->decoration_offset_left + NPI.padding[0] + P), (double)(DECOBOX.h - (gPlugin->decoration_offset_bottom + NPI.padding[3] + P)), (double)(DECOBOX.w - (gPlugin->decoration_offset_left + gPlugin->decoration_offset_right + NPI.padding[0] + NPI.padding[2] + 2 * P)), (double)(gPlugin->decoration_offset_bottom + NPI.padding[3] + P)};
    }
    else if (gPlugin->decoration_title_placement == "left")
    {
        topBarBox = {0.0, (double)(gPlugin->decoration_offset_top + NPI.padding[1] + P), (double)(gPlugin->decoration_offset_left + NPI.padding[0] + P), (double)(DECOBOX.h - (gPlugin->decoration_offset_top + gPlugin->decoration_offset_bottom + NPI.padding[1] + NPI.padding[3] + 2 * P))};
    }
    else
    {
        topBarBox = {(double)(DECOBOX.w - (gPlugin->decoration_offset_right + NPI.padding[2] + P)), (double)(gPlugin->decoration_offset_top + NPI.padding[1] + P), (double)(gPlugin->decoration_offset_right + NPI.padding[2] + P), (double)(DECOBOX.h - (gPlugin->decoration_offset_top + gPlugin->decoration_offset_bottom + NPI.padding[1] + NPI.padding[3] + 2 * P))};
    }

    if (!VECINRECT(COORDS, topBarBox.x, topBarBox.y, topBarBox.x + topBarBox.w, topBarBox.y + topBarBox.h))
        return -1;

    const auto relativeCOORDS = COORDS - topBarBox.pos();
    const auto BARBUF = VERTICAL ? Vector2D(topBarBox.h, topBarBox.w) : Vector2D(topBarBox.w, topBarBox.h);

    const bool BUTTONSRIGHT = gPlugin->bar_buttons_alignment != "left";
    const float iconReserved = gPlugin->decoration_appicon_enabled ? (VERTICAL ? topBarBox.w : topBarBox.h) : 0;
    float offset = gPlugin->decoration_padding + (BUTTONSRIGHT ? 0 : iconReserved);
    int idx = 0;
    for (auto &b : gPlugin->m_vButtons)
    {
        const float alongSize = VERTICAL ? b.size.y : b.size.x;
        const float acrossSize = VERTICAL ? b.size.x : b.size.y;
        Vector2D currentPos = Vector2D((double)(BUTTONSRIGHT ? BARBUF.x - gPlugin->bar_button_padding - alongSize - offset : offset), (double)(BARBUF.y - acrossSize) / 2.0).floor();

        if (VERTICAL)
        {
            if (VECINRECT(relativeCOORDS, currentPos.y, currentPos.x, currentPos.y + b.size.x, currentPos.x + b.size.y + gPlugin->bar_button_padding))
                return idx;
        }
        else
        {
            if (VECINRECT(relativeCOORDS, currentPos.x, currentPos.y, currentPos.x + b.size.x + gPlugin->bar_button_padding, currentPos.y + b.size.y))
                return idx;
        }

        offset += gPlugin->bar_button_padding + alongSize;
        idx++;
    }

    return -1;
}

void CHyprWindowDecorator::renderText(SP<Render::ITexture> out, const std::string &text, const CHyprColor &color, const Vector2D &bufferSize, const float scale, const int fontSize)
{
    const auto CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bufferSize.x, bufferSize.y);
    const auto CAIRO = cairo_create(CAIROSURFACE);

    // clear the pixmap
    cairo_save(CAIRO);
    cairo_set_operator(CAIRO, CAIRO_OPERATOR_CLEAR);
    cairo_paint(CAIRO);
    cairo_restore(CAIRO);

    // draw title using Pango
    PangoLayout *layout = pango_cairo_create_layout(CAIRO);
    pango_layout_set_text(layout, text.c_str(), -1);

    PangoFontDescription *fontDesc = pango_font_description_from_string("sans");
    pango_font_description_set_size(fontDesc, fontSize * scale * PANGO_SCALE);
    pango_layout_set_font_description(layout, fontDesc);
    pango_font_description_free(fontDesc);

    const int maxWidth = bufferSize.x;

    pango_layout_set_width(layout, maxWidth * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);

    cairo_set_source_rgba(CAIRO, color.r, color.g, color.b, color.a);

    PangoRectangle ink_rect, logical_rect;
    pango_layout_get_extents(layout, &ink_rect, &logical_rect);

    const int layoutWidth = ink_rect.width;
    const int layoutHeight = logical_rect.height;

    const double xOffset = (bufferSize.x / 2.0 - layoutWidth / PANGO_SCALE / 2.0);
    const double yOffset = (bufferSize.y / 2.0 - layoutHeight / PANGO_SCALE / 2.0);

    cairo_move_to(CAIRO, xOffset, yOffset);
    pango_cairo_show_layout(CAIRO, layout);

    g_object_unref(layout);

    cairo_surface_flush(CAIROSURFACE);

    uploadCairoSurface(out, CAIROSURFACE, false);

    // delete cairo
    cairo_destroy(CAIRO);
    cairo_surface_destroy(CAIROSURFACE);
}

void CHyprWindowDecorator::renderBarTitle(const Vector2D &bufferSize, const float scale)
{
    const bool VERTICAL = gPlugin->decoration_title_placement == "left" || gPlugin->decoration_title_placement == "right";

    const auto PWINDOW = m_pWindow.lock();

    float buttonSizes = gPlugin->bar_button_padding;
    for (auto &b : gPlugin->m_vButtons)
    {
        buttonSizes += (VERTICAL ? b.size.y : b.size.x) + gPlugin->bar_button_padding;
    }

    const auto scaledSize = gPlugin->decoration_title_size * scale;
    const auto scaledButtonsSize = buttonSizes * scale;
    const auto scaledBarPadding = gPlugin->decoration_padding * scale;

    const CHyprColor COLOR = m_bForcedTitleColor.value_or(gPlugin->col_text);

    const auto CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bufferSize.x, bufferSize.y);
    const auto CAIRO = cairo_create(CAIROSURFACE);

    // clear the pixmap
    cairo_save(CAIRO);
    cairo_set_operator(CAIRO, CAIRO_OPERATOR_CLEAR);
    cairo_paint(CAIRO);
    cairo_restore(CAIRO);

    if (VERTICAL)
    {
        cairo_translate(CAIRO, bufferSize.x / 2.0, bufferSize.y / 2.0);
        if (gPlugin->decoration_title_placement == "left")
            cairo_rotate(CAIRO, -M_PI / 2.0);
        else
            cairo_rotate(CAIRO, M_PI / 2.0);
        cairo_translate(CAIRO, -bufferSize.y / 2.0, -bufferSize.x / 2.0);
    }

    const float logicalWidth = VERTICAL ? bufferSize.y : bufferSize.x;
    const float logicalHeight = VERTICAL ? bufferSize.x : bufferSize.y;

    // draw title using Pango
    PangoLayout *layout = pango_cairo_create_layout(CAIRO);
    pango_layout_set_text(layout, m_szLastTitle.c_str(), -1);

    PangoFontDescription *fontDesc = pango_font_description_from_string(gPlugin->bar_text_font.c_str());
    pango_font_description_set_size(fontDesc, scaledSize * PANGO_SCALE);
    pango_layout_set_font_description(layout, fontDesc);
    pango_font_description_free(fontDesc);

    PangoContext *context = pango_layout_get_context(layout);
    pango_context_set_base_dir(context, PANGO_DIRECTION_NEUTRAL);

    const float iconReserved = gPlugin->decoration_appicon_enabled ? (VERTICAL ? bufferSize.x : bufferSize.y) : 0;
    const float paddingTotal = scaledBarPadding * 2 + scaledButtonsSize + iconReserved;
    const float maxWidth = std::max(0.0f, logicalWidth - paddingTotal);

    pango_layout_set_width(layout, (int)std::round(maxWidth * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    cairo_set_source_rgba(CAIRO, COLOR.r, COLOR.g, COLOR.b, COLOR.a);

    int layoutWidth, layoutHeight;
    pango_layout_get_size(layout, &layoutWidth, &layoutHeight);

    // Use float alignment (0.0 to 1.0)
    const float align = std::clamp(gPlugin->decoration_title_align, 0.0f, 1.0f);

    const bool BUTTONSRIGHT = gPlugin->bar_buttons_alignment != "left";
    const float availableWidth = logicalWidth - paddingTotal;
    const int xOffset = std::round(scaledBarPadding + (BUTTONSRIGHT ? 0 : scaledButtonsSize) + iconReserved + (availableWidth - (float)layoutWidth / PANGO_SCALE) * align);
    const int yOffset = std::round((logicalHeight / 2.0 - layoutHeight / PANGO_SCALE / 2.0));

    cairo_move_to(CAIRO, xOffset, yOffset);
    pango_cairo_show_layout(CAIRO, layout);

    g_object_unref(layout);

    uploadCairoSurface(m_pTextTex, CAIROSURFACE, false);

    m_pTextTex->m_size = bufferSize;

    // delete cairo
    cairo_destroy(CAIRO);
    cairo_surface_destroy(CAIROSURFACE);
}

size_t CHyprWindowDecorator::getVisibleButtonCount(const Vector2D &bufferSize, const float scale)
{
    const bool VERTICAL = gPlugin->decoration_title_placement == "left" || gPlugin->decoration_title_placement == "right";
    const float iconReserved = gPlugin->decoration_appicon_enabled ? bufferSize.y : 0;
    float availableSpace = bufferSize.x - gPlugin->decoration_padding * scale * 2 - iconReserved;
    size_t count = 0;

    for (const auto &button : gPlugin->m_vButtons)
    {
        const float buttonSpace = ((VERTICAL ? button.size.y : button.size.x) + gPlugin->bar_button_padding) * scale;
        if (availableSpace >= buttonSpace)
        {
            count++;
            availableSpace -= buttonSpace;
        }
        else
            break;
    }

    return count;
}

bool CHyprWindowDecorator::renderBarButtons(const Vector2D &bufferSize, [[maybe_unused]] const float scale)
{
    // Buttons are drawn by renderBarButtonsText(); this only clears the button canvas.
    const bool texturesLoaded = false;

    const auto CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bufferSize.x, bufferSize.y);
    const auto CAIRO = cairo_create(CAIROSURFACE);

    // clear the pixmap
    cairo_save(CAIRO);
    cairo_set_operator(CAIRO, CAIRO_OPERATOR_CLEAR);
    cairo_paint(CAIRO);
    cairo_restore(CAIRO);

    // (upstream computed a running button offset here and never used it)

    uploadCairoSurface(m_pButtonsTex, CAIROSURFACE, false);

    m_pButtonsTex->m_size = bufferSize;

    // delete cairo
    cairo_destroy(CAIRO);
    cairo_surface_destroy(CAIROSURFACE);

    // Return true if textures were loaded, so caller knows to keep rendering
    return texturesLoaded;
}

void CHyprWindowDecorator::renderBarButtonsText(CBox *barBox, const float scale, const float a)
{
    const bool VERTICAL = gPlugin->decoration_title_placement == "left" || gPlugin->decoration_title_placement == "right";
    const auto visibleCount = getVisibleButtonCount(VERTICAL ? Vector2D((double)barBox->height, (double)barBox->width) : Vector2D((double)barBox->width, (double)barBox->height), scale);
    const auto COORDS = cursorRelativeToBar();

    const bool BUTTONSRIGHT = gPlugin->bar_buttons_alignment != "left";
    const float iconReserved = gPlugin->decoration_appicon_enabled ? (VERTICAL ? barBox->width : barBox->height) : 0;
    double offset = (gPlugin->decoration_padding * scale) + (BUTTONSRIGHT ? 0 : iconReserved);
    float noScaleOffset = gPlugin->decoration_padding + (BUTTONSRIGHT ? 0 : iconReserved / scale);

    int hoveredIdx = indexToButton(COORDS);

    for (size_t i = 0; i < visibleCount; ++i)
    {
        auto &button = gPlugin->m_vButtons[i];
        const auto scaledButtonSizeX = button.size.x * scale;
        const auto scaledButtonSizeY = button.size.y * scale;
        const auto scaledButtonsPad = gPlugin->bar_button_padding * scale;

        // check if hovering here
        bool hovering = (hoveredIdx == (int)i);


        CBox pos;

        if (!VERTICAL)
        {
            pos = {barBox->x + (int)std::round(BUTTONSRIGHT ? barBox->width - offset - scaledButtonSizeX : offset), (double)std::round(barBox->y + (barBox->height - scaledButtonSizeY) / 2.0), (double)std::round(scaledButtonSizeX), (double)std::round(scaledButtonSizeY)};
        }
        else
        {
            pos = {barBox->x + (int)std::round((barBox->width - scaledButtonSizeX) / 2.0), barBox->y + (int)std::round(BUTTONSRIGHT ? barBox->height - offset - scaledButtonSizeY : offset), (double)std::round(scaledButtonSizeX), (double)std::round(scaledButtonSizeY)};
        }

        noScaleOffset += gPlugin->bar_button_padding + (VERTICAL ? button.size.y : button.size.x);

        // Skip if textured button is not available
        if (button.texActive->m_texID == 0)
            continue;

        SP<Render::ITexture> tex = button.texActive;
        if (!m_bWindowHasFocus)
            tex = button.texInactive;
        if (hovering)
            tex = button.texHover;
        if (m_iButtonPressedIdx == (int)i)
            tex = button.texPressed;

        if (tex->m_texID == 0)
            tex = button.texActive;

        Render::GL::CHyprOpenGLImpl::STextureRenderData data;
        data.a = a;
        Render::GL::g_pHyprOpenGL->renderTexture(tex, pos, data);
        offset += scaledButtonsPad + (VERTICAL ? scaledButtonSizeY : scaledButtonSizeX);

        bool currentBit = (m_iButtonHoverState & (1 << i)) != 0;
        if (hovering != currentBit)
        {
            m_iButtonHoverState ^= (1 << i);
            // damage to get rid of some artifacts when icons are "hidden"
            damageEntire();
        }
    }
}

void CHyprWindowDecorator::draw(PHLMONITOR pMonitor, const float &a)
{
    gPlugin->loadAllTextures();

    // Consume the reposition request here, where it is acted on. m_bWindowSizeChanged
    // itself must stay true until renderPass() has rebuilt the textures, otherwise the
    // frame would never update. Previously nothing cleared the request, so once it was
    // set, repositionDeco() ran every frame.
    if (m_bLastEnabledState != gPlugin->enabled || m_bNeedsReposition)
    {
        m_bLastEnabledState = gPlugin->enabled;
        m_bNeedsReposition = false;
        g_pDecorationPositioner->repositionDeco(this);
    }

    if (m_hidden || !validMapped(m_pWindow) || !gPlugin->enabled)
        return;

    const auto PWINDOW = m_pWindow.lock();

    if (!PWINDOW->m_ruleApplicator->decorate().valueOrDefault())
        return;

    // 0.56 keeps decorations in m_renderPass, a persistent queue consumed when the
    // pass renders. (addPassElement()/inline drawing belong to a newer API.)
    auto data = CRenderPassElement::SBarData{this, pMonitor, a};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRenderPassElement>(data));
}

void CHyprWindowDecorator::renderPass(PHLMONITOR pMonitor, const float &a)
{
    const auto PWINDOW = m_pWindow.lock();

    bool windowFocus = PWINDOW == Desktop::focusState()->window();
    bool focusChanged = windowFocus != m_bWindowHasFocus;
    if (focusChanged)
    {
        m_bWindowHasFocus = windowFocus;
        m_bButtonsDirty = true;
        damageEntire();
    }

    const CHyprColor DEST_COLOR = m_bForcedBarColor.value_or(gPlugin->bar_color);
    if (DEST_COLOR != m_cRealBarColor->goal())
        *m_cRealBarColor = DEST_COLOR;

    CHyprColor color = m_cRealBarColor->value();

    color.a *= a;
    const bool SHOULDBLUR = gPlugin->bar_blur && color.a < 1.F;
    const auto PWORKSPACE = PWINDOW->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !PWINDOW->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    // Only window rounding matters here: the stencil path below masks out the window
    // area so the frame does not paint over the client surface. Including the border
    // size made it engage even with rounding = 0, which masked the whole frame and
    // left only a black rectangle.
    const auto ROUNDING = PWINDOW->rounding();

    const auto scaledRounding = ROUNDING > 0 ? ROUNDING * pMonitor->m_scale - 2 /* idk why but otherwise it looks bad due to the gaps */ : 0;

    m_seExtents = currentExtents();

    const auto DECOBOX = assignedBoxGlobal();

    // Never build regions from a degenerate box: pixman rejects them. Log loudly,
    // because an unassigned box means the positioner never replied and the
    // decoration would silently never be drawn.
    if (DECOBOX.w <= 0 || DECOBOX.h <= 0)
    {
        const auto RAW = assignedBoxGlobal();
        Log::logger->log(Log::WARN, std::format("[HYPRDECOR] draw skipped: degenerate decobox {}x{} (raw assigned {}x{} at {},{}), window {}x{} at {},{}",
                                                DECOBOX.w, DECOBOX.h, RAW.w, RAW.h, RAW.x, RAW.y,
                                                PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x,
                                                PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y,
                                                PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x,
                                                PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y));
        return;
    }

    const auto BARBUF = DECOBOX.size() * pMonitor->m_scale;

    CBox titleBarBox = {DECOBOX.x - pMonitor->m_position.x, DECOBOX.y - pMonitor->m_position.y, DECOBOX.w, DECOBOX.h};

    titleBarBox.scale(pMonitor->m_scale).round();

    // Diagnostics for `hyprctl hyprdecor status`.
    gPlugin->lastBarW = (int)titleBarBox.w;
    gPlugin->lastBarH = (int)titleBarBox.h;
    gPlugin->lastDrawn = true;

    if (titleBarBox.w < 1 || titleBarBox.h < 1)
    {
        Log::logger->log(Log::WARN, std::format("[HYPRDECOR] skipping draw: titleBarBox {}x{}", titleBarBox.w, titleBarBox.h));
        return;
    }

    static CBox s_lastLoggedBox;
    if (DECOBOX != s_lastLoggedBox)
    {
        s_lastLoggedBox = DECOBOX;
        Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] draw: decobox={},{} {}x{} titleBar={},{} {}x{} scale={} surface={} ninepatch={}",
                                             DECOBOX.x, DECOBOX.y, DECOBOX.w, DECOBOX.h,
                                             titleBarBox.x, titleBarBox.y, titleBarBox.w, titleBarBox.h,
                                             pMonitor->m_scale,
                                             (m_bWindowHasFocus ? gPlugin->activeSurface : gPlugin->inactiveSurface) ? "yes" : "NO",
                                             (m_bWindowHasFocus ? gPlugin->activeNinepatch : gPlugin->inactiveNinepatch).defined ? "yes" : "NO"));
    }

    Render::GL::g_pHyprOpenGL->scissor(titleBarBox);

    if (ROUNDING)
    {
        // the +1 is a shit garbage temp fix until renderRect supports an alpha matte
        CBox windowBox = {PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x - pMonitor->m_position.x + 1,
                          PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y - pMonitor->m_position.y + 1, PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x - 2,
                          PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y - 2};

        if (windowBox.w < 1 || windowBox.h < 1)
            return;

        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);

        Render::GL::g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, true);

        glStencilFunc(GL_ALWAYS, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        windowBox.scale(pMonitor->m_scale).round();
        Render::GL::g_pHyprOpenGL->renderRect(windowBox, CHyprColor(0, 0, 0, 0), {.round = (int)scaledRounding, .roundingPower = m_pWindow->roundingPower()});
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glStencilFunc(GL_NOTEQUAL, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }

    const auto &NPI = m_bWindowHasFocus ? gPlugin->activeNinepatch : gPlugin->inactiveNinepatch;
    float border[4] = {NPI.border[0], NPI.border[1], NPI.border[2], NPI.border[3]};
    cairo_surface_t *sourceSurface = m_bWindowHasFocus ? gPlugin->activeSurface : gPlugin->inactiveSurface;

    if (sourceSurface)
    {
        // Under ABSOLUTE positioning Hyprland replies with an empty assignedGeometry, so
        // m_bWindowSizeChanged is never set by a resize. Comparing the texture size to
        // the box is what actually detects a stale frame (previously only focus changes
        // rebuilt it, which is why resized windows kept an old stretched texture).
        const Vector2D BARWANT{titleBarBox.width, titleBarBox.height};

        if (m_bWindowSizeChanged || m_pBarFinalTex->m_texID == 0 || focusChanged || m_bNinePatchChanged || m_pBarFinalTex->m_size != BARWANT)
        {
            const auto CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, titleBarBox.width, titleBarBox.height);
            const auto CAIRO = cairo_create(CAIROSURFACE);

            cairo_set_operator(CAIRO, CAIRO_OPERATOR_CLEAR);
            cairo_paint(CAIRO);
            cairo_set_operator(CAIRO, CAIRO_OPERATOR_OVER);

            const int sw = cairo_image_surface_get_width(sourceSurface);
            const int sh = cairo_image_surface_get_height(sourceSurface);

            // Grid maths lives in ninepatch.hpp and is covered by unit tests.
            SNinePatchInfo gridInfo;
            gridInfo.border[0] = border[0];
            gridInfo.border[1] = border[1];
            gridInfo.border[2] = border[2];
            gridInfo.border[3] = border[3];
            gridInfo.defined = true;

            // The grid scale is monitor scale times the art scale, so border.scale
            // enlarges the pixel art without compounding with the monitor scale.
            const auto GRID = NinePatch::layout(gridInfo, (double)sw, (double)sh, (double)titleBarBox.width, (double)titleBarBox.height,
                                                pMonitor->m_scale * (double)std::max(1, gPlugin->cfg.artScale));

            if (GRID.degraded)
                Log::logger->log(Log::WARN, std::format("[HYPRDECOR] frame too large for its box ({}x{} at scale {}); scaling to fit", sw, sh, pMonitor->m_scale));

            // Resampling happens HERE, in Cairo, because the canvas is composited at
            // device scale before it ever reaches OpenGL. The GL texture filter cannot
            // undo it, so this is where linear_filtering must be applied.
            const auto RESAMPLE = gPlugin->cfg.linearFiltering ? NinePatch::eFilter::Bilinear : NinePatch::eFilter::Nearest;

            const double *sx = GRID.sx;
            const double *sy = GRID.sy;
            const double *dx = GRID.dx;
            const double *dy = GRID.dy;

            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j)
                {
                    double middleAlphaVal = (i == 1 && j == 1) ? gPlugin->ninepatch_middle_alpha : 1.0;
                    if (middleAlphaVal <= 0)
                        continue;

                    cairo_save(CAIRO);
                    if (middleAlphaVal < 1.0)
                    {
                        cairo_push_group(CAIRO);
                    }

                    bool isMiddlePatch = (i == 1 || j == 1);
                    if (gPlugin->ninepatch_repeat && isMiddlePatch)
                    {
                        drawRepeatedSurface(CAIRO, sourceSurface, sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j], dx[i], dy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j], RESAMPLE);
                    }
                    else
                    {
                        drawSizedSurface(CAIRO, sourceSurface, sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j], dx[i], dy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j], RESAMPLE);
                    }

                    if (middleAlphaVal < 1.0)
                    {
                        cairo_pop_group_to_source(CAIRO);
                        cairo_paint_with_alpha(CAIRO, middleAlphaVal);
                    }
                    cairo_restore(CAIRO);
                }
            }

            cairo_surface_flush(CAIROSURFACE);

            uploadCairoSurface(m_pBarFinalTex, CAIROSURFACE, gPlugin->ninepatch_linear_filtering);

            cairo_destroy(CAIRO);
            cairo_surface_destroy(CAIROSURFACE);
            m_bNinePatchChanged = false;
        }
        Render::GL::CHyprOpenGLImpl::STextureRenderData data;
        data.a = a;
        Render::GL::g_pHyprOpenGL->renderTexture(m_pBarFinalTex, titleBarBox, data);
    }
    else
    {
        if (SHOULDBLUR)
            Render::GL::g_pHyprOpenGL->renderRect(titleBarBox, color, {.round = (int)scaledRounding, .roundingPower = m_pWindow->roundingPower(), .blur = true, .blurA = a});
        else
            Render::GL::g_pHyprOpenGL->renderRect(titleBarBox, color, {.round = (int)scaledRounding, .roundingPower = m_pWindow->roundingPower()});
    }

    if (m_bWindowSizeChanged)
        m_bButtonsDirty = true;

    // Positioned images (eye, button backdrop, title background, ...). Drawn inside the
    // TOP BAND only, so art can never spill over the client area.
    {
        const double BANDH = gPlugin->decoration_offset_top * pMonitor->m_scale;
        const double BANDW = titleBarBox.width;

        if (BANDH > 0 && BANDW > 0)
        {
            for (const auto &element : gPlugin->m_vElements)
            {
                if (!element.tex || element.tex->m_texID == 0)
                    continue;

                // border.scale applies to BORDER ART ONLY. Elements have their own
                // scale (default 1) and are otherwise sized by the monitor scale, in
                // logical pixels: a 15px eye is 15 logical px = 30 device px on a 2x
                // output. Inheriting border.scale made every element silently 2x.
                const int ESCALE = element.cfg.scale.value_or(1);
                const double S = (double)ESCALE;

                // naturalSize is in SOURCE pixels, so multiply by the element scale and
                // the monitor scale to reach device pixels.
                const double DEV = S * pMonitor->m_scale;

                double W = element.cfg.width > 0 ? element.cfg.width * pMonitor->m_scale : element.naturalSize.x * DEV;
                double H = element.cfg.height > 0 ? element.cfg.height * pMonitor->m_scale : element.naturalSize.y * DEV;

                const double OX = element.cfg.offsetX * pMonitor->m_scale;
                const double OY = element.cfg.offsetY * pMonitor->m_scale;

                double X = titleBarBox.x;
                switch (element.cfg.anchor)
                {
                    case SElement::eAnchor::TopLeft: X = titleBarBox.x + OX; break;
                    case SElement::eAnchor::TopCentre: X = titleBarBox.x + (BANDW - W) / 2.0 + OX; break;
                    case SElement::eAnchor::TopRight: X = titleBarBox.x + BANDW - W + OX; break;
                }
                const double Y = titleBarBox.y + OY;

                Render::GL::CHyprOpenGLImpl::STextureRenderData edata;
                edata.a = a;

                if (element.cfg.ninepatch && element.npi.defined)
                {
                    // A nine-segment element: stretch the middle band, keep the ends.
                    // Horizontal by default, so a title background keeps its height.
                    const double SRCW = element.naturalSize.x;
                    const double SRCH = element.naturalSize.y;

                    // Horizontal-only stretch (the default): the element keeps its
                    // natural height, so its top and bottom bands are drawn unscaled
                    // and the vertical middle contributes nothing.
                    if (!element.cfg.stretchVertical)
                        H = (element.npi.border[1] + element.npi.border[3]) * DEV;

                    // Optional cap: never stretch beyond max_stretch source pixels.
                    if (element.cfg.maxStretch && *element.cfg.maxStretch > 0)
                    {
                        const double FIXED = (element.npi.border[0] + element.npi.border[2]) * DEV;
                        W = std::min(W, FIXED + *element.cfg.maxStretch * DEV);
                    }

                    const auto GRID = NinePatch::layout(element.npi, SRCW, SRCH, W, H, DEV);

                    // Draw the 9 cells; for horizontal-only stretch we collapse the
                    // vertical middle band by using the same destination rows.
                    for (int gi = 0; gi < 3; ++gi)
                        for (int gj = 0; gj < 3; ++gj)
                        {
                            const double SW = GRID.sx[gi + 1] - GRID.sx[gi];
                            const double SH = GRID.sy[gj + 1] - GRID.sy[gj];
                            const double DW = GRID.dx[gi + 1] - GRID.dx[gi];
                            const double DH = GRID.dy[gj + 1] - GRID.dy[gj];
                            if (SW <= 0 || SH <= 0 || DW <= 0 || DH <= 0)
                                continue;

                            // Each cell draws a sub-rectangle of the texture into its
                            // own box: renderTexture maps the whole texture by default,
                            // so the UVs must be restricted explicitly.
                            auto cellData = edata;
                            cellData.allowCustomUV            = true;
                            cellData.primarySurfaceUVTopLeft     = Vector2D(GRID.sx[gi] / SRCW, GRID.sy[gj] / SRCH);
                            cellData.primarySurfaceUVBottomRight = Vector2D(GRID.sx[gi + 1] / SRCW, GRID.sy[gj + 1] / SRCH);

                            Render::GL::g_pHyprOpenGL->renderTexture(element.tex, CBox{X + GRID.dx[gi], Y + GRID.dy[gj], DW, DH}, cellData);
                        }
                }
                else
                    Render::GL::g_pHyprOpenGL->renderTexture(element.tex, CBox{X, Y, W, H}, edata);
            }
        }
    }

    // Plain solid outline, drawn on top of whatever was rendered above.
    // `bar_outline_always = false` restricts it to the no-texture fallback.
    if (gPlugin->bar_outline_width > 0 && (gPlugin->bar_outline_always || !sourceSurface))
    {
        CHyprColor outlineColor = gPlugin->bar_outline_color;
        outlineColor.a *= a;

        const double THICK = std::max(1.0, (double)std::round(gPlugin->bar_outline_width * pMonitor->m_scale));
        const CBox &B = titleBarBox;

        const Render::GL::CHyprOpenGLImpl::SRectRenderData RECTDATA{.round = 0, .roundingPower = m_pWindow->roundingPower()};

        // top, bottom, left, right
        Render::GL::g_pHyprOpenGL->renderRect(CBox{B.x, B.y, B.w, THICK}, outlineColor, RECTDATA);
        Render::GL::g_pHyprOpenGL->renderRect(CBox{B.x, B.y + B.h - THICK, B.w, THICK}, outlineColor, RECTDATA);
        Render::GL::g_pHyprOpenGL->renderRect(CBox{B.x, B.y + THICK, THICK, B.h - 2 * THICK}, outlineColor, RECTDATA);
        Render::GL::g_pHyprOpenGL->renderRect(CBox{B.x + B.w - THICK, B.y + THICK, THICK, B.h - 2 * THICK}, outlineColor, RECTDATA);
    }

    const auto P = gPlugin->decoration_padding;
    CBox topBarBox;
    if (gPlugin->decoration_title_placement == "top")
    {
        topBarBox = {titleBarBox.x + (int)std::round((gPlugin->decoration_offset_left + NPI.padding[0] + P) * pMonitor->m_scale),
                     titleBarBox.y,
                     (double)std::round(titleBarBox.width - (gPlugin->decoration_offset_left + gPlugin->decoration_offset_right + NPI.padding[0] + NPI.padding[2] + 2 * P) * pMonitor->m_scale),
                     (double)std::round((gPlugin->decoration_offset_top + NPI.padding[1] + P) * pMonitor->m_scale)};
    }
    else if (gPlugin->decoration_title_placement == "bottom")
    {
        topBarBox = {titleBarBox.x + (int)std::round((gPlugin->decoration_offset_left + NPI.padding[0] + P) * pMonitor->m_scale),
                     titleBarBox.y + (int)std::round(titleBarBox.height - (gPlugin->decoration_offset_bottom + NPI.padding[3] + P) * pMonitor->m_scale),
                     (double)std::round(titleBarBox.width - (gPlugin->decoration_offset_left + gPlugin->decoration_offset_right + NPI.padding[0] + NPI.padding[2] + 2 * P) * pMonitor->m_scale),
                     (double)std::round((gPlugin->decoration_offset_bottom + NPI.padding[3] + P) * pMonitor->m_scale)};
    }
    else if (gPlugin->decoration_title_placement == "left")
    {
        topBarBox = {titleBarBox.x,
                     titleBarBox.y + (int)std::round((gPlugin->decoration_offset_top + NPI.padding[1] + P) * pMonitor->m_scale),
                     (double)std::round((gPlugin->decoration_offset_left + NPI.padding[0] + P) * pMonitor->m_scale),
                     (double)std::round(titleBarBox.height - (gPlugin->decoration_offset_top + gPlugin->decoration_offset_bottom + NPI.padding[1] + NPI.padding[3] + 2 * P) * pMonitor->m_scale)};
    }
    else if (gPlugin->decoration_title_placement == "right")
    {
        topBarBox = {titleBarBox.x + (int)std::round(titleBarBox.width - (gPlugin->decoration_offset_right + NPI.padding[2] + P) * pMonitor->m_scale),
                     titleBarBox.y + (int)std::round((gPlugin->decoration_offset_top + NPI.padding[1] + P) * pMonitor->m_scale),
                     (double)std::round((gPlugin->decoration_offset_right + NPI.padding[2] + P) * pMonitor->m_scale),
                     (double)std::round(titleBarBox.height - (gPlugin->decoration_offset_top + gPlugin->decoration_offset_bottom + NPI.padding[1] + NPI.padding[3] + 2 * P) * pMonitor->m_scale)};
    }

    // render app icon
    if (gPlugin->decoration_appicon_enabled)
    {
        std::string appId = PWINDOW->m_initialClass;

        int iconSizeDesired = (int)(std::max(topBarBox.width, topBarBox.height) * 0.6); // Reasonable size check
        if (gPlugin->decoration_title_placement == "top" || gPlugin->decoration_title_placement == "bottom")
            iconSizeDesired = (int)(topBarBox.height * 0.6);
        else
            iconSizeDesired = (int)(topBarBox.width * 0.6);

        if (appId != m_szLastAppId || m_pAppIconTex->m_texID == 0 || (int)m_pAppIconTex->m_size.x != iconSizeDesired)
        {
            m_szLastAppId = appId;

            // Reset the texture
            if (m_pAppIconTex->m_texID != 0)
            {
                m_pAppIconTex = makeShared<Render::GL::CGLTexture>();
            }

            loadAppIcon(appId, m_pAppIconTex, iconSizeDesired);
        }

        if (m_pAppIconTex->m_texID != 0)
        {
            int iconPad;
            CBox iconBox;
            const auto ATEXSIZE = m_pAppIconTex->m_size;
            if (gPlugin->decoration_title_placement == "top" || gPlugin->decoration_title_placement == "bottom")
            {
                iconPad = (int)std::round(topBarBox.height * 0.2);
                iconBox = {topBarBox.x + iconPad + gPlugin->decoration_appicon_offset.x * pMonitor->m_scale, topBarBox.y + (int)std::round((topBarBox.height - ATEXSIZE.y) / 2.0) + gPlugin->decoration_appicon_offset.y * pMonitor->m_scale, ATEXSIZE.x, ATEXSIZE.y};
            }
            else
            {
                iconPad = (int)std::round(topBarBox.width * 0.2);
                iconBox = {topBarBox.x + (int)std::round((topBarBox.width - ATEXSIZE.x) / 2.0) + gPlugin->decoration_appicon_offset.x * pMonitor->m_scale, topBarBox.y + iconPad + gPlugin->decoration_appicon_offset.y * pMonitor->m_scale, ATEXSIZE.x, ATEXSIZE.y};
            }
            Render::GL::CHyprOpenGLImpl::STextureRenderData data;
            data.a = a;
            Render::GL::g_pHyprOpenGL->renderTexture(m_pAppIconTex, iconBox, data);
        }
    }

    // render title
    const Vector2D TEXTWANT{(double)topBarBox.width, (double)topBarBox.height};

    if (gPlugin->decoration_title_enabled &&
        (m_szLastTitle != PWINDOW->m_title || m_bWindowSizeChanged || m_pTextTex->m_texID == 0 || m_bTitleColorChanged || m_pTextTex->m_size != TEXTWANT))
    {
        m_szLastTitle = PWINDOW->m_title;
        renderBarTitle(Vector2D((double)topBarBox.width, (double)topBarBox.height), pMonitor->m_scale);
    }

    if (ROUNDING)
    {
        // cleanup stencil
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        Render::GL::g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
        glStencilMask(-1);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
    }

    if (gPlugin->decoration_title_enabled && m_pTextTex->m_texID)
    {
        // render title texture at full bar size (text is already positioned within the texture)
        CBox textBox = {topBarBox.x, topBarBox.y, (double)m_pTextTex->m_size.x, (double)m_pTextTex->m_size.y};
        Render::GL::CHyprOpenGLImpl::STextureRenderData data;
        data.a = a;
        Render::GL::g_pHyprOpenGL->renderTexture(m_pTextTex, textBox, data);
    }

    const bool BTNSVERTICAL = gPlugin->decoration_title_placement == "left" || gPlugin->decoration_title_placement == "right";
    const Vector2D BTNWANT = BTNSVERTICAL ? Vector2D((double)topBarBox.height, (double)topBarBox.width) : Vector2D((double)topBarBox.width, (double)topBarBox.height);

    if (m_bButtonsDirty || m_bWindowSizeChanged || m_pButtonsTex->m_size != BTNWANT)
    {
        const bool VERTICAL = gPlugin->decoration_title_placement == "left" || gPlugin->decoration_title_placement == "right";
        m_bButtonsDirty = renderBarButtons(VERTICAL ? Vector2D((double)topBarBox.height, (double)topBarBox.width) : Vector2D((double)topBarBox.width, (double)topBarBox.height), pMonitor->m_scale);
    }

    if (m_pButtonsTex->m_texID)
    {
        Render::GL::CHyprOpenGLImpl::STextureRenderData data;
        data.a = a;
        Render::GL::g_pHyprOpenGL->renderTexture(m_pButtonsTex, topBarBox, data);
    }

    Render::GL::g_pHyprOpenGL->scissor(nullptr);

    renderBarButtonsText(&topBarBox, pMonitor->m_scale, a);

    m_bWindowSizeChanged = false;
    m_bTitleColorChanged = false;
    m_bButtonsDirty = false;

    // The extents changed, so the layout must be recalculated -- but NEVER from inside
    // a render pass. recalculateMonitor() mutates window geometry while the renderer is
    // iterating windows, and the second window to open would drop the first. Defer it
    // to the main loop instead.
    if (m_iLastHeight != gPlugin->decoration_offset_top)
    {
        // Record only. Forcing recalculateMonitor() here threw the newly laid-out
        // window back to an output-sized box; a fullscreen cycle is the only thing that
        // recomputes it correctly, which is why the bug "fixed itself" that way.
        m_iLastHeight = gPlugin->decoration_offset_top;
    }
}

eDecorationType CHyprWindowDecorator::getDecorationType()
{
    return DECORATION_CUSTOM;
}

void CHyprWindowDecorator::updateWindow([[maybe_unused]] PHLWINDOW pWindow)
{
    damageEntire();
}

void CHyprWindowDecorator::damageEntire()
{
    g_pHyprRenderer->damageBox(assignedBoxGlobal());
}

Vector2D CHyprWindowDecorator::cursorRelativeToBar()
{
    return g_pInputManager->getMouseCoordsInternal() - assignedBoxGlobal().pos();
}

bool CHyprWindowDecorator::isMouseOnBar()
{
    return isPointOnBar(cursorRelativeToBar());
}

bool CHyprWindowDecorator::isPointOnBar(Vector2D COORDS)
{
    const auto BOX = assignedBoxGlobal();

    if (!VECINRECT(COORDS, 0, 0, BOX.w, BOX.h))
        return false;

    // Same extents the layout reserved, so the clickable band always matches the frame.
    const auto EXTENTS = currentExtents();
    const auto L = EXTENTS.topLeft.x;
    const auto T = EXTENTS.topLeft.y;
    const auto R = EXTENTS.bottomRight.x;
    const auto B = EXTENTS.bottomRight.y;

    if (COORDS.x >= L && COORDS.x < BOX.w - R && COORDS.y >= T && COORDS.y < BOX.h - B)
        return false;

    return true;
}

eDecorationLayer CHyprWindowDecorator::getDecorationLayer()
{
    return gPlugin->decoration_render_above ? DECORATION_LAYER_OVER : DECORATION_LAYER_UNDER;
}

uint64_t CHyprWindowDecorator::getDecorationFlags()
{
    return DECORATION_ALLOWS_MOUSE_INPUT | (gPlugin->bar_part_of_window ? DECORATION_PART_OF_MAIN_WINDOW : 0);
}

CBox CHyprWindowDecorator::assignedBoxGlobal()
{
    if (!validMapped(m_pWindow))
        return {};

    const auto PWINDOW = m_pWindow.lock();

    // Under DECORATION_POSITION_ABSOLUTE Hyprland replies with an EMPTY
    // assignedGeometry ("assignedGeometry is empty" in DecorationPositioner.hpp), so the
    // box must be derived from the window plus the reserved extents rather than read
    // from m_bAssignedBox.
    const auto INFO = getPositioningInfo();

    CBox box = {PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x - INFO.desiredExtents.topLeft.x,
                PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y - INFO.desiredExtents.topLeft.y,
                PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + INFO.desiredExtents.topLeft.x + INFO.desiredExtents.bottomRight.x,
                PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y + INFO.desiredExtents.topLeft.y + INFO.desiredExtents.bottomRight.y};

    const auto PWORKSPACE = PWINDOW->m_workspace;
    if (PWORKSPACE && !PWINDOW->m_pinned)
        box.translate(PWORKSPACE->m_renderOffset->value());

    return box;
}

PHLWINDOW CHyprWindowDecorator::getOwner()
{
    return m_pWindow.lock();
}

void CHyprWindowDecorator::updateRules()
{
    const auto PWINDOW = m_pWindow.lock();
    auto prevHidden = m_hidden;
    auto prevForcedTitleColor = m_bForcedTitleColor;

    m_bForcedBarColor = std::nullopt;
    m_bForcedTitleColor = std::nullopt;
    m_hidden = false;

    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(gPlugin->m_nobarRuleIdx))
        m_hidden = truthy(PWINDOW->m_ruleApplicator->m_otherProps.props.at(gPlugin->m_nobarRuleIdx)->effect);
    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(gPlugin->m_barColorRuleIdx))
        m_bForcedBarColor = CHyprColor(Compat::configStringToInt(PWINDOW->m_ruleApplicator->m_otherProps.props.at(gPlugin->m_barColorRuleIdx)->effect).value_or(0));
    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(gPlugin->m_titleColorRuleIdx))
        m_bForcedTitleColor = CHyprColor(Compat::configStringToInt(PWINDOW->m_ruleApplicator->m_otherProps.props.at(gPlugin->m_titleColorRuleIdx)->effect).value_or(0));

    if (prevHidden != m_hidden)
        g_pDecorationPositioner->repositionDeco(this);
    if (prevForcedTitleColor != m_bForcedTitleColor)
        m_bTitleColorChanged = true;
}

void CHyprWindowDecorator::invalidateTextures()
{
    m_pBarFinalTex = makeShared<Render::GL::CGLTexture>();
    m_pTextTex = makeShared<Render::GL::CGLTexture>();
    m_pButtonsTex = makeShared<Render::GL::CGLTexture>();

    // Mark everything as dirty to force full re-render
    m_bWindowSizeChanged = true;
    m_bButtonsDirty = true;
    m_bTitleColorChanged = true;
    m_bNinePatchChanged = true;
    m_szLastTitle = ""; // Force title re-render

    damageEntire();
}

void CHyprWindowDecorator::damageOnButtonHover()
{
    const auto COORDS = cursorRelativeToBar();
    const int idx = indexToButton(COORDS);

    uint32_t newHoverState = 0;
    if (idx != -1)
        newHoverState = (1 << idx);

    if (newHoverState != m_iButtonHoverState)
    {
        m_iButtonHoverState = newHoverState;
        damageEntire();
    }
}
