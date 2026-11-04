#include "plugin.hpp"
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include "compat.hpp"
#include "hyprWindowDecorator.hpp"
#include "util.hpp"
#include <hyprland/src/config/ConfigManager.hpp>
#include <filesystem>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>

std::string CPlugin::statePath(const std::string &base, const char *suffix) const
{
    // The suffix goes BEFORE the extension: "close.png" -> "close_hovered.png". Suffixing
    // the raw path would look for "close.png_hovered", which never matches.
    std::string stem = base;
    const auto  DOT  = stem.find_last_of('.');
    const auto  SLASH = stem.find_last_of('/');
    if (DOT != std::string::npos && (SLASH == std::string::npos || DOT > SLASH))
        stem.erase(DOT);

    return gPlugin->resolveTexturePath(stem, {suffix}, base);
}

std::string CPlugin::resolveTexturePath(const std::string &base, const std::vector<std::string> &suffixes, const std::string &fallback)
{
    for (const auto &suffix : suffixes)
    {
        // Suffixes are matched both raw and with .png appended. The suffix lists for
        // frames are extension-less ("_active"), while explicit button paths carry
        // their extension, so both forms have to be tried.
        for (const std::string &candidate : {base + suffix, base + suffix + ".png"})
        {
            std::error_code ec;
            const bool EXISTS = std::filesystem::exists(candidate, ec);

            Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] trying \"{}\" -> exists={} ec={}",
                                                     candidate, EXISTS, ec.value()));

            if (ec)
            {
                m_szLastResolveError = std::format("\"{}\": {} (errno {})", candidate, ec.message(), ec.value());
                continue;
            }

            if (!EXISTS)
                continue;

            std::error_code ec2;
            if (std::filesystem::is_regular_file(candidate, ec2) && !ec2)
            {
                m_szLastResolveError.clear();
                return candidate;
            }
        }
    }

    return fallback;
}

CHyprWindowDecorator *CPlugin::decoratorFor(PHLWINDOW pWindow)
{
    if (!pWindow)
        return nullptr;

    for (const auto &deco : pWindow->presentation().decorations())
    {
        if (deco && deco->getDisplayName() == "Hyprdecor")
        {
            // RTTI does not reliably cross the plugin boundary, so use the pointer the
            // decorator records about itself. Every decorator is a
            // CHyprWindowDecorator: this plugin is the only source of the name above.
            return static_cast<CHyprWindowDecorator *>(deco.get());
        }
    }

    return nullptr;
}

std::vector<CHyprWindowDecorator *> CPlugin::liveDecorators() const
{
    std::vector<CHyprWindowDecorator *> out;

    // Prune expired refs as we go: window.destroy does not always reach us (unload,
    // non-standard teardown), so the list is perpetually self-healing.
    std::erase_if(const_cast<std::vector<PHLWINDOWREF> &>(m_vWindows), [](const auto &ref) { return !ref.lock(); });

    for (const auto &ref : m_vWindows)
    {
        const auto WINDOW = ref.lock();
        if (!WINDOW)
            continue;

        if (auto deco = decoratorFor(WINDOW))
            out.push_back(deco);
    }

    return out;
}

Hyprlang::CParseResult onNewButtonTextured([[maybe_unused]] const char *K, const char *V)
{
    std::string v = V;
    const auto vars = Compat::splitVars(v);

    Hyprlang::CParseResult result;

    // hyprdecor-button = size, action, active_tex, [inactive_tex], [hover_tex], [pressed_tex]

    if (vars[0].empty() || vars[1].empty() || vars[2].empty())
    {
        result.setError("size, action and active_tex cannot be empty");
        return result;
    }

    float size = 10;
    try
    {
        size = std::stof(vars[0]);
    }
    catch (std::exception &e)
    {
        result.setError("failed to parse size");
        return result;
    }

    SHyprButton button;
    button.size = {size, size};
    button.cmd = vars[1];

    if (vars.size() == 3)
    {
        std::string base = vars[2];
        button.pathActive = gPlugin->resolveTexturePath(base, {"", "_normal", "_active", "_focused"});
        button.pathInactive = gPlugin->resolveTexturePath(base, {"_unfocused", "_inactive"}, button.pathActive);
        button.pathHover = gPlugin->resolveTexturePath(base, {"_hover"}, button.pathActive);
        button.pathPressed = gPlugin->resolveTexturePath(base, {"_pressed", "_clicked"}, button.pathHover);
    }
    else
    {
        button.pathActive = vars[2];
        button.pathInactive = vars.size() > 3 ? vars[3] : vars[2];
        button.pathHover = vars.size() > 4 ? vars[4] : button.pathActive;
        button.pathPressed = vars.size() > 5 ? vars[5] : button.pathHover;
    }

    gPlugin->m_vButtons.push_back(button);

    for (auto bar : gPlugin->liveDecorators())
    {
        if (bar)
            bar->m_bButtonsDirty = true;
    }

    return result;
}

void CPlugin::loadAllTextures()
{
    // Elements: the config is ALWAYS copied, so editing an existing element's offsets
    // takes effect. (Comparing only the list size meant edits were ignored whenever the
    // count stayed the same.) Textures reload only when their inputs changed.
    if (m_vElements.size() != cfg.elements.size())
        m_vElements.resize(cfg.elements.size());

    for (size_t i = 0; i < cfg.elements.size(); ++i)
    {
        auto &element = m_vElements[i];
        element.cfg = cfg.elements[i];

        const bool LINEAR = element.cfg.filter ? *element.cfg.filter : cfg.linearFiltering;
        const bool NEEDS_RELOAD = element.tex->m_texID == 0 || element.loadedPath != element.cfg.path || element.loadedLinear != LINEAR;

        if (NEEDS_RELOAD && !element.cfg.path.empty())
        {
            element.tex = makeShared<Render::GL::CGLTexture>();
            element.npi = SNinePatchInfo{};

            if (element.cfg.ninepatch)
            {
                // Nine-patch elements need the markers, so go through loadSurface with
                // metadata parsing and upload the cropped result.
                cairo_surface_t *surface = loadSurface(element.cfg.path, &element.npi);
                if (surface)
                {
                    uploadCairoSurface(element.tex, surface, LINEAR);
                    cairo_surface_destroy(surface);
                }
            }
            else
                loadTexture(element.cfg.path, element.tex, LINEAR);
            element.loadedPath = element.cfg.path;
            element.loadedLinear = LINEAR;
            Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] element[{}] loaded \"{}\" filter={} (border.linear_filtering={}, override={})",
                                                      i, element.cfg.path, LINEAR ? "linear" : "nearest", cfg.linearFiltering,
                                                      element.cfg.filter ? (*element.cfg.filter ? "linear" : "nearest") : "none"));
            if (element.tex->m_texID != 0)
                element.naturalSize = element.tex->m_size;
            else
            {
                notifyError(std::format("element[{}]: couldn't load \"{}\"", i, element.cfg.path));
                continue;
            }

            // State variants: same base path, different suffix. A missing file is not an
            // error, it simply shares the active texture.
            element.active = {element.tex, element.npi};

            const auto LOADSTATE = [&](SElementTexture &slot, const char *suffix) {
                const std::string VARIANT = statePath(element.cfg.path, suffix);
                if (VARIANT == element.cfg.path)
                {
                    // No variant on disk: reuse the active one, geometry included.
                    slot = element.active;
                    return;
                }

                slot.tex = makeShared<Render::GL::CGLTexture>();
                slot.npi = SNinePatchInfo{};

                // Loaded EXACTLY like the active texture, which is what makes the swap
                // pixel-for-pixel. Passing a nine-patch info here used to crop a 1px
                // border off every variant, so a 13x14 icon became 11x12 and the hovered
                // art appeared cropped and zoomed relative to the idle art.
                if (element.cfg.ninepatch)
                {
                    cairo_surface_t *surface = loadSurface(VARIANT, &slot.npi, 0);
                    if (surface)
                    {
                        uploadCairoSurface(slot.tex, surface, LINEAR);
                        cairo_surface_destroy(surface);
                    }
                }
                else
                    loadTexture(VARIANT, slot.tex, LINEAR);

                if (slot.tex->m_texID == 0)
                {
                    notifyError(std::format("element[{}]: \"{}\" exists but couldn't be loaded", i, VARIANT));
                    slot = element.active;
                    return;
                }
            };

            // Non-interactive elements still get the unfocused swap, which is driven by
            // the window's focus rather than the pointer.
            LOADSTATE(element.unfocused, "_unfocused");

            // `_on` is the "toggle is active" art: pin while pinned, float while
            // floating. Loaded for any element, because an element without an action can
            // still be a state indicator.
            LOADSTATE(element.on, "_on");

            if (element.cfg.interactive)
            {
                LOADSTATE(element.hovered, "_hovered");
                LOADSTATE(element.pressed, "_pressed");
            }
            else
            {
                element.hovered = element.active;
                element.pressed = element.active;
            }
        }
    }

    // Title backdrop, loaded when its path changes. It has an active _unfocused pair,
    // like the frame: the swap follows window focus, not the pointer, so it needs no
    // hover/pressed states.
    if (cfg.titleBgEnabled && !cfg.titleBgPath.empty() && m_szLoadedTitleBg != cfg.titleBgPath)
    {
        m_pTitleBgTex          = makeShared<Render::GL::CGLTexture>();
        m_pTitleBgTexUnfocused = makeShared<Render::GL::CGLTexture>();
        m_titleBgNPI           = SNinePatchInfo{};

        cairo_surface_t *surface = loadSurface(cfg.titleBgPath, &m_titleBgNPI, 0);
        if (surface)
        {
            uploadCairoSurface(m_pTitleBgTex, surface, cfg.linearFiltering);
            m_titleBgSize = m_pTitleBgTex->m_size;
            cairo_surface_destroy(surface);
        }

        // The variant must keep the active geometry, so it is a swap of pixels only.
        // Loaded through the same marker path, because a nine-patch variant must share
        // the base's border; a missing file falls back to the active texture.
        const std::string VARIANT = statePath(cfg.titleBgPath, "_unfocused");
        if (VARIANT != cfg.titleBgPath)
        {
            SNinePatchInfo variantNPI;
            cairo_surface_t *vs = loadSurface(VARIANT, &variantNPI, 0);
            if (vs)
            {
                uploadCairoSurface(m_pTitleBgTexUnfocused, vs, cfg.linearFiltering);
                cairo_surface_destroy(vs);
            }
        }

        if (m_pTitleBgTexUnfocused->m_texID == 0)
            m_pTitleBgTexUnfocused = m_pTitleBgTex;

        m_szLoadedTitleBg = cfg.titleBgPath;
    }

    // load textures if they exist and are not loaded
    for (auto &button : m_vButtons)
    {
        if (!button.pathActive.empty() && button.texActive->m_texID == 0)
        {
            loadTexture(button.pathActive, button.texActive, ninepatch_linear_filtering);
            if (button.size.x <= 0 && button.texActive->m_texID != 0)
                button.size = button.texActive->m_size;
        }
        if (!button.pathInactive.empty() && button.texInactive->m_texID == 0)
            loadTexture(button.pathInactive, button.texInactive, ninepatch_linear_filtering);
        if (!button.pathHover.empty() && button.texHover->m_texID == 0)
            loadTexture(button.pathHover, button.texHover, ninepatch_linear_filtering);
        if (!button.pathPressed.empty() && button.texPressed->m_texID == 0)
            loadTexture(button.pathPressed, button.texPressed, ninepatch_linear_filtering);

        if (button.size.x <= 0)
            button.size = {20, 20};
    }
}

CPlugin::CPlugin(HANDLE handle)
{
    m_pHandle = handle;
    // No addConfigValue() registrations: the Lua config manager never commits them, so
    // every read needed a null check and a fallback. Configuration lives in SConfig and
    // is filled by hl.plugin.hyprdecor.setup().
}

CPlugin::~CPlugin()
{
    if (activeSurface)
        cairo_surface_destroy(activeSurface);
    if (inactiveSurface && inactiveSurface != activeSurface)
        cairo_surface_destroy(inactiveSurface);
}

// Read a config value's data pointer, or nullptr if the option is not
// registered/committed yet (e.g. during PLUGIN_INIT before the first reload).

void CPlugin::requestUpdate()
{
    // Mark dirty and let the next main-loop tick run update(). Calling update()
    // re-entrantly from a Lua callback would drive layout and damage while the
    // config manager is still evaluating.
    m_bUpdatePending = true;
    Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] update requested (this={})", (void *)this));
}

void CPlugin::processPendingUpdate()
{
    if (!m_bUpdatePending)
        return;

    m_bUpdatePending = false;

    // No readiness gate: the flat `plugin:hyprdecor:*` values it waited for no longer
    // exist (everything comes from setup()), so the check could only ever time out after
    // 60 ticks and log a warning. update() reads SConfig, which setup() applies whole or
    // not at all.
    Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] running deferred update (this={})", (void *)this));
    update();
}

std::string CPlugin::statusString() const
{
    std::string out;

    out += std::format("hyprdecor status (this={})\n", (const void *)this);
    out += std::format("  loaded:            yes\n");
    out += std::format("  setup() applied:   {}\n", setup_applied ? "yes" : "no");
    out += std::format("  texture (base):    {}\n", ninepatch_texture.empty() ? "(none)" : ninepatch_texture);
    out += std::format("  texture (active):  {}\n", ninepatch_active.empty() ? "(none)" : ninepatch_active);
    out += std::format("  texture (inactive): {}\n", ninepatch_inactive.empty() ? "(none)" : ninepatch_inactive);
    out += std::format("  surface loaded:    {}\n", activeSurface ? "yes" : "NO");
    out += std::format("  ninepatch parsed:  {}\n", activeNinepatch.defined ? "yes" : "NO");
    out += std::format("  border L/T/R/B:    {} {} {} {}\n",
                       activeNinepatch.border[0], activeNinepatch.border[1], activeNinepatch.border[2], activeNinepatch.border[3]);
    out += std::format("  padding L/T/R/B:   {} {} {} {}\n",
                       activeNinepatch.padding[0], activeNinepatch.padding[1], activeNinepatch.padding[2], activeNinepatch.padding[3]);
    out += std::format("  render fallback:   {}\n", activeSurface ? "texture" : "solid bar_color (NO TEXTURE)");
    out += std::format("  offsets L/T/R/B:   {} {} {} {}\n",
                       decoration_offset_left, decoration_offset_top, decoration_offset_right, decoration_offset_bottom);
    out += std::format("  enabled:           {}\n", enabled ? "yes" : "no");
    out += std::format("  decorated windows: {}\n", liveDecorators().size());
    out += std::format("  canvas (last):     {}x{}\n", lastBarW, lastBarH);
    out += std::format("  drawn (last):      {}\n", lastDrawn ? "yes" : "no");

    // The resolved layout as JSON: coordinates and sizes of every item, from the same
    // list the renderer draws, so it can be checked against the screen or consumed by
    // quickshell. One object per decorated window, joined into an array.
    out += "\nresolved layout (JSON):\n";
    out += std::format("{{ \"version\": 1, \"windows\": [");

    bool first = true;
    for (auto *bar : liveDecorators())
    {
        if (!bar)
            continue;
        const std::string JSON = bar->layoutJson();
        if (JSON == "null")
            continue;
        if (!first)
            out += ", ";
        first = false;
        out += "\n  " + JSON;
    }

    out += "\n] }\n";

    return out;
}

void CPlugin::update()
{
    // All configuration now comes from gPlugin->cfg, filled by setup(). There are no
    // nullable config pointers left, so no fallback dance and no uninitialised state.
    const SConfig &C = cfg;

    enabled = C.enabled;

    bar_color             = CHyprColor(C.barColor);
    col_text              = CHyprColor(C.textColor);
    bar_text_font         = C.titleFont;
    bar_text_font_unfocused = C.titleFontUnfocused.empty() ? C.titleFont : C.titleFontUnfocused;
    decoration_title_size = C.titleSize;
    decoration_title_enabled = C.titleEnabled;
    decoration_title_placement = C.titlePlacement;
    bar_blur                 = C.blur;
    bar_part_of_window       = C.partOfWindow;
    bar_precedence_over_border = C.precedenceOverBorder;
    bar_buttons_alignment    = C.buttonsAlignment;
    decoration_padding       = C.padding;
    bar_button_padding       = C.buttonPadding;
    on_double_click          = C.onDoubleClick;
    retro_shader             = C.retroShader;
    decoration_appicon_enabled = C.appIconEnabled;
    decoration_appicon_offset  = {C.appIconOffsetX, C.appIconOffsetY};
    decoration_render_above    = C.renderAbove;
    ninepatch_linear_filtering = C.linearFiltering;
    ninepatch_repeat           = C.repeat;
    bar_outline_color          = CHyprColor(C.outlineColor);
    bar_outline_width          = C.outlineWidth;
    bar_outline_always         = C.outlineAlways;

    // Per-side reserved space, resolved with the documented precedence:
    // explicit value, then the nine-patch padding marker, then its border size.
    // Resolved after the texture is parsed, below.

    const std::string NINEPATCHACTIVE   = resolveTexturePath(C.textureBase, {"_active", "_focused", "active", ""});
    const std::string NINEPATCHINACTIVE = resolveTexturePath(C.textureBase, {"_inactive", "_unfocused", "inactive"}, NINEPATCHACTIVE);

    if (!C.textureBase.empty() && NINEPATCHACTIVE.empty())
        notifyError(std::format("texture not found: \"{}\"", C.textureBase));

    // Refresh surfaces
    SNinePatchInfo newActiveNPI, newInactiveNPI;
    auto newActiveSurface = loadSurface(NINEPATCHACTIVE, &newActiveNPI);
    auto newInactiveSurface = (NINEPATCHINACTIVE == NINEPATCHACTIVE || NINEPATCHINACTIVE.empty()) ? nullptr : loadSurface(NINEPATCHINACTIVE, &newInactiveNPI);

    // Hard error: the file exists but is not a readable PNG.
    if (!NINEPATCHACTIVE.empty() && !newActiveSurface)
    {
        notifyError(std::format("failed to load texture: \"{}\"", NINEPATCHACTIVE));
    }
    else if (newActiveSurface && !newActiveNPI.defined)
    {
        Log::logger->log(Log::WARN, std::format("[HYPRDECOR] \"{}\" has no nine-patch metadata (1px marker border); stretched as a whole image", NINEPATCHACTIVE));
    }

    if (activeSurface)
        cairo_surface_destroy(activeSurface);
    if (inactiveSurface && inactiveSurface != activeSurface)
        cairo_surface_destroy(inactiveSurface);

    activeSurface = nullptr;
    inactiveSurface = nullptr;

    ninepatch_active = NINEPATCHACTIVE;
    ninepatch_inactive = NINEPATCHINACTIVE;
    ninepatch_texture = C.textureBase;
    activeSurface = newActiveSurface;
    inactiveSurface = newInactiveSurface ? newInactiveSurface : newActiveSurface;
    activeNinepatch = newActiveNPI;
    inactiveNinepatch = newInactiveSurface ? newInactiveNPI : newActiveNPI;

    // Resolved per-side reserved space, using the documented precedence:
    // explicit value -> nine-patch padding marker -> nine-patch border size.
    // Corner sizes (border) mark where the art stops stretching, which is the closest
    // thing to "how much the frame occupies" when padding markers are absent.
    static const SNinePatchInfo EMPTY_NPI;
    const SNinePatchInfo &RESOLVE = newActiveNPI.defined ? newActiveNPI : EMPTY_NPI;

    const auto SIDE = [&](double explicitValue, float marker, float borderPx) -> int
    {
        if (explicitValue >= 0)
            return (int)std::lround(explicitValue);
        if (marker > 0)
            return (int)std::lround(marker);
        return (int)std::lround(borderPx);
    };

    // border.scale magnifies the frame art, so the space it occupies grows with it.
    // Reserved space and rendering must use the same factor or the frame overlaps the
    // window (or leaves a gap).
    const int SC = std::max(1, C.artScale);

    decoration_offset_left   = SIDE(C.thickness.left, RESOLVE.padding[0], RESOLVE.border[0]) * SC;
    decoration_offset_top    = SIDE(C.thickness.top, RESOLVE.padding[1], RESOLVE.border[1]) * SC;
    decoration_offset_right  = SIDE(C.thickness.right, RESOLVE.padding[2], RESOLVE.border[2]) * SC;
    decoration_offset_bottom = SIDE(C.thickness.bottom, RESOLVE.padding[3], RESOLVE.border[3]) * SC;

    // inset is additional space on top of the art. It is NOT drawn as a gap: the
    // reserved box and the box the nine-patch stretches over are the same rectangle.
    //
    // Scaled here, not in frameMetrics: the reserved extents are built from inset_left..
    // (see currentExtents()), so both must use the SAME number or the frame overlaps the
    // window by exactly the amount one side forgot to scale.
    const auto INSET = [&](double v) { return v >= 0 ? (int)std::lround(v * SC) : 0; };
    inset_left   = INSET(C.inset.left);
    inset_top    = INSET(C.inset.top);
    inset_right  = INSET(C.inset.right);
    inset_bottom = INSET(C.inset.bottom);

    Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] resolved extents L{} T{} R{} B{} at border.scale={} (markers: padding {} {} {} {}, border {} {} {} {})",
                                             decoration_offset_left, decoration_offset_top, decoration_offset_right, decoration_offset_bottom, SC,
                                             (int)RESOLVE.padding[0], (int)RESOLVE.padding[1], (int)RESOLVE.padding[2], (int)RESOLVE.padding[3],
                                             (int)RESOLVE.border[0], (int)RESOLVE.border[1], (int)RESOLVE.border[2], (int)RESOLVE.border[3]));

    if (inset_left || inset_top || inset_right || inset_bottom)
        Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] inset L{} T{} R{} B{} (added to the art, reserved box grows; no gap is drawn)",
                                                 inset_left, inset_top, inset_right, inset_bottom));

    // Damage and update windows after all bars are invalidated
    for (auto bar : liveDecorators())
    {
        if (bar)
        {
            bar->invalidateTextures();
            g_pDecorationPositioner->repositionDeco(bar);

            if (auto pWindow = bar->getOwner())
            {
                pWindow->presentation().updateDecorations();
                g_pHyprRenderer->damageWindow(pWindow);
            }
            bar->damageEntire();
        }
    }

    loadAllTextures();
}
