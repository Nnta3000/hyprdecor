#include "hyprWindowDecorator.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
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
// The minimize action must go through the Lua dispatchers on a Lua-config Hyprland.
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <libdrm/drm_fourcc.h>
#include <pango/pangocairo.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "renderPassElement.hpp"
#include "plugin.hpp"
#include "compat.hpp"
#include "util.hpp"
#include "titlebox.hpp"
#include "itemstate.hpp"
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>

// Dispatcher helpers. Defined further down with the dispatcher notes, but used by the
// pointer handlers above them.
static std::string luaWindowSelector(const PHLWINDOW &PWINDOW);
// eWindowState is a SCOPED enum in 0.57, so `m_state & WINDOW_STATE_PINNED` does not
// compile without a cast. This mirrors what Hyprland's own Actions::pinWindow() does.
static bool pinnedBit(const PHLWINDOW &W)
{
    return W && (((uint8_t)W->m_state & (uint8_t)Desktop::View::WINDOW_STATE_PINNED) != 0);
}

static std::string luaQuote(const std::string &TEXT);
static void        dispatchLua(const std::string &what, const std::string &code);
static void        dispatchLuaSync(const std::string &what, const std::string &code);

// eRectCorner is a bit mask: CORNER_LEFT alone is "left edge only", not "left corner".
// Mapping our edge name onto it is what gives single-axis resizing.
static Layout::eRectCorner cornerForEdge(const char *EDGE)
{
    if (!EDGE)
        return Layout::CORNER_NONE;

    const std::string_view E = EDGE;

    // Every name in resizeEdgeAt() is unique, so an exact match is enough.
    if (E == "top_left_corner") return Layout::CORNER_TOPLEFT;
    if (E == "top_right_corner") return Layout::CORNER_TOPRIGHT;
    if (E == "bottom_left_corner") return Layout::CORNER_BOTTOMLEFT;
    if (E == "bottom_right_corner") return Layout::CORNER_BOTTOMRIGHT;
    if (E == "left_side") return Layout::CORNER_LEFT;
    if (E == "right_side") return Layout::CORNER_RIGHT;
    if (E == "top_side") return Layout::CORNER_TOP;
    if (E == "bottom_side") return Layout::CORNER_BOTTOM;
    return Layout::CORNER_NONE;
}

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

    if (!m_pWindow->m_workspace || !m_pWindow->m_workspace->visible() || !g_pInputManager->m_exclusiveKeyboardLSes.empty() ||
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

    // Resize edge: claim the press BEFORE the bar logic, which would otherwise treat it as
    // a drag or a button. Only presses start a resize; releases fall through so the normal
    // drag-end path still runs.
    //
    // MBIND_RESIZE is what Hyprland's own border path uses (see
    // processMouseDownNormal: beginDragTarget(target, MBIND_RESIZE)), so the resize
    // behaves identically once started.
    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED && !Fullscreen::controller()->isFullscreen(m_pWindow.lock()))
    {
        const auto PWINDOW = m_pWindow.lock();

        // An item (button) wins over the resize band: pressing close on the right edge
        // must still close, not resize. resizeEdgeAt() is the same call the cursor uses,
        // so the grab zone and the arrow agree exactly.
        const auto  COORDS = cursorRelativeToBar();
        const char *EDGE   = resizeEdgeAt(COORDS);
        if (EDGE && elementIndexAt(COORDS) == -1 && indexToButton(COORDS) == -1)
        {
            // Pass the edge we already know. Without it Hyprland picks a CORNER from the
            // cursor quadrant alone (DragController::dragBegin), never a single edge, so a
            // press on the middle of the left border resized both axes.
            if (!g_layoutManager->dragController()->target())
                g_layoutManager->beginDragTarget(PWINDOW->layoutTarget(), MBIND_RESIZE, cornerForEdge(EDGE));

            info.cancelled = true;
            return;
        }
    }

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
    // The resize cursor for OUR band must be re-applied after Hyprland's own.
    // setCursorIconOnBorder() runs at the END of mouseMoveUnified(), i.e. AFTER this
    // listener, so setting the override here would be overwritten every frame. doLater
    // schedules a Wayland idle callback, which is after that function has returned.
    requestResizeCursorUpdate();

    if (m_bTouchEv || !validMapped(m_pWindow) || m_touchId != 0)
        return;

    updateElementHover();

    if (!gPlugin->m_vButtons.empty())
        damageOnButtonHover();

    if (!m_bDragPending)
        return;

    m_bDragPending = false;
    handleMovement();
}

// Map the resize band to an edge/corner shape, in the same vocabulary Hyprland uses in
// setCursorIconOnBorder() so the same theme sprites are picked.
// Which resize edge (if any) COORDS is on. THE single source of truth: the resize ACTION
// and the resize CURSOR both call this, so they cannot disagree by a pixel.
//
// Returns nullptr when the point is not in the band. A point outside the decoration box
// is never in the band -- that check lives here rather than at the call sites, because a
// caller that forgot it (the cursor did) reported edges a couple of pixels too wide.
const char *CHyprWindowDecorator::resizeEdgeAt(Vector2D COORDS)
{
    const double SC = gPlugin->cfg.defaultScale > 0 ? (double)gPlugin->cfg.defaultScale : 1.0;
    const double W  = gPlugin->cfg.resizeGrabWidth * SC;
    if (W <= 0)
        return nullptr;

    const auto BOX = assignedBoxGlobal();
    if (!VECINRECT(COORDS, 0, 0, BOX.w, BOX.h))
        return nullptr;

    const auto   EXT = currentExtents();
    const double L = EXT.topLeft.x, T = EXT.topLeft.y, R = EXT.bottomRight.x, B = EXT.bottomRight.y;

    // Clip each side to the frame actually present there: a side with no reserved space
    // has no band, and a thin side cannot offer more than it has.
    const double BANDL = L > 0 ? std::min(W, L) : 0;
    const double BANDR = R > 0 ? std::min(W, R) : 0;
    const double BANDT = T > 0 ? std::min(W, T) : 0;
    const double BANDB = B > 0 ? std::min(W, B) : 0;

    const bool ONL = BANDL > 0 && COORDS.x < BANDL;
    const bool ONR = BANDR > 0 && COORDS.x >= BOX.w - BANDR;
    const bool ONT = BANDT > 0 && COORDS.y < BANDT;
    const bool ONB = BANDB > 0 && COORDS.y >= BOX.h - BANDB;

    if (ONL && ONT) return "top_left_corner";
    if (ONR && ONT) return "top_right_corner";
    if (ONL && ONB) return "bottom_left_corner";
    if (ONR && ONB) return "bottom_right_corner";
    if (ONL) return "left_side";
    if (ONR) return "right_side";
    if (ONT) return "top_side";
    if (ONB) return "bottom_side";
    return nullptr;
}

// Keep the WINDOW_EDGE override in sync with the band. One slot is shared with Hyprland's
// own border logic, so leaving the band must UNSET it rather than set an arrow -- Hyprland
// will repaint its own answer on the next move.
void CHyprWindowDecorator::applyResizeCursor()
{
    if (!gPlugin->enabled || !validMapped(m_pWindow) || !Pointer::Cursor::overrideController)
        return;

    // Never fight a real resize/drag in progress.
    if (g_layoutManager && g_layoutManager->dragController()->target())
        return;

    const auto PWINDOW = m_pWindow.lock();
    if (Fullscreen::controller()->isFullscreen(PWINDOW))
        return;

    // Only the decorator under the POINTER may touch the shared override slot.
    // Otherwise every other decorated window would also run this and unset the override
    // that the hovered one just set, leaving whichever ran last as the winner.
    const auto HITTESTER = Desktop::viewState()->hitTest();
    const auto CURSOR    = g_pInputManager->getMouseCoordsInternal();
    const auto UNDER     = HITTESTER.windowAt(CURSOR, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

    if (UNDER != PWINDOW)
    {
        // Not us any more: clear only if WE were the last to set it. Hyprland repaints
        // its own answer on the next move, so unsetting is always safe here.
        if (m_bResizeCursorActive)
        {
            Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_WINDOW_EDGE);
            m_bResizeCursorActive = false;
        }
        return;
    }

    const auto COORDS = cursorRelativeToBar();

    // An item under the pointer keeps its own cursor (buttons must look clickable). The
    // band test itself is resizeEdgeAt(), the SAME call the resize action makes, so the
    // cursor and the grab zone are identical by construction.
    const bool ONITEM = elementIndexAt(COORDS) != -1 || indexToButton(COORDS) != -1;

    const char *SHAPE = ONITEM ? nullptr : resizeEdgeAt(COORDS);

    if (SHAPE)
    {
        Pointer::Cursor::overrideController->setOverride(SHAPE, Pointer::Cursor::CURSOR_OVERRIDE_WINDOW_EDGE);
        m_bResizeCursorActive = true;
    }
    else if (m_bResizeCursorActive)
    {
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_WINDOW_EDGE);
        m_bResizeCursorActive = false;
    }
}

void CHyprWindowDecorator::requestResizeCursorUpdate()
{
    if (m_bResizeCursorPending || !g_pEventLoopManager)
        return;

    m_bResizeCursorPending = true;

    g_pEventLoopManager->doLater([this]() {
        m_bResizeCursorPending = false;

        // Owned by Hyprland and possibly gone before this runs.
        if (!gPlugin)
            return;

        bool alive = false;
        for (auto *d : gPlugin->liveDecorators())
            if (d == this)
            {
                alive = true;
                break;
            }

        if (!alive)
            return;

        applyResizeCursor();
    });
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
        // Initial setup for dragging a window. One eval, because the calls are ordered:
        // float, size to half the monitor, then pin so the workspace can be changed
        // mid-drag. `setfloating` was the legacy name for this; `float { action =
        // "enable" }` is the Lua equivalent and, unlike a bare toggle, cannot un-float a
        // window that is already floating.
        //
        // The interactive grab is `hl.dsp.window.drag()`, which arms Hyprland's own
        // movewindow mouse bind and takes no argument table at all.
        //
        // Sizing replaces the old "exact 50% 50%". resize needs BOTH x and y (it rejects
        // an x or y it cannot read), and takes an absolute target size in window units,
        // so the half-monitor size is computed here. A 0-arg resize would arm a
        // mouse-resize grab instead of sizing the window.
        const auto PMONITOR = Desktop::focusState()->monitor();
        const auto W        = PMONITOR ? (PMONITOR->m_size.x * 0.5) / PMONITOR->m_scale : 0.0;
        const auto H        = PMONITOR ? (PMONITOR->m_size.y * 0.5) / PMONITOR->m_scale : 0.0;

        const std::string TARGET = luaWindowSelector(m_pWindow.lock());
        dispatchLuaSync("drag-start", std::format("hl.dispatch(hl.dsp.window.float({{ action = \"enable\", window = hl.get_window(\"{0}\") }})) "
                                                  "hl.dispatch(hl.dsp.window.resize({{ x = {1}, y = {2}, window = hl.get_window(\"{0}\") }})) "
                                                  "hl.dispatch(hl.dsp.window.pin({{ window = hl.get_window(\"{0}\") }})) "
                                                  "hl.dispatch(hl.dsp.window.drag())",
                                                  TARGET, (int)W, (int)H));
    }
    // Follow the finger. `movewindowpixel exact x y` maps onto move's absolute x/y form:
    // position is the target position, which is what the cursor-centred coords compute.
    dispatchLuaSync("drag-move", std::format("hl.dispatch(hl.dsp.window.move({{ x = {}, y = {}, window = hl.get_window(\"{}\") }}))",
                                             (int)(COORDS.x - (assignedBoxGlobal().w / 2)), (int)COORDS.y, luaWindowSelector(m_pWindow.lock())));
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
                dispatchLuaSync("drag-tile", std::format("hl.dispatch(hl.dsp.window.float({{ action = \"disable\", window = hl.get_window(\"{}\") }}))",
                                                         luaWindowSelector(m_pWindow.lock())));
            dispatchLuaSync("drag-release", "hl.dispatch(hl.dsp.window.drag())");
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

    if (PWINDOW->isFloating())
        Desktop::windowState()->raise(PWINDOW);

    // An interactive element under the pointer takes the press: show the pressed art
    // and swallow the click so it never becomes a window drag.
    m_iPressedElement = elementIndexAt(COORDS);
    if (m_iPressedElement != -1)
    {
        info.cancelled = true;
        damageEntire();

        return;
    }

    m_iButtonPressedIdx = indexToButton(COORDS);
    if (m_iButtonPressedIdx != -1)
    {
        damageEntire();
        return;
    }

    if (!ON_DOUBLE_CLICK.empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_lastMouseDown).count() < 400 /* Arbitrary delay I found suitable */)
    {
        dispatchLua("double-click", std::format("hl.dispatch(hl.dsp.exec_cmd({}))", luaQuote(ON_DOUBLE_CLICK)));
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
        dispatchLuaSync("drag-release", "hl.dispatch(hl.dsp.window.drag())");
        m_bDraggingThis = false;
        if (m_bTouchEv)
            dispatchLuaSync("drag-tile", std::format("hl.dispatch(hl.dsp.window.float({{ action = \"disable\", window = hl.get_window(\"{}\") }}))",
                                                     luaWindowSelector(m_pWindow.lock())));
    }

    m_bDragPending = false;
    m_bTouchEv = false;
    m_touchId = 0;

    if (m_iPressedElement != -1)
    {
        // Fire only if the pointer is STILL over the element that took the press, which
        // is the same rule the legacy buttons use: dragging off cancels.
        const int RELEASED = elementIndexAt(cursorRelativeToBar());
        if (RELEASED == m_iPressedElement)
            runElementAction((size_t)m_iPressedElement);

        m_iPressedElement = -1;
        damageEntire();
    }

    if (m_iButtonPressedIdx != -1)
    {
        if (indexToButton(cursorRelativeToBar()) == m_iButtonPressedIdx)
        {
            dispatchLua("button", std::format("hl.dispatch(hl.dsp.exec_cmd({}))", luaQuote(gPlugin->m_vButtons[m_iButtonPressedIdx].cmd)));
        }
        m_iButtonPressedIdx = -1;
        damageEntire();
    }
}

// A `maximize` toggle is THREE states, and the decorator state already tells which one a
// press should land on: tiled -> maximise, maximised -> restore, fullscreen -> restore
// (then a second press maximises). Deciding that here is what keeps `maximize` from being
// "go to state 1" with no way back. Both modes report maximised.
//
// Returns the mode as the Lua dispatcher names it, because fullscreen's `mode` field is a
// string ("maximized" / "fullscreen"). An earlier version returned the legacy "1,0" and
// the two were compared against each other, which silently never matched.

// ---------------------------------------------------------------------------
// Dispatchers. This Hyprland runs a Lua config, so `hyprctl dispatch` takes LUA and the
// legacy names (`closewindow`, `movetoworkspacesilent …`, `fullscreenstate 1,0`) are
// rejected with "expected a dispatcher (e.g. hl.dsp.window.close())". A rejected call is
// a silent no-op, which is why the eye did nothing before it was converted.
//
// Every call follows the same path: build the Lua source, schedule it on the event loop,
// then evaluate it against Hyprland's own Lua state. Evaluating inline would run Lua
// re-entrantly from inside a pointer event, which the repo rules forbid, and every
// dispatcher below addresses its window EXPLICITLY, so a delay cannot retarget it.
//
// The argument shapes are taken from the 0.56.2 sources, not guessed:
//   window.close({ window? })
//   window.float({ action?, window? })              action: toggle|enable|disable
//   window.pin({ action?, window? })
//   window.fullscreen({ action?, window?, mode?, layout_aware? })
//   window.move({ workspace = <selector>, follow?, window? })
//   window.move({ x, y, relative?, window? })
//   window.drag()                                    no argument table at all
//   window.resize({ x, y, relative?, window? })      needs BOTH x and y
//   exec_cmd(cmd)
// ---------------------------------------------------------------------------
static std::string luaWindowSelector(const PHLWINDOW &PWINDOW)
{
    return std::format("address:0x{:x}", (uintptr_t)PWINDOW.get());
}

// A Lua long-bracket string. User-configured commands contain quotes, `$` and
// backslashes often enough that %q-style escaping would be a bug waiting to happen; a
// long bracket copies bytes verbatim. The bracket level is raised until the closing
// sequence cannot appear inside the text: [[ ]] , then [=[ ]=] , then [==[ ]==] ...
static std::string luaQuote(const std::string &TEXT)
{
    for (size_t LEVEL = 0;; LEVEL++)
    {
        const auto EQ = std::string(LEVEL, '=');
        if (!TEXT.contains(std::format("]{}]", EQ)))
            return std::format("[{0}[{1}]{0}]", EQ, TEXT);
    }
}

// `what` names the caller for the log, so a failing dispatch is attributable.
static void dispatchLua(const std::string &what, const std::string &code)
{
    Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] action \"{}\": deferred lua dispatch of \"{}\"", what, code));

    g_pEventLoopManager->doLater([what, code]() {
        auto *const MGR = dynamic_cast<Config::Lua::CConfigManager *>(Config::mgr().get());
        if (!MGR)
        {
            Log::logger->log(Log::ERR, std::format("[HYPRDECOR] action \"{}\": no Lua config manager (not a Lua-config Hyprland)", what));
            return;
        }

        // eval() reports the Lua error instead of throwing; dropping it would put us
        // back to the silent no-op this conversion exists to remove.
        if (const auto ERR = MGR->eval(code); ERR)
            Log::logger->log(Log::ERR, std::format("[HYPRDECOR] action \"{}\": lua dispatch failed: {}", what, *ERR));
    });
}

// Synchronous variant for gestures, where ordering against the pointer matters: the
// drag must be armed before the next motion event arrives. Only the pointer path uses
// it; see the note at the call site.
static void dispatchLuaSync(const std::string &what, const std::string &code)
{
    Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] gesture \"{}\": lua dispatch of \"{}\"", what, code));

    auto *const MGR = dynamic_cast<Config::Lua::CConfigManager *>(Config::mgr().get());
    if (!MGR)
    {
        Log::logger->log(Log::ERR, std::format("[HYPRDECOR] gesture \"{}\": no Lua config manager (not a Lua-config Hyprland)", what));
        return;
    }

    if (const auto ERR = MGR->eval(code); ERR)
        Log::logger->log(Log::ERR, std::format("[HYPRDECOR] gesture \"{}\": lua dispatch failed: {}", what, *ERR));
}

// Runs an element's action. Named actions only: the plugin asks Hyprland to do
// something, it never launches anything (ROADMAP D2).
static const char *elementActionName(const SElement::eAction action)
{
    switch (action)
    {
        case SElement::eAction::Close: return "close";
        case SElement::eAction::Maximize: return "maximize";
        case SElement::eAction::Fullscreen: return "fullscreen";
        case SElement::eAction::Pin: return "pin";
        case SElement::eAction::Float: return "float";
        case SElement::eAction::Minimize: return "minimize";
        case SElement::eAction::RetroFilter: return "retro_filter";
        default: return "none";
    }
}

void CHyprWindowDecorator::runElementAction(const size_t index)
{
    if (index >= gPlugin->m_vElements.size())
        return;

    const auto &ELEMENT = gPlugin->m_vElements[index];
    const auto  PWINDOW = m_pWindow.lock();
    if (!PWINDOW)
        return;

    // Every action below addresses this window explicitly instead of relying on
    // "activewindow": the press handler does focus the window first, but the dispatch
    // is deferred to the event loop and focus can move in between.
    const std::string TARGET = luaWindowSelector(PWINDOW);
    const std::string WHAT   = elementActionName(ELEMENT.cfg.action);

    switch (ELEMENT.cfg.action)
    {
        case SElement::eAction::None: return;

        case SElement::eAction::Close:
            dispatchLua(WHAT, std::format("hl.dispatch(hl.dsp.window.close({{ window = hl.get_window(\"{}\") }}))", TARGET));
            return;

        // Fullscreen::eFullscreenMode is NONE=0, MAXIMIZED=1, FULLSCREEN=2.
        //
        // No `action` field: that routes to the plain toggle dispatcher, which does
        // "if isFullscreen(window, mode) -> NONE, else -> mode". Reading the state here
        // and choosing "set"/"unset" ourselves duplicated that decision, and the two
        // disagreed once the window was already maximised -- the button would maximise
        // but never restore. Letting Hyprland make the decision removes the mismatch.
        case SElement::eAction::Maximize:
            dispatchLua(WHAT, std::format("hl.dispatch(hl.dsp.window.fullscreen({{ mode = \"maximized\", window = hl.get_window(\"{}\") }}))", TARGET));
            return;

        case SElement::eAction::Fullscreen:
            // Same as Maximize: no `action`, so Hyprland's own per-mode toggle decides.
            dispatchLua(WHAT, std::format("hl.dispatch(hl.dsp.window.fullscreen({{ mode = \"fullscreen\", window = hl.get_window(\"{}\") }}))", TARGET));
            return;

        case SElement::eAction::Pin:
        {
            // Pinning a TILED window makes Hyprland complain and does nothing useful, so
            // float first. Verified behaviour (AGENTS.md): pin requires floating.
            // Both calls go in ONE eval, so the float cannot land after the pin.
            const std::string CODE = std::format("{}{}hl.dispatch(hl.dsp.window.pin({{ window = hl.get_window(\"{}\") }}))",
                                                 PWINDOW->isFloating() ? "" : std::format("hl.dispatch(hl.dsp.window.float({{ action = \"enable\", window = hl.get_window(\"{}\") }})) ", TARGET),
                                                 TARGET, TARGET);
            dispatchLua(WHAT, CODE);
            return;
        }

        case SElement::eAction::Float:
            dispatchLua(WHAT, std::format("hl.dispatch(hl.dsp.window.float({{ window = hl.get_window(\"{}\") }}))", TARGET));
            return;

        case SElement::eAction::Minimize:
        {
            // Project convention: park the window on special:min-<its current workspace>
            // so the bar can list exactly the windows of the workspace you are on.
            const auto PW  = m_pWindow.lock();
            const auto PWS = PW ? PW->m_workspace : nullptr;
            if (!PWS)
                return;

            // Already parked (a second click on the eye): do nothing rather than nest
            // special:min-<special:…>.
            if (PWS->addressableName().rfind("special:", 0) == 0)
                return;

            const std::string STASH = std::format("special:min-{}", PWS->addressableName());

            // Form verified in a terminal and confirmed working (AGENTS.md):
            //     hyprctl dispatch 'hl.dsp.window.move({ workspace = "special:min-2",
            //                        follow = false, window = hl.get_window("address:0x…") })'
            // `follow = false` is the "silent" half: focus stays where it is.
            dispatchLua(WHAT, std::format("hl.dispatch(hl.dsp.window.move({{ workspace = \"{}\", follow = false, window = hl.get_window(\"{}\") }}))",
                                          STASH, TARGET));
            return;
        }

        case SElement::eAction::RetroFilter:
        {
            // Runs `hyprshade toggle <shader>`.
            //
            // The shader MUST be named. With no argument, hyprshade infers one from its
            // schedule config, which we do not use -- shader and fallback both resolve to
            // None, and it calls Shader.off() unconditionally. That is why clicking the
            // eye appeared to do nothing: it was only ever switching the shader OFF.
            //
            // Naming it makes hyprshade compare against `hyprctl getoption
            // decoration.screen_shader` and flip it either way.
            //
            // On 0.57 a config change to decoration:screen_shader carries
            // REFRESH_SCREEN_SHADER, so hl.config() reloads the shader and damages the
            // monitor -- no restart needed.
            g_pEventLoopManager->doLater([]() {
                const std::string SHADER = gPlugin ? gPlugin->retro_shader : "";
                if (SHADER.empty())
                {
                    Log::logger->log(Log::ERR, "[HYPRDECOR] action \"retro_filter\": no retro shader configured");
                    return;
                }

                Log::logger->log(Log::INFO, std::format("[HYPRDECOR] action \"retro_filter\": hyprshade toggle {}", SHADER));

                // Detached: a hanging hyprshade must never wedge the compositor.
                const auto CMD = std::format("hyprshade toggle '{}' >/dev/null 2>&1 &", SHADER);
                if (std::system(CMD.c_str()) != 0)
                    Log::logger->log(Log::ERR, "[HYPRDECOR] action \"retro_filter\": could not start hyprshade");
            });
            return;
        }

    }
}

void CHyprWindowDecorator::handleMovement()
{
    dispatchLuaSync("drag", "hl.dispatch(hl.dsp.window.drag())");
    m_bDraggingThis = true;
    return;
}

int CHyprWindowDecorator::indexToButton(Vector2D COORDS)
{
    const bool VERTICAL = gPlugin->decoration_title_placement == "left" || gPlugin->decoration_title_placement == "right";

    // The legacy button strip is the top region's item area. It comes from the same
    // layout pass as everything else.
    const auto M     = frameMetrics();
    const auto AREA  = Layout::itemArea(M, Layout::eRegion::Top, Layout::regionRect(M, Layout::eRegion::Top));
    const CBox topBarBox = {AREA.x, AREA.y, AREA.w, AREA.h};

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

// The box the title text and its backdrop both live in. Two modes, mirroring the way
// `elements` are positioned:
//
//   "fill" : whatever the left- and right-anchored elements leave between them, with
//            `gap` as the distance from those elements.
//   "box"  : placed like an element, anchor + offset, then sized by width/height.
//
// Computing one box for both the text and the backdrop is what makes them impossible to
// desynchronise: there is no independent "title background" position.
CBox CHyprWindowDecorator::titleBox(const CBox &band, const float scale) const
{
    const auto &C = gPlugin->cfg;

    // "box" mode is an explicit placement: the title is not in the flow at all.
    if (C.titleMode == SConfig::eTitleMode::Box)
        return titleBoxPlaced(band, scale);

    // Fill mode: the title is a flow ITEM, so its rectangle comes from the layout pass.
    // Recomputing it here is what allowed the title to disagree with its neighbours.
    (void)band;
    auto &SELF = const_cast<CHyprWindowDecorator &>(*this);

    const auto RESOLVED = SELF.resolveLayout();
    if (RESOLVED.titleIndex < 0 || RESOLVED.titleVoids)
        return {0, 0, 0, 0};

    const auto &R = RESOLVED.items[RESOLVED.titleIndex].rect;
    return {R.x, R.y, R.w, R.h};
}

// "box" mode: the same idea as SElement. The anchor picks a corner/edge of the band,
// `offset` moves from it, and `width`/`height` size the stretch band in source px.
CBox CHyprWindowDecorator::titleBoxPlaced(const CBox &band, const float scale) const
{
    const auto &C = gPlugin->cfg;

    // 1. Size. `width`/`height` are the stretch band in source px, exactly like
    //    max_stretch on an element; the fixed corners come on top, never resized.
    const bool HASART = C.titleBgEnabled && gPlugin->m_pTitleBgTex && gPlugin->m_pTitleBgTex->m_texID != 0 && gPlugin->m_titleBgNPI.defined;
    const auto &NPI   = gPlugin->m_titleBgNPI;

    // LOGICAL units, like fill mode. artScale converts source px to logical; the monitor
    // scale is NOT involved here, the caller applies it once when drawing.
    //
    // artScale ALREADY includes defaults.scale when border.scale is unset (see the parser),
    // so it must not be multiplied by it again here.
    const double BS = (double)std::max(1, gPlugin->cfg.artScale);

    double w = -1, h = -1;
    if (HASART)
    {
        const double BANDW = C.titleWidth >= 0 ? C.titleWidth * BS : 0;
        const double BANDH = C.titleHeight >= 0 ? C.titleHeight * BS : 0;
        w = (NPI.border[0] + NPI.border[2]) * BS + BANDW;
        h = (NPI.border[1] + NPI.border[3]) * BS + BANDH;
    }
    else
    {
        if (C.titleWidth >= 0)
            w = C.titleWidth;
        if (C.titleHeight >= 0)
            h = C.titleHeight;
    }

    // A box with no explicit size still needs to be drawable: fall back to the band.
    if (w <= 0)
        w = band.width;
    if (h <= 0)
        h = band.height;

    // 2. Position from the anchor. Offsets are art units, so they follow defaults.scale.
    (void)scale;
    const double OS = gPlugin->cfg.defaultScale > 0 ? (double)gPlugin->cfg.defaultScale : 1.0;
    const auto R = TitleBox::layoutPlaced(band.x, band.y, band.width, band.height, w, h, (TitleBox::eAnchor)C.titleAnchor, C.titleOffsetX * OS,
                                          C.titleOffsetY * OS);

    return {R.x, R.y, R.w, R.h};
}

// Frame metrics, resolved ONCE from the config in logical px. Everything downstream
// (layout, drawing, hit-testing, status) consumes this, so no unit can be mixed twice.
Layout::SFrameMetrics CHyprWindowDecorator::frameMetrics()
{
    const auto &NPI = m_bWindowHasFocus ? gPlugin->activeNinepatch : gPlugin->inactiveNinepatch;
    const auto  BOX = assignedBoxGlobal();

    // `defaults.scale` is the single "make the whole frame bigger" knob: art, spacings,
    // offsets and text all follow it, so a scaled frame does not end up with a small icon
    // floating in a big band. Per-element `scale` still overrides the ART size only; the
    // spacing below is global by nature (there is one gap, one padding).
    //
    // The reserved extents (decoration_offset_*) are already scaled at resolve time from
    // the nine-patch, so they are NOT multiplied again here.
    const double S = gPlugin->cfg.defaultScale > 0 ? (double)gPlugin->cfg.defaultScale : 1.0;

    Layout::SFrameMetrics m;
    m.left          = gPlugin->decoration_offset_left;
    m.top           = gPlugin->decoration_offset_top;
    m.right         = gPlugin->decoration_offset_right;
    m.bottom        = gPlugin->decoration_offset_bottom;
    m.contentLeft   = NPI.padding[0];
    m.contentTop    = NPI.padding[1];
    m.contentRight  = NPI.padding[2];
    m.contentBottom = NPI.padding[3];
    m.padding       = gPlugin->decoration_padding * S;
    m.gap           = gPlugin->cfg.regionGap * S;
    m.stackEnd      = gPlugin->cfg.regionStackEnd;
    m.boxW          = BOX.w;
    m.boxH          = BOX.h;
    // inset_* are already scaled at resolve time (they feed the reserved extents), so they
    // are used as-is here.
    m.insetLeft     = gPlugin->inset_left;
    m.insetTop      = gPlugin->inset_top;
    m.insetRight    = gPlugin->inset_right;
    m.insetBottom   = gPlugin->inset_bottom;
    m.scale         = S;
    return m;
}

// Maps the configured elements to pure layout specs. Ids are the element indices, so the
// resolved item id is all the renderer needs to find its texture and config.
std::vector<Layout::SItemSpec> CHyprWindowDecorator::itemSpecs()
{
    std::vector<Layout::SItemSpec> specs;
    specs.reserve(gPlugin->m_vElements.size());

    for (size_t i = 0; i < gPlugin->m_vElements.size(); ++i)
    {
        const auto &ELEMENT = gPlugin->m_vElements[i];

        Layout::SItemSpec s;
        s.id              = (int)i;
        switch (ELEMENT.cfg.type)
        {
            case SElement::eType::Spacer: s.kind = Layout::eItemKind::Spacer; break;
            case SElement::eType::Text: s.kind = Layout::eItemKind::Text; break;
            case SElement::eType::Button: s.kind = Layout::eItemKind::Button; break;
            default: s.kind = ELEMENT.cfg.interactive ? Layout::eItemKind::Button : Layout::eItemKind::Image; break;
        }
        // Every distance an element declares is in ART units, so it scales with
        // defaults.scale -- otherwise a scaled icon sits at an unscaled offset and the row
        // drifts apart. `scale` below stays the ART multiplier (which a per-element value
        // may override); spacing is always global.
        const double S = gPlugin->cfg.defaultScale > 0 ? (double)gPlugin->cfg.defaultScale : 1.0;

        s.align           = ELEMENT.cfg.anchor == SElement::eAnchor::TopRight ? Layout::eAlign::End : Layout::eAlign::Start;
        s.offsetX         = ELEMENT.cfg.offsetX * S;
        s.offsetY         = ELEMENT.cfg.offsetY * S;
        s.w               = ELEMENT.cfg.width > 0 ? ELEMENT.cfg.width * S : ELEMENT.cfg.width;
        s.h               = ELEMENT.cfg.height > 0 ? ELEMENT.cfg.height * S : ELEMENT.cfg.height;
        s.scale           = (double)ELEMENT.cfg.scale.value_or(gPlugin->cfg.defaultScale > 0 ? gPlugin->cfg.defaultScale : 1);
        s.srcW            = ELEMENT.naturalSize.x;
        s.srcH            = ELEMENT.naturalSize.y;
        s.ninepatch       = ELEMENT.cfg.ninepatch;
        s.stretchVertical = ELEMENT.cfg.stretchVertical;
        s.minStretch      = ELEMENT.cfg.minStretch.value_or(-1);
        s.maxStretch      = ELEMENT.cfg.maxStretch.value_or(-1);
        s.npi             = ELEMENT.npi;
        s.collapse        = ELEMENT.cfg.collapse;
        s.fill            = ELEMENT.cfg.fill;
        s.weight          = ELEMENT.cfg.weight;
        s.crossAlign      = ELEMENT.cfg.crossAlign == "start" ? Layout::eAlign::Start : ELEMENT.cfg.crossAlign == "end" ? Layout::eAlign::End : Layout::eAlign::Centre;
        s.marginLeft      = ELEMENT.cfg.marginLeft * S;
        s.marginTop       = ELEMENT.cfg.marginTop * S;
        s.marginRight     = ELEMENT.cfg.marginRight * S;
        s.marginBottom    = ELEMENT.cfg.marginBottom * S;
        specs.push_back(s);
    }

    return specs;
}

Layout::SResult CHyprWindowDecorator::resolveLayout()
{
    // Elements are routed to their region; the title only sees top-region neighbours,
    // because that is the band it lives in.
    const auto M = frameMetrics();

    // Window state for `visible_when`, read ONCE per layout so every item sees the same
    // snapshot. A hidden item keeps its slot unless `collapse` is set.
    ItemState::SWindowState STATE;
    if (const auto W = m_pWindow.lock())
    {
        STATE.focused  = W == Desktop::focusState()->window();
        STATE.floating = W->isFloating();
        // 0.57 dropped the per-window `pinned` accessor; the state bit is how Hyprland's
        // own Actions::pinWindow() reads it.
        STATE.pinned   = pinnedBit(W);

        // isFullscreen takes the mode; MAXIMIZED and FULLSCREEN are distinct states, and
        // a pinned window can report neither even when the dispatcher "succeeded".
        const auto FS = Fullscreen::controller()->getFullscreenModes(W);
        STATE.maximised  = FS.internal == Fullscreen::FSMODE_MAXIMIZED || FS.internal == Fullscreen::FSMODE_FULLSCREEN;
        STATE.fullscreen = FS.internal == Fullscreen::FSMODE_FULLSCREEN;
    }

    std::vector<Layout::SItemSpec> top, bottom, left, right;
    for (auto &spec : itemSpecs())
    {
        if ((size_t)spec.id >= gPlugin->m_vElements.size())
            continue;

        const auto &CFG = gPlugin->m_vElements[spec.id].cfg;
        ItemState::SVisibleWhen WHEN;
        WHEN.focused    = CFG.whenFocused;
        WHEN.floating   = CFG.whenFloating;
        WHEN.maximised  = CFG.whenMaximised;
        WHEN.fullscreen = CFG.whenFullscreen;
        WHEN.pinned     = CFG.whenPinned;
        spec.hidden = WHEN.any() && !ItemState::visible(WHEN, STATE);

        switch (gPlugin->m_vElements[spec.id].cfg.region)
        {
            case SElement::eRegion::Bottom: bottom.push_back(spec); break;
            case SElement::eRegion::Left: left.push_back(spec); break;
            case SElement::eRegion::Right: right.push_back(spec); break;
            default: top.push_back(spec); break;
        }
    }

    // The title participates in the top region's flow like any other item, which is
    // what makes `title.gap` unnecessary: its spacing is `margin` now. Its id is one
    // past the last element, so it cannot collide with an element index.
    const auto &C = gPlugin->cfg;
    if (C.titleEnabled && C.titleMode == SConfig::eTitleMode::Fill)
    {
        Layout::SItemSpec t;
        t.id          = (int)gPlugin->m_vElements.size();
        t.kind        = Layout::eItemKind::Title;
        t.fill        = true;
        t.weight      = 1;
        // Title spacing and any fixed height are art units too, so they follow
        // defaults.scale like every other distance in the frame.
        const double TS = gPlugin->cfg.defaultScale > 0 ? (double)gPlugin->cfg.defaultScale : 1.0;
        t.h           = C.titleHeight > 0 ? C.titleHeight * TS : C.titleHeight; // <= 0 = the full band height
        t.marginLeft  = C.titleMarginLeft * TS;
        t.marginRight = C.titleMarginRight * TS;
        t.marginTop   = C.titleMarginTop * TS;
        t.marginBottom = C.titleMarginBottom * TS;
        t.crossAlign  = C.titleCrossAlign == "start" ? Layout::eAlign::Start : C.titleCrossAlign == "end" ? Layout::eAlign::End : Layout::eAlign::Centre;

        // Appended last so the end-aligned buttons keep their positions: the title takes
        // the space between the start and end groups, as it did before.
        top.push_back(t);
    }

    return Layout::compute(M, top, bottom, left, right);
}

int CHyprWindowDecorator::elementIndexAt(const Vector2D &local)
{
    // `local` is cursor - assignedBoxGlobal(), i.e. LOGICAL pixels, and the resolved
    // rectangles are logical too. Both sides now come from resolveLayout(), so the
    // pointer cannot land somewhere the art is not drawn.
    const auto RESOLVED = resolveLayout();

    for (const auto &item : RESOLVED.items)
    {
        // Neither the title nor a spacer nor text has an action, so none are hit-testable.
        if (item.kind == Layout::eItemKind::Title || item.kind == Layout::eItemKind::Spacer || item.kind == Layout::eItemKind::Text)
            continue;

        // A hidden item is not clickable, even though it still holds its slot.
        if (item.hidden)
            continue;

        if (item.id < 0 || (size_t)item.id >= gPlugin->m_vElements.size())
            continue;

        const auto &ELEMENT = gPlugin->m_vElements[item.id];
        if (!ELEMENT.cfg.interactive || ELEMENT.tex->m_texID == 0)
            continue;

        if (item.rect.contains(local.x, local.y))
            return item.id;
    }

    return -1;
}

void CHyprWindowDecorator::updateElementHover()
{
    if (!validMapped(m_pWindow))
        return;

    const int HOVERED = elementIndexAt(cursorRelativeToBar());
    const int PRESSED = m_iPressedElement;

    if (HOVERED != m_iHoveredElement)
    {
        m_iHoveredElement = HOVERED;
        damageEntire();
    }

    // Dragging the pointer off a pressed element cancels the press, like a button.
    if (PRESSED != -1 && HOVERED != PRESSED)
    {
        m_iPressedElement = -1;
        damageEntire();
    }
}

// pressed > hovered > unfocused > active. Returned whole so the caller gets the matching
// nine-patch markers too: a variant may be plain art where the base is a nine-patch.
// JSON for `hyprctl hyprdecor status`. Rectangles are the SAME logical, decoration-local
// values the renderer uses, so a reader can verify hover areas against the art. Global
// coordinates are decoration origin + rect.
std::string CHyprWindowDecorator::layoutJson() const
{
    const auto PWINDOW = m_pWindow.lock();
    if (!PWINDOW)
        return "null";

    // resolveLayout() is non-const only because it reads assignedBoxGlobal(); the result
    // is a pure function of config and window state.
    auto &SELF = const_cast<CHyprWindowDecorator &>(*this);

    const auto M        = SELF.frameMetrics();
    const auto RESOLVED = SELF.resolveLayout();
    const auto BOX      = SELF.assignedBoxGlobal();

    const auto ESC = [](const std::string &v) {
        std::string out;
        for (const char c : v)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
        return out;
    };

    const auto FMT = [](double v) { return std::format("{:.0f}", std::round(v)); };

    std::string out;
    out += std::format("{{\"address\": \"{}\",", ESC(std::format("0x{:x}", (uintptr_t)PWINDOW.get())));
    out += std::format(" \"decoration_box\": {{ \"x\": {}, \"y\": {}, \"w\": {}, \"h\": {} }},",
                       FMT(BOX.x), FMT(BOX.y), FMT(BOX.w), FMT(BOX.h));
    out += std::format(" \"reserved\": {{ \"left\": {}, \"top\": {}, \"right\": {}, \"bottom\": {} }},",
                       FMT(M.left), FMT(M.top), FMT(M.right), FMT(M.bottom));
    out += std::format(" \"frame\": {{ \"state\": \"{}\" }},", m_bWindowHasFocus ? "active" : "inactive");

    // Same emitter as the unit tests, so what status prints is what the tests assert.
    out += " \"items\": " + Layout::itemsJson(RESOLVED.items, BOX.x, BOX.y);

    out += " }";

    return out;
}

const SElementTexture &CHyprWindowDecorator::elementTexture(const size_t index, const bool focus) const
{
    const auto &ELEMENT = gPlugin->m_vElements[index];

    if (ELEMENT.cfg.interactive && m_iPressedElement == (int)index)
        return ELEMENT.pressed;

    if (ELEMENT.cfg.interactive && m_iHoveredElement == (int)index)
        return ELEMENT.hovered;

    // `_on` outranks the plain focused art: the toggle's state matters more than the
    // fact that the window is focused. Spec precedence is
    // pressed > hovered > _on > unfocused > active.
    if (const auto W = m_pWindow.lock())
    {
        const bool ISON = (ELEMENT.cfg.action == SElement::eAction::Pin && pinnedBit(W)) ||
                          (ELEMENT.cfg.action == SElement::eAction::Float && W->isFloating()) ||
                          (ELEMENT.cfg.action == SElement::eAction::Fullscreen &&
                           Fullscreen::controller()->getFullscreenModes(W).internal == Fullscreen::FSMODE_FULLSCREEN);

        if (ISON && ELEMENT.on.tex && ELEMENT.on.tex->m_texID != 0)
            return ELEMENT.on;
    }

    if (!focus)
        return ELEMENT.unfocused;

    return ELEMENT.active;
}

void CHyprWindowDecorator::renderBarTitle([[maybe_unused]] Render::CRenderContext &ctx, const CBox &band, const float scale)
{
    const bool VERTICAL = gPlugin->decoration_title_placement == "left" || gPlugin->decoration_title_placement == "right";

    const auto PWINDOW = m_pWindow.lock();

    // The text lives in the box the layout computed, so a background drawn for the same
    // box lines up by construction.
    //
    // titleBox() is LOGICAL; the canvas is device pixels, so this is the one place the
    // monitor scale is applied. The band comes in device-scaled (topBarBox), so recover
    // the logical band by dividing it out.
    // titleBox() works in DECORATION-LOCAL logical px, so the canvas size comes straight
    // from it. Its origin is not needed here: the texture is blitted later at the box
    // position by renderPass, so this function only renders pixels.
    const double M       = scale > 0 ? scale : 1.0;
    const CBox   LOGBAND = {band.x / M, band.y / M, band.width / M, band.height / M};
    const CBox   LOGBOX  = titleBox(LOGBAND, scale);
    const CBox   TITLEBOX{0, 0, LOGBOX.w * M, LOGBOX.h * M};

    // No room to draw anything: leave the texture empty rather than rendering text that
    // would spill outside the box.
    if (TITLEBOX.width < 1 || TITLEBOX.height < 1)
    {
        m_pTextTex = makeShared<Render::GL::CGLTexture>();
        return;
    }

    // Font size follows BOTH: `defaults.scale` (art units, so the text grows with the
    // frame) and the monitor scale (logical -> device). Previously only the monitor scale
    // was applied, so a frame scaled to 4 kept 13px-tall text.
    const double ART   = gPlugin->cfg.defaultScale > 0 ? (double)gPlugin->cfg.defaultScale : 1.0;
    const auto   scaledSize = gPlugin->decoration_title_size * ART * scale;

    const CHyprColor COLOR = m_bForcedTitleColor.value_or(gPlugin->col_text);

    // The canvas is the box, in BOTH directions: a box shorter than the band must not
    // render text at full band height (it would be squashed when blitted).
    const Vector2D bufferSize = {TITLEBOX.width, TITLEBOX.height};
    if (bufferSize.x <= 0 || bufferSize.y <= 0)
        return;

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

    [[maybe_unused]] const float logicalWidth = VERTICAL ? bufferSize.y : bufferSize.x;
    const float logicalHeight = VERTICAL ? bufferSize.x : bufferSize.y;

    // draw title using Pango
    PangoLayout *layout = pango_cairo_create_layout(CAIRO);
    pango_layout_set_text(layout, m_szLastTitle.c_str(), -1);

    // The font follows focus, like the frame art: an unfocused window gets
    // `title.font_unfocused` when it is set, and `title.font` otherwise.
    const std::string &FONT = m_bWindowHasFocus ? gPlugin->bar_text_font : gPlugin->bar_text_font_unfocused;
    PangoFontDescription *fontDesc = pango_font_description_from_string(FONT.c_str());
    pango_font_description_set_size(fontDesc, scaledSize * PANGO_SCALE);
    pango_layout_set_font_description(layout, fontDesc);
    pango_font_description_free(fontDesc);

    PangoContext *context = pango_layout_get_context(layout);
    pango_context_set_base_dir(context, PANGO_DIRECTION_NEUTRAL);

    // The canvas is exactly the title box. titleInset is padding INSIDE it, so the text
    // gets the box minus that padding on both sides.
    const float INSET = (float)gPlugin->cfg.titleInset * scale;
    const float maxWidth = std::max(0.0f, (float)bufferSize.x - INSET * 2.0f);

    pango_layout_set_width(layout, (int)std::round(maxWidth * PANGO_SCALE));
    if (gPlugin->cfg.titleEllipsize)
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    else
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);

    cairo_set_source_rgba(CAIRO, COLOR.r, COLOR.g, COLOR.b, COLOR.a);

    int layoutWidth, layoutHeight;
    pango_layout_get_size(layout, &layoutWidth, &layoutHeight);

    // Alignment of the text INSIDE the box (the box itself is already positioned).
    // In box mode it follows the anchor, so `anchor = "centre"` centres both; in fill
    // mode it is `align`.
    float align = 0.0f;
    if (gPlugin->cfg.titleMode == SConfig::eTitleMode::Box)
        align = (float)TitleBox::textAlignFor((TitleBox::eAnchor)gPlugin->cfg.titleAnchor);
    else if (gPlugin->cfg.titleAlign == "right")
        align = 1.0f;
    else if (gPlugin->cfg.titleAlign == "centre")
        align = 0.5f;

    // --- vertical placement --------------------------------------------------
    //
    // `layoutHeight` is the MAXIMUM over every run in the string. A title is normally one
    // run of the configured pixel font, but one missing glyph -- an accented letter, an em
    // dash -- makes Pango fall back to a different family for that run. If that family is
    // taller, the laid-out line grows and the text shifts, which is why a terminal and a
    // browser looked misaligned by a fixed number of pixels: their titles contain
    // different fallback glyphs.
    //
    // Fix: position the line by the DECLARED font's baseline, not by the tallest run.
    // `line` mode keeps the old behaviour for anyone who wants it.
    //
    // Measured with a 30px and a 44px run mixed into a 16px line: centring the layout
    // height moved the ink centre 19.5 -> 25.5 -> 35.5, while anchoring the baseline kept
    // the ink bottom fixed at row 27 in all three cases.
    double lineTop; // cairo origin: pango_cairo_show_layout() draws from the LINE TOP
    if (gPlugin->cfg.titleBaseline == "line")
    {
        lineTop = logicalHeight / 2.0 - layoutHeight / PANGO_SCALE / 2.0;
    }
    else
    {
        PangoFontDescription *declared = pango_font_description_from_string(FONT.c_str());
        pango_font_description_set_size(declared, scaledSize * PANGO_SCALE);
        PangoFontMetrics *metrics = pango_context_get_metrics(context, declared, nullptr);
        const double      ASCENT  = pango_font_metrics_get_ascent(metrics) / (double)PANGO_SCALE;
        const double      DESCENT = pango_font_metrics_get_descent(metrics) / (double)PANGO_SCALE;
        pango_font_metrics_unref(metrics);
        pango_font_description_free(declared);

        // Where the DECLARED font's baseline should sit: centre its own ascent/descent.
        const double WANT = (logicalHeight + (ASCENT - DESCENT)) / 2.0;

        // Where the LINE's baseline actually is. Pango puts every run on the tallest run's
        // baseline, so this is NOT the declared ascent -- ask Pango rather than assume.
        PangoLayoutIter *iter = pango_layout_get_iter(layout);
        const double     LINEBASE = iter ? pango_layout_iter_get_baseline(iter) / (double)PANGO_SCALE : ASCENT;
        if (iter)
            pango_layout_iter_free(iter);

        lineTop = WANT - LINEBASE;
    }

    // Both modes end at ONE cairo origin. "line" reproduces the old behaviour exactly
    // (origin at the top of the laid-out line, offset 0 by default); "font" puts the
    // origin on the declared font's baseline. baseline_offset is a nudge for the pixel
    // fonts whose ink sits off their nominal metrics, and applies to both.
    const double TOFF    = (gPlugin->cfg.defaultScale > 0 ? (double)gPlugin->cfg.defaultScale : 1.0) * M;
    const int    xOffset = (int)std::round(INSET + (maxWidth - (float)layoutWidth / PANGO_SCALE) * align + gPlugin->cfg.textOffsetX * TOFF);
    const double yOrigin = lineTop + (gPlugin->cfg.titleBaselineOffset + gPlugin->cfg.textOffsetY) * TOFF;

    cairo_move_to(CAIRO, xOffset, yOrigin);
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

void CHyprWindowDecorator::renderBarButtonsText(Render::CRenderContext &ctx, CBox *barBox, const float scale, const float a)
{
    const bool VERTICAL = gPlugin->decoration_title_placement == "left" || gPlugin->decoration_title_placement == "right";
    const auto visibleCount = getVisibleButtonCount(VERTICAL ? Vector2D((double)barBox->height, (double)barBox->width) : Vector2D((double)barBox->width, (double)barBox->height), scale);
    const auto COORDS = cursorRelativeToBar();

    const bool BUTTONSRIGHT = gPlugin->bar_buttons_alignment != "left";
    const float iconReserved = gPlugin->decoration_appicon_enabled ? (VERTICAL ? barBox->width : barBox->height) : 0;
    double offset = (gPlugin->decoration_padding * scale) + (BUTTONSRIGHT ? 0 : iconReserved);

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
        Render::GL::g_pHyprOpenGL->renderTexture(ctx, tex, pos, data);
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

void CHyprWindowDecorator::draw(Render::CRenderContext &ctx, PHLMONITOR pMonitor, const float &a, [[maybe_unused]] const Render::SWindowRenderPresentation &presentation)
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
    ctx.m_pass.add(makeUnique<CRenderPassElement>(data));
}

void CHyprWindowDecorator::renderPass(Render::CRenderContext &ctx, PHLMONITOR pMonitor, const float &a)
{
    const auto PWINDOW = m_pWindow.lock();

    bool windowFocus = PWINDOW == Desktop::focusState()->window();
    bool focusChanged = windowFocus != m_bWindowHasFocus;
    if (focusChanged)
    {
        m_bWindowHasFocus = windowFocus;
        m_bButtonsDirty = true;
        // The title texture is keyed on title text and box size only, so a focus change
        // that swaps `title.font_unfocused` in would otherwise keep the old pixels.
        m_bForceTitleRerender = true;
        damageEntire();
    }

    // `visible_when` depends on floating / pinned / fullscreen too, and none of those
    // damage the decoration on their own: an item would keep its old visibility until
    // something else happened to repaint. Compare a cheap snapshot instead.
    {
        const auto W = m_pWindow.lock();
        const auto FS = Fullscreen::controller()->getFullscreenModes(W);
        const int  SNAP = (W->isFloating() ? 1 : 0) | (pinnedBit(W) ? 2 : 0) | (FS.internal != Fullscreen::FSMODE_NONE ? 4 : 0) |
                         (FS.internal == Fullscreen::FSMODE_FULLSCREEN ? 8 : 0);

        if (SNAP != m_iLastStateSnap)
        {
            m_iLastStateSnap = SNAP;
            damageEntire();
        }
    }

    const CHyprColor DEST_COLOR = m_bForcedBarColor.value_or(gPlugin->bar_color);
    if (DEST_COLOR != m_cRealBarColor->goal())
        *m_cRealBarColor = DEST_COLOR;

    CHyprColor color = m_cRealBarColor->value();

    color.a *= a;
    const bool SHOULDBLUR = gPlugin->bar_blur && color.a < 1.F;

    // Only window rounding matters here: the stencil path below masks out the window
    // area so the frame does not paint over the client surface. Including the border
    // size made it engage even with rounding = 0, which masked the whole frame and
    // left only a black rectangle.
    const auto ROUNDING = PWINDOW->presentation().rounding();

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

    Render::GL::g_pHyprOpenGL->scissor(ctx, titleBarBox);

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
        // Named rather than a brace-init list: 0.57 added members to SRectRenderData, and
        // a partial designated initialiser warns on every new field. Assigning only what
        // we need keeps the defaults for the rest and stays warning-free.
        Render::GL::CHyprOpenGLImpl::SRectRenderData CLEARDATA;
        CLEARDATA.round         = (int)scaledRounding;
        CLEARDATA.roundingPower = m_pWindow->presentation().roundingPower();
        Render::GL::g_pHyprOpenGL->renderRect(ctx, windowBox, CHyprColor(0, 0, 0, 0), CLEARDATA);
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
                    // All nine cells are painted. The window is always drawn on top of
                    // the decoration, so a "hollow centre" option would only ever hide
                    // frame art behind the client.
                    cairo_save(CAIRO);

                    bool isMiddlePatch = (i == 1 || j == 1);
                    if (gPlugin->ninepatch_repeat && isMiddlePatch)
                    {
                        drawRepeatedSurface(CAIRO, sourceSurface, sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j], dx[i], dy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j], RESAMPLE);
                    }
                    else
                    {
                        drawSizedSurface(CAIRO, sourceSurface, sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j], dx[i], dy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j], RESAMPLE);
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
        Render::GL::g_pHyprOpenGL->renderTexture(ctx, m_pBarFinalTex, titleBarBox, data);
    }
    else
    {
        // Named structs rather than brace-init lists: 0.57 added members to
        // SRectRenderData, and a partial designated initialiser warns on every new one.
        Render::GL::CHyprOpenGLImpl::SRectRenderData BARDATA;
        BARDATA.round         = (int)scaledRounding;
        BARDATA.roundingPower = m_pWindow->presentation().roundingPower();

        if (SHOULDBLUR)
        {
            BARDATA.blur  = true;
            BARDATA.blurA = a;
        }

        Render::GL::g_pHyprOpenGL->renderRect(ctx, titleBarBox, color, BARDATA);
    }

    if (m_bWindowSizeChanged)
        m_bButtonsDirty = true;

    // Items. ONE layout pass produces the rectangles; hit-testing and
    // `hyprctl hyprdecor status` read the very same ones.
    {
        const auto RESOLVED = resolveLayout();

        for (const auto &item : RESOLVED.items)
        {
            // These take flow space but are not drawn from a texture here: the title is
            // rendered by renderBarTitle, a spacer draws nothing, and text is not
            // implemented yet (C5 templates).
            if (item.kind == Layout::eItemKind::Title || item.kind == Layout::eItemKind::Spacer || item.kind == Layout::eItemKind::Text)
                continue;

            // Hidden by `visible_when`: it keeps its slot but is not painted.
            if (item.hidden)
                continue;

            if (item.id < 0 || (size_t)item.id >= gPlugin->m_vElements.size())
                continue;

            const auto &element = gPlugin->m_vElements[item.id];
            if (!element.tex || element.tex->m_texID == 0)
                continue;

            // Which variant to draw: pressed > hovered > unfocused > active.
            const auto &STATE = elementTexture((size_t)item.id, m_bWindowHasFocus);
            if (!STATE.tex || STATE.tex->m_texID == 0)
                continue;

            // Logical -> device, applied exactly once. The rects are decoration-local,
            // so add the decoration's device origin.
            const double M   = pMonitor->m_scale;
            const double DEV = item.artScale * M;

            Render::GL::CHyprOpenGLImpl::STextureRenderData edata;
            edata.a = a;

            if (element.cfg.ninepatch && STATE.npi.defined)
            {
                // The grid was already resolved by the layout pass; only the scale to
                // device pixels remains. Recomputing it here is how the caps started
                // disagreeing with what was drawn.
                const double SRCW = element.naturalSize.x;
                const double SRCH = element.naturalSize.y;

                const auto GRID = NinePatch::layout(STATE.npi, SRCW, SRCH, item.rect.w, item.rect.h, item.artScale,
                                                    element.cfg.maxStretch.value_or(-1), element.cfg.stretchVertical ? element.cfg.maxStretch.value_or(-1) : 0,
                                                    element.cfg.minStretch.value_or(-1), element.cfg.stretchVertical ? element.cfg.minStretch.value_or(-1) : 0);

                const double X = titleBarBox.x + item.rect.x * M;
                const double Y = titleBarBox.y + item.rect.y * M;

                for (int gi = 0; gi < 3; ++gi)
                    for (int gj = 0; gj < 3; ++gj)
                    {
                        const double SW = GRID.sx[gi + 1] - GRID.sx[gi];
                        const double SH = GRID.sy[gj + 1] - GRID.sy[gj];
                        const double DW = GRID.dx[gi + 1] - GRID.dx[gi];
                        const double DH = GRID.dy[gj + 1] - GRID.dy[gj];
                        if (SW <= 0 || SH <= 0 || DW <= 0 || DH <= 0)
                            continue;

                        auto cellData = edata;
                        cellData.allowCustomUV               = true;
                        cellData.primarySurfaceUVTopLeft     = Vector2D(GRID.sx[gi] / SRCW, GRID.sy[gj] / SRCH);
                        cellData.primarySurfaceUVBottomRight = Vector2D(GRID.sx[gi + 1] / SRCW, GRID.sy[gj + 1] / SRCH);

                        Render::GL::g_pHyprOpenGL->renderTexture(ctx, STATE.tex, CBox{X + GRID.dx[gi] * M, Y + GRID.dy[gj] * M, DW * M, DH * M}, cellData);
                    }
            }
            else
            {
                const CBox DEST{titleBarBox.x + item.rect.x * M, titleBarBox.y + item.rect.y * M, item.rect.w * M, item.rect.h * M};
                Render::GL::g_pHyprOpenGL->renderTexture(ctx, STATE.tex, DEST, edata);
            }

            (void)DEV;
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

        Render::GL::CHyprOpenGLImpl::SRectRenderData RECTDATA;
        RECTDATA.round         = 0;
        RECTDATA.roundingPower = m_pWindow->presentation().roundingPower();

        // top, bottom, left, right
        Render::GL::g_pHyprOpenGL->renderRect(ctx, CBox{B.x, B.y, B.w, THICK}, outlineColor, RECTDATA);
        Render::GL::g_pHyprOpenGL->renderRect(ctx, CBox{B.x, B.y + B.h - THICK, B.w, THICK}, outlineColor, RECTDATA);
        Render::GL::g_pHyprOpenGL->renderRect(ctx, CBox{B.x, B.y + THICK, THICK, B.h - 2 * THICK}, outlineColor, RECTDATA);
        Render::GL::g_pHyprOpenGL->renderRect(ctx, CBox{B.x + B.w - THICK, B.y + THICK, THICK, B.h - 2 * THICK}, outlineColor, RECTDATA);
    }

    // The title band comes from the SAME layout pass as the elements, in logical px,
    // so the title and its neighbours cannot disagree about where the band is. This
    // replaced a hand-built device-space copy of the same arithmetic.
    const auto   LOGM       = frameMetrics();
    const auto   LOGBAND    = Layout::itemArea(LOGM, Layout::eRegion::Top, Layout::regionRect(LOGM, Layout::eRegion::Top));
    const double TITLEM     = pMonitor->m_scale;
    const CBox   LOGBOX{LOGBAND.x, LOGBAND.y, LOGBAND.w, LOGBAND.h};
    const CBox   LOGTITLEBOX = titleBox(LOGBOX, pMonitor->m_scale);

    // The legacy button strip still measures itself in device px; it is retired in
    // C3/C4, so it gets a device-space view of the same logical area rather than its own
    // arithmetic.
    CBox topBarBox{titleBarBox.x + LOGBAND.x * TITLEM, titleBarBox.y + LOGBAND.y * TITLEM, LOGBAND.w * TITLEM, LOGBAND.h * TITLEM};

    // Device space for the title, scaled exactly once.
    const CBox TITLEBOX{titleBarBox.x + LOGTITLEBOX.x * TITLEM, titleBarBox.y + LOGTITLEBOX.y * TITLEM, LOGTITLEBOX.w * TITLEM,
                        LOGTITLEBOX.h * TITLEM};

    // render app icon
    if (gPlugin->decoration_appicon_enabled)
    {
        std::string appId = PWINDOW->metadata().initialAppID();

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
            Render::GL::g_pHyprOpenGL->renderTexture(ctx, m_pAppIconTex, iconBox, data);
        }
    }

    // render title
    //
    // The cache key is the TITLE BOX, not the band. Keying on the band meant that
    // anything which moved or resized the box without changing the band -- a margin, for
    // instance -- reused the previous text texture, so the box moved and the pixels did
    // not. TITLEBOX is already in device px, matching the texture size.
    const Vector2D TEXTWANT{(double)TITLEBOX.width, (double)TITLEBOX.height};

    if (gPlugin->decoration_title_enabled &&
        (m_szLastTitle != PWINDOW->metadata().title() || m_bWindowSizeChanged || m_pTextTex->m_texID == 0 || m_bTitleColorChanged || m_bForceTitleRerender || m_pTextTex->m_size != TEXTWANT))
    {
        m_szLastTitle = PWINDOW->metadata().title();
        renderBarTitle(ctx, topBarBox, pMonitor->m_scale);
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

    if (gPlugin->decoration_title_enabled)
    {
        // Background and text share the SAME box, so they cannot drift apart: the
        // backdrop is derived from titleBox(), never positioned independently.
        const bool HASROOM = TITLEBOX.width >= 1 && TITLEBOX.height >= 1;

        if (HASROOM && gPlugin->cfg.titleBgEnabled && gPlugin->m_pTitleBgTex && gPlugin->m_pTitleBgTex->m_texID != 0 && gPlugin->m_titleBgNPI.defined)
        {
            const double BS = (double)std::max(1, gPlugin->cfg.artScale) * pMonitor->m_scale;

            // The box already has the size the stretch band asked for (titleBoxPlaced
            // sized it from the art), so the destination IS the grid. Corners keep
            // their pixel size and only the middle band follows.
            const auto GRID = NinePatch::layout(gPlugin->m_titleBgNPI, gPlugin->m_titleBgSize.x, gPlugin->m_titleBgSize.y,
                                                TITLEBOX.width, TITLEBOX.height, BS);

            // Same rule as the frame and the elements: the unfocused art is shown exactly
            // when frame_inactive is, i.e. the window does not have focus.
            const auto &BGTEX = m_bWindowHasFocus ? gPlugin->m_pTitleBgTex : gPlugin->m_pTitleBgTexUnfocused;

            Render::GL::CHyprOpenGLImpl::STextureRenderData bgData;
            bgData.a = a;

            for (int gi = 0; gi < 3; ++gi)
                for (int gj = 0; gj < 3; ++gj)
                {
                    const double SW = GRID.sx[gi + 1] - GRID.sx[gi];
                    const double SH = GRID.sy[gj + 1] - GRID.sy[gj];
                    const double DW = GRID.dx[gi + 1] - GRID.dx[gi];
                    const double DH = GRID.dy[gj + 1] - GRID.dy[gj];
                    if (SW <= 0 || SH <= 0 || DW <= 0 || DH <= 0)
                        continue;

                    auto cell = bgData;
                    cell.allowCustomUV                   = true;
                    cell.primarySurfaceUVTopLeft         = Vector2D(GRID.sx[gi] / gPlugin->m_titleBgSize.x, GRID.sy[gj] / gPlugin->m_titleBgSize.y);
                    cell.primarySurfaceUVBottomRight     = Vector2D(GRID.sx[gi + 1] / gPlugin->m_titleBgSize.x, GRID.sy[gj + 1] / gPlugin->m_titleBgSize.y);

                    Render::GL::g_pHyprOpenGL->renderTexture(ctx, BGTEX, CBox{TITLEBOX.x + GRID.dx[gi], TITLEBOX.y + GRID.dy[gj], DW, DH}, cell);
                }
        }

        if (HASROOM && m_pTextTex->m_texID)
        {
            // The text texture is exactly the title box, so it is blitted 1:1.
            CBox textBox = {TITLEBOX.x, TITLEBOX.y, (double)m_pTextTex->m_size.x, (double)m_pTextTex->m_size.y};
            Render::GL::CHyprOpenGLImpl::STextureRenderData data;
            data.a = a;
            Render::GL::g_pHyprOpenGL->renderTexture(ctx, m_pTextTex, textBox, data);
        }
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
        Render::GL::g_pHyprOpenGL->renderTexture(ctx, m_pButtonsTex, topBarBox, data);
    }

    Render::GL::g_pHyprOpenGL->disableScissor();

    renderBarButtonsText(ctx, &topBarBox, pMonitor->m_scale, a);

    m_bWindowSizeChanged = false;
    m_bTitleColorChanged = false;
    m_bForceTitleRerender = false;
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
    requestMinSizeClamp();
    damageEntire();
}

// Ask for a clamp, but never do it inside a layout callback.
//
// clampWindowSize() calls setPositionGlobal(), which runs another layout pass and calls
// updateWindow() again. Clamping synchronously here recursed until the stack died -- the
// crash happened on the first window, right after "positioning: window 5x5".
//
// Deferring to the event loop breaks the cycle: a second request while one is already
// queued is dropped, so a burst of size changes still yields one clamp.
void CHyprWindowDecorator::requestMinSizeClamp()
{
    if (m_bMinSizeClampPending || !g_pEventLoopManager)
        return;

    m_bMinSizeClampPending = true;

    g_pEventLoopManager->doLater([this]() {
        // The decorator is owned by Hyprland and can be destroyed before this runs. Only
        // touch it if it is still among the live ones.
        if (!gPlugin)
            return;

        bool alive = false;
        for (auto *d : gPlugin->liveDecorators())
            if (d == this)
            {
                alive = true;
                break;
            }

        if (!alive)
            return;

        m_bMinSizeClampPending = false;
        enforceMinSize();
    });
}

void CHyprWindowDecorator::enforceMinSize()
{
    // The decorator can be gone by the time the deferred callback runs.
    const auto PWINDOW = m_pWindow.lock();
    if (!PWINDOW || !validMapped(PWINDOW))
        return;

    if (Fullscreen::controller()->isFullscreen(PWINDOW))
        return;

    const auto &NPI = m_bWindowHasFocus ? gPlugin->activeNinepatch : gPlugin->inactiveNinepatch;
    if (!NPI.defined)
        return;

    const double SC = (double)std::max(1, gPlugin->cfg.artScale);

    // The nine-patch needs its two corners plus at least one pixel of stretch band.
    double minW = (NPI.border[0] + NPI.border[2]) * SC + 1.0;
    double minH = (NPI.border[1] + NPI.border[3]) * SC + 1.0;

    // The app's own minimum still applies; never shrink below it.
    if (const auto CLIENTMIN = PWINDOW->minSize(); CLIENTMIN.has_value())
    {
        minW = std::max(minW, CLIENTMIN->x);
        minH = std::max(minH, CLIENTMIN->y);
    }

    const auto SIZE = PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    if (SIZE.x >= minW && SIZE.y >= minH)
        return;

    // No callback runs from here: we are already outside the layout pass.
    PWINDOW->clampWindowSize(Vector2D{minW, minH}, std::nullopt);
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

// Is COORDS inside the resize band? Delegates to resizeEdgeAt() so the ACTION and the
// CURSOR answer from one expression; this used to be a second copy of the same maths and
// the two disagreed at the box boundary.
bool CHyprWindowDecorator::isInResizeBand(Vector2D COORDS)
{
    return resizeEdgeAt(COORDS) != nullptr;
}

eDecorationLayer CHyprWindowDecorator::getDecorationLayer()
{
    // Frame and items are drawn OVER the window by default: under, the client painted
    // over the frame's inner edge and over any item that overlapped the client area,
    // which made `inset` look broken. OVER (not OVERLAY) keeps the window's own popups
    // and menus above the frame.
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

    // 0.57 removed the workspace render-offset member: workspace animation is applied by
    // the presentation system, and draw() receives the result in
    // SWindowRenderPresentation (floatingOffset etc). Nothing to translate here any more.
    // The old guard also skipped the offset for pinned windows; that concept moved to
    // presentation().pinned and no longer affects this geometry.

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
    m_bForceTitleRerender = true;
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
