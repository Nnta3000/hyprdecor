#pragma once

#include <hyprlang.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <cairo/cairo.h>
#include "ninepatch.hpp"
#include "config.hpp"

// Runtime state for a configured SElement: the loaded texture and its natural size.
// One of the four visual states an element can be in. Every state keeps the geometry of
// the ACTIVE texture: a hovered variant replaces pixels, never the element's size or
// place, so a state swap can never make the layout jump.
struct SElementTexture
{
    SP<Render::ITexture> tex = makeShared<Render::GL::CGLTexture>();
    SNinePatchInfo       npi;
};

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

    // Queues a layout recalculation for the next main-loop tick. Safe to call from
    // inside a render pass, unlike recalculateMonitor() itself.

    // True once the config manager has committed this plugin's values.

    // Shows an on-screen notification (and logs) for a hard configuration error.
    void notifyError(const std::string &message);

    // Human-readable state, for `hyprctl hyprdecor status`.
    std::string statusString() const;

    // First existing file for `base` + one of `suffixes`; empty when none match.
    std::string resolveTexturePath(const std::string &base, const std::vector<std::string> &suffixes, const std::string &fallback = "");

    // "close.png" + "_hovered" -> the path of close_hovered.png, or `base` when that file
    // does not exist. The suffix goes BEFORE the extension: suffixing the raw path would
    // look for "close.png_hovered", which is why element variants silently fell back.
    std::string statePath(const std::string &base, const char *suffix) const;

    // The whole configuration, filled by hl.plugin.hyprdecor.setup().
    SConfig cfg;

    CHyprColor bar_color = CHyprColor(0.2F, 0.2F, 0.2F, 0.53F);
    int decoration_offset_top = 0;
    CHyprColor col_text = CHyprColor(1, 1, 1, 1);
    int decoration_title_size = 10;
    bool decoration_title_enabled = true;
    bool bar_blur = false;
    std::string bar_text_font = "Sans";
    std::string decoration_title_placement = "top";
    bool bar_part_of_window = true;
    bool bar_precedence_over_border = false;
    std::string bar_buttons_alignment = "right";
    int decoration_padding = 7;
    int bar_button_padding = 5;
    bool enabled = true;
    std::string on_double_click;

    std::string ninepatch_texture;

    // Values coming from hl.plugin.hyprdecor.setup(). They take precedence over the
    // flat plugin:hyprdecor:* config values, because setup() is the structured API.
    bool        setup_applied = false;
    bool        ninepatch_forceTexture = false;
    int         setup_border_thickness = -1; // < 0 = keep the flat config value
    float       setup_border_alpha = -1.F;   // < 0 = keep the flat config value

    // Last filesystem problem hit by resolveTexturePath(), if any. Lets the Lua
    // error distinguish "missing" from "permission denied".
    std::string m_szLastResolveError;

    // Deferred update, see CPlugin::requestUpdate().
    bool m_bUpdatePending = false;

    // Last render state, exposed by CPlugin::statusString().
    int  lastBarW = 0;
    int  lastBarH = 0;
    bool lastDrawn = false;
    std::string ninepatch_active;
    std::string ninepatch_inactive;
    SNinePatchInfo activeNinepatch;
    SNinePatchInfo inactiveNinepatch;
    // Extra space added to the art thickness, per side, pushing the window inward.
    // Layout reserves art + inset; the nine-patch is stretched over that same box, so
    // there is no hole. A visual gap belongs in the texture.
    int inset_left = 0;
    int inset_top = 0;
    int inset_right = 0;
    int inset_bottom = 0;
    int decoration_offset_left = 0;
    int decoration_offset_right = 0;
    int decoration_offset_bottom = 0;
    bool ninepatch_linear_filtering = true;
    bool ninepatch_repeat = false;
    bool decoration_appicon_enabled = true;
    bool decoration_render_above = false;
    CHyprColor bar_outline_color = CHyprColor(1, 1, 1, 1);
    int bar_outline_width = 1;
    bool bar_outline_always = true;
    Vector2D decoration_appicon_offset;

    // Parsed results
    cairo_surface_t *activeSurface = nullptr;
    cairo_surface_t *inactiveSurface = nullptr;

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
    std::vector<CHyprSignalListener> m_vListeners;
};

inline std::unique_ptr<CPlugin> gPlugin;

