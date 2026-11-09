#pragma once

#define WLR_USE_UNSTABLE

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRule.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <cairo/cairo.h>
#include "plugin.hpp"
#include "layout.hpp"

class CHyprWindowDecorator : public IHyprWindowDecoration
{
public:
  CHyprWindowDecorator(PHLWINDOW);
  virtual ~CHyprWindowDecorator();

  virtual SDecorationPositioningInfo getPositioningInfo();

  // Reserved extents: the single source of truth shared by layout, drawing and input.
  SBoxExtents currentExtents() const;

  // The frame this decorator renders. Empty theme name = the default frame.
  const SFrame &frame() const;

  // Resolves the window's theme from the rule effect or the plugin-side class rules.
  std::string ruleTheme() const;

  virtual void onPositioningReply(const SDecorationPositioningReply &reply);

  virtual void draw(Render::CRenderContext &ctx, PHLMONITOR, float const &a, const Render::SWindowRenderPresentation &presentation);

  virtual eDecorationType getDecorationType();

  virtual void updateWindow(PHLWINDOW);

  virtual void damageEntire();

  virtual eDecorationLayer getDecorationLayer();

  virtual uint64_t getDecorationFlags();

  bool m_bButtonsDirty = true;

  virtual std::string getDisplayName();

  PHLWINDOW getOwner();

  void updateRules();

  void invalidateTextures();

  // The resolved layout as JSON, for `hyprctl hyprdecor status` and quickshell. This is
  // the same list the renderer draws from, so it is a truthful description of the screen.
  std::string layoutJson() const;



private:
  SBoxExtents m_seExtents;

  PHLWINDOWREF m_pWindow;

  // Empty = default frame. Otherwise resolved through CPlugin::frameFor(); storing the
  // name rather than a pointer keeps the decorator valid across a config reload.
  std::string m_themeName;

  CBox m_bAssignedBox;

  // Last window size we logged from getPositioningInfo(), to avoid log spam.
  Vector2D m_vLastLoggedSize = {-1, -1};

  SP<Render::ITexture> m_pTextTex;
  SP<Render::ITexture> m_pButtonsTex;
  SP<Render::ITexture> m_pBarFinalTex;

  SP<Render::ITexture> m_pAppIconTex;
  std::string m_szLastAppId;

  bool m_bWindowSizeChanged = false;
  bool m_bNeedsReposition = false;
  bool m_hidden = false;
  bool m_bTitleColorChanged = false;
  // Set when the title must be re-rasterised for a reason other than text, box or colour:
  // a focus change swaps `title.font_unfocused` in, and the cache key would miss it.
  bool m_bForceTitleRerender = false;
  bool m_bLastEnabledState = false;
  bool m_bWindowHasFocus = false;
  bool m_bNinePatchChanged = false;
  std::optional<CHyprColor> m_bForcedBarColor;
  std::optional<CHyprColor> m_bForcedTitleColor;

  Time::steady_tp m_lastMouseDown = Time::steadyNow();

  PHLANIMVAR<CHyprColor> m_cRealBarColor;

  Vector2D cursorRelativeToBar();
  bool isMouseOnBar();

  void renderPass(Render::CRenderContext &ctx, PHLMONITOR, float const &a);
  void renderBarTitle(Render::CRenderContext &ctx, const CBox &band, const float scale);

  // Where the title (and its background) goes, in the same space as topBarBox.
  // The title fills the gap between left- and right-anchored elements unless a
  // max_width caps it. Both the text and its backdrop are derived from this, so they
  // cannot drift apart.
  CBox titleBox(const CBox &band, const float scale) const;
  CBox titleBoxPlaced(const CBox &band, const float scale) const;

  // ONE layout pass. Drawing, hit-testing and `hyprctl hyprdecor status` all read the
  // list this returns, so the pointer can never land somewhere the art is not drawn.
  // Computed in LOGICAL pixels, decoration-local; the renderer scales the result once.
  Layout::SResult                resolveLayout();
  Layout::SFrameMetrics          frameMetrics();
  std::vector<Layout::SItemSpec> itemSpecs();

  // Recomputes m_iHoveredElement / m_iPressedElement from the cursor, damaging only
  // when the state actually changes.
  void updateElementHover();
  int  elementIndexAt(const Vector2D &local);

  // Which texture represents an element right now. `focus` is the window's focus.
  const SElementTexture &elementTexture(size_t index, const bool focus) const;

  // Runs a clicked element's action, if it has one.
  void runElementAction(size_t index);

  // Snapshot of floating/pinned/fullscreen, so `visible_when` repaints when it changes.
  int m_iLastStateSnap = -1;

  int m_iHoveredElement = -1;
  int m_iPressedElement = -1;
  void renderText(SP<Render::ITexture> out, const std::string &text, const CHyprColor &color, const Vector2D &bufferSize, const float scale, const int fontSize);
  bool renderBarButtons(const Vector2D &bufferSize, const float scale);
  void renderBarButtonsText(Render::CRenderContext &ctx, CBox *barBox, const float scale, const float a);
  void damageOnButtonHover();

  bool inputIsValid();
  bool isPointOnBar(Vector2D COORDS);

  // True when COORDS (decoration-local) is in the band along the OUTER edge that starts a
  // resize instead of a drag or an item click.
  bool isInResizeBand(Vector2D COORDS);

  // Clamp the window up to the frame's nine-patch minimum, so the decoration can never
  // be squeezed into collapse.
  //
  // requestMinSizeClamp() is what updateWindow() calls: it DEFERS the clamp to the event
  // loop, because clampWindowSize() re-enters layout and would otherwise recurse forever.
  void requestMinSizeClamp();
  void enforceMinSize();
  bool m_bMinSizeClampPending = false;

  // Last clamp attempt that did not stick. A fixed-size window (Vesktop's 5x5 loading
  // window, for example) ignores the clamp and stays the same size, and re-issuing the
  // identical request would loop: clampWindowSize() -> layout -> updateWindow() ->
  // requestMinSizeClamp() -> enforceMinSize(). Remembering both the window size and the
  // requested minimum lets a real resize (which changes the size) try again.
  //
  // Deliberately NOT cleared when the size satisfies the minimum: a fixed-size client can
  // accept the clamp for one layout pass and then revert, and clearing here would re-arm
  // the loop. A shrink to a different size is a different key and is still clamped once.
  Vector2D m_vLastMinSizeClampSize = {-1, -1};
  Vector2D m_vLastMinSizeClampMin  = {-1, -1};

  // Resize CURSOR over the band. Hyprland's own setCursorIconOnBorder() measures a band
  // around the CLIENT surface and skips any point inside a decoration, so it never shows
  // the resize icon on the frame. These re-apply it for the band, after Hyprland's pass.
  void requestResizeCursorUpdate();
  void applyResizeCursor();
  // Which resize edge COORDS is on ("left_side", "top_left_corner", ...) or nullptr.
  // THE single source of truth for the band: both the resize ACTION and the resize
  // CURSOR call it, so they cannot drift apart.
  const char *resizeEdgeAt(Vector2D COORDS);
  bool m_bResizeCursorPending = false;
  // True while THIS decorator owns the shared window-edge cursor override, so it never
  // clears one another window set.
  bool m_bResizeCursorActive = false;
  void onMouseButton(Event::SCallbackInfo &info, IPointer::SButtonEvent e);
  void onTouchDown(Event::SCallbackInfo &info, ITouch::SDownEvent e);
  void onTouchUp(Event::SCallbackInfo &info, ITouch::SUpEvent e);
  void onMouseMove(Vector2D coords);
  void onTouchMove(Event::SCallbackInfo &info, ITouch::SMotionEvent e);

  void handleDownEvent(Event::SCallbackInfo &info, std::optional<ITouch::SDownEvent> touchEvent);
  void handleUpEvent(Event::SCallbackInfo &info);
  void handleMovement();
  int indexToButton(Vector2D COORDS);

  CBox assignedBoxGlobal();

  CHyprSignalListener m_pMouseButtonCallback;
  CHyprSignalListener m_pTouchDownCallback;
  CHyprSignalListener m_pTouchUpCallback;

  CHyprSignalListener m_pTouchMoveCallback;
  CHyprSignalListener m_pMouseMoveCallback;

  std::string m_szLastTitle;
  Vector2D m_vLastTitleSize;

  bool m_bDraggingThis = false;
  bool m_bTouchEv = false;
  bool m_bDragPending = false;
  bool m_bCancelledDown = false;
  int m_touchId = 0;

  int m_iButtonPressedIdx = -1;

  // store hover state for buttons as a bitfield
  unsigned int m_iButtonHoverState = 0;

  // for dynamic updates
  int m_iLastHeight = 0;

  size_t getVisibleButtonCount(const Vector2D &bufferSize, const float scale);

  friend class CRenderPassElement;
};
