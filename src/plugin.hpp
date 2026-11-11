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

    HANDLE m_pHandle = nullptr;
    std::vector<SElementTex> m_vElements;

    // Title backdrop: a nine-patch stretched to the title box, drawn behind the text.
    // The backdrop has the same active/unfocused pair as the frame: `_unfocused` is
    // shown whenever frame_inactive is, i.e. the window is not focused.
    SP<Render::ITexture> m_pTitleBgTex = makeShared<Render::GL::CGLTexture>();
    SP<Render::ITexture> m_pTitleBgTexUnfocused = makeShared<Render::GL::CGLTexture>();
    SNinePatchInfo       m_titleBgNPI;
    Vector2D             m_titleBgSize = {0, 0};
    std::string          m_szLoadedTitleBg;
    // What the loaded title backdrop was uploaded with. Part of the cache key: the
    // resampling filter is baked in at upload time.
    bool                 m_bLoadedTitleBgLinear = true;

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

