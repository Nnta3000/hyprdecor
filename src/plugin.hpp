#pragma once

#include <hyprlang.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>
#include <cairo/cairo.h>
#include <map>
#include "ninepatch.hpp"
#include "config.hpp"

/**
 * One visual state of an element (active, unfocused, hovered, pressed or on).
 * Every state keeps the geometry of the ACTIVE texture: a variant replaces pixels, never
 * the element's size or place, so a state swap cannot make the layout jump.
 */
struct SElementTexture
{
    SP<Render::ITexture> tex = makeShared<Render::GL::CGLTexture>();
    SNinePatchInfo       npi;
};

/** Runtime state of one configured SElement: its textures and natural size. */
struct SElementTex
{
    SElement             cfg;
    SP<Render::ITexture> tex = makeShared<Render::GL::CGLTexture>();
    Vector2D             naturalSize = {0, 0};

    // State variants, resolved from cfg.path by suffix. Unset ones share the active
    // texture, so drawing never has to test for null.
    SElementTexture active, unfocused, hovered, pressed, on;

    // What the currently loaded texture was built from, so a settings change forces a
    // reload instead of silently keeping the old pixels.
    std::string loadedPath;
    bool        loadedLinear = false;

    // Nine-patch markers, read once when the image loads.
    SNinePatchInfo npi;
};

struct SHyprButton
{
    std::string cmd = "";
    Vector2D size = {10, 10};

    // textured states
    SP<Render::ITexture> texActive = makeShared<Render::GL::CGLTexture>();
    SP<Render::ITexture> texInactive = makeShared<Render::GL::CGLTexture>();
    SP<Render::ITexture> texHover = makeShared<Render::GL::CGLTexture>();
    SP<Render::ITexture> texPressed = makeShared<Render::GL::CGLTexture>();

    std::string pathActive = "";
    std::string pathInactive = "";
    std::string pathHover = "";
    std::string pathPressed = "";
};

/** Resolved frame assets and metrics for the default config or one named theme. */
struct SFrame
{
    std::string    ninepatch_texture;
    std::string    ninepatch_active;
    std::string    ninepatch_inactive;
    SNinePatchInfo activeNinepatch;
    SNinePatchInfo inactiveNinepatch;
    cairo_surface_t *activeSurface = nullptr;
    cairo_surface_t *inactiveSurface = nullptr;

    // Extra space added to the art thickness, per side, pushing the window inward.
    int inset_left   = 0;
    int inset_top    = 0;
    int inset_right  = 0;
    int inset_bottom = 0;

    int decoration_offset_left   = 0;
    int decoration_offset_top    = 0;
    int decoration_offset_right  = 0;
    int decoration_offset_bottom = 0;

    // Effective art scale, already clamped to at least 1.
    int artScale = 1;

    // Resampling and tiling for this frame.
    bool linearFiltering = true;
    bool repeat          = false;
};

class CHyprWindowDecorator;

class CPlugin
{
public:
    CPlugin(HANDLE handle);
    ~CPlugin();

    void update();
    void loadAllTextures();

    // Coalesced update: safe to call from a Lua callback or an event handler.
    void requestUpdate();
    void processPendingUpdate();

    // Shows an on-screen notification (and logs) for a hard configuration error.
    void notifyError(const std::string &message);

    // Human-readable state, for `hyprctl hyprdecor status`.
    std::string statusString() const;

    // The frame for a named theme, or the default frame when the name is empty/unknown.
    const SFrame &frameFor(const std::string &name) const;

    // Resolves a frame texture (and its metadata) for the default config or one theme.
    SFrame resolveFrame(const SConfig &C, const STheme *theme);

    // First existing file for `base` + one of `suffixes`; empty when none match.
    std::string resolveTexturePath(const std::string &base, const std::vector<std::string> &suffixes, const std::string &fallback = "");

    // "close.png" + "_hovered" -> the path of close_hovered.png, or `base` when that file
    // does not exist. The suffix goes BEFORE the extension: suffixing the raw path would
    // look for "close.png_hovered", which is why element variants silently fell back.
    std::string statePath(const std::string &base, const char *suffix) const;

    // The whole configuration, filled by hl.plugin.hyprdecor.setup().
    SConfig cfg;

    CHyprColor bar_color = CHyprColor(0.2F, 0.2F, 0.2F, 0.53F);
    CHyprColor col_text = CHyprColor(1, 1, 1, 1);
    int decoration_title_size = 10;
    bool decoration_title_enabled = true;
    bool bar_blur = false;
    std::string bar_text_font = "Sans";
    // Empty means "same as bar_text_font": plugin.cpp resolves the fallback when the
    // configuration is applied, so renderBarTitle always has a usable name.
    std::string bar_text_font_unfocused;
    std::string decoration_title_placement = "top";
    bool bar_part_of_window = true;
    bool bar_precedence_over_border = false;
    std::string bar_buttons_alignment = "right";
    int decoration_padding = 7;
    int bar_button_padding = 5;
    bool enabled = true;
    std::string on_double_click;

    // The default frame. Named themes are resolved into their own SFrame entries.
    SFrame frame;
    std::map<std::string, SFrame> themes;

    /** True once hl.plugin.hyprdecor.setup() has been applied at least once. */
    bool        setup_applied = false;

    // Last filesystem problem hit by resolveTexturePath(), if any. Lets the Lua
    // error distinguish "missing" from "permission denied".
    std::string m_szLastResolveError;

    // Deferred update, see CPlugin::requestUpdate().
    bool m_bUpdatePending = false;

    // Last render state, exposed by CPlugin::statusString().
    int  lastBarW = 0;
    int  lastBarH = 0;
    bool lastDrawn = false;
    bool ninepatch_linear_filtering = true;
    bool ninepatch_repeat = false;
    bool decoration_appicon_enabled = true;
    bool decoration_render_above = false;
    CHyprColor bar_outline_color = CHyprColor(1, 1, 1, 1);
    int bar_outline_width = 1;
    bool bar_outline_always = true;
    Vector2D decoration_appicon_offset;

    HANDLE m_pHandle = nullptr;
    std::vector<SHyprButton> m_vButtons;
    std::vector<SElementTex> m_vElements;

    // Title backdrop: a nine-patch stretched to the title box, drawn behind the text.
    // The backdrop has the same active/unfocused pair as the frame: `_unfocused` is
    // shown whenever frame_inactive is, i.e. the window is not focused.
    SP<Render::ITexture> m_pTitleBgTex = makeShared<Render::GL::CGLTexture>();
    SP<Render::ITexture> m_pTitleBgTexUnfocused = makeShared<Render::GL::CGLTexture>();
    SNinePatchInfo       m_titleBgNPI;
    Vector2D             m_titleBgSize = {0, 0};
    std::string          m_szLoadedTitleBg;

    // Windows we have decorated. Decorators themselves are owned by Hyprland
    // (window->m_windowDecorations); storing their raw pointers here went stale as
    // soon as a decoration was destroyed or replaced, and passing a dangling pointer
    // to repositionDeco corrupted the compositor state.
    std::vector<PHLWINDOWREF> m_vWindows;

    // Live decorator for a window, or nullptr. Walks Hyprland's own list, so it can
    // never return a freed decorator.
    static CHyprWindowDecorator *decoratorFor(PHLWINDOW pWindow);

    // All live decorators we own.
    std::vector<CHyprWindowDecorator *> liveDecorators() const;
    uint32_t m_nobarRuleIdx = 0;
    uint32_t m_barColorRuleIdx = 0;
    uint32_t m_titleColorRuleIdx = 0;
    uint32_t m_themeRuleIdx = 0;
    std::vector<CHyprSignalListener> m_vListeners;

    // The registered `hyprctl hyprdecor` command. 0.57 returns a SHARED pointer to the
    // registered command, so it must be held: dropping it unregisters.
    SP<IPC::Socket1::SCommand> m_pStatusCommand;
};

inline std::unique_ptr<CPlugin> gPlugin;

