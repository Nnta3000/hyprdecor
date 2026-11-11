#include "plugin.hpp"
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include "compat.hpp"
#include "hyprWindowDecorator.hpp"
#include "util.hpp"
#include "hyprdecor_version.hpp"
#include <hyprland/src/config/ConfigManager.hpp>
#include <filesystem>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>

namespace {
// Frame state suffixes, tried in order. One list for every frame resolution so a new
// spelling cannot be added to one call site and forgotten in another.
const std::vector<std::string> FRAME_SUFFIX_ACTIVE   = {"_active", "_focused", "active", ""};
const std::vector<std::string> FRAME_SUFFIX_INACTIVE = {"_inactive", "_unfocused", "inactive"};
} // namespace

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
                // Nine-patch elements need the markers, so go through the shared loader,
                // which parses them and uploads the cropped image.
                loadNinePatchTexture(element.cfg.path, element.tex, element.npi, LINEAR);
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
                    loadNinePatchTexture(VARIANT, slot.tex, slot.npi, LINEAR);
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

    // Title backdrop, loaded when its path OR its filtering changes. It has an active
    // _unfocused pair, like the frame: the swap follows window focus, not the pointer,
    // so it needs no hover/pressed states. The cache key includes `linearFiltering`
    // because the filter is applied when the texture is uploaded; keying on the path
    // alone silently kept the old resampling after a reload.
    if (cfg.titleBgEnabled && !cfg.titleBgPath.empty() &&
        (m_szLoadedTitleBg != cfg.titleBgPath || m_bLoadedTitleBgLinear != cfg.linearFiltering))
    {
        m_pTitleBgTex          = makeShared<Render::GL::CGLTexture>();
        m_pTitleBgTexUnfocused = makeShared<Render::GL::CGLTexture>();
        m_titleBgNPI           = SNinePatchInfo{};

        if (loadNinePatchTexture(cfg.titleBgPath, m_pTitleBgTex, m_titleBgNPI, cfg.linearFiltering))
            m_titleBgSize = m_pTitleBgTex->m_size;

        // The variant must keep the active geometry, so it is a swap of pixels only.
        const std::string VARIANT = statePath(cfg.titleBgPath, "_unfocused");
        if (VARIANT != cfg.titleBgPath)
        {
            SNinePatchInfo variantNPI;
            loadNinePatchTexture(VARIANT, m_pTitleBgTexUnfocused, variantNPI, cfg.linearFiltering);
        }

        if (m_pTitleBgTexUnfocused->m_texID == 0)
            m_pTitleBgTexUnfocused = m_pTitleBgTex;

        m_szLoadedTitleBg       = cfg.titleBgPath;
        m_bLoadedTitleBgLinear  = cfg.linearFiltering;
    }


}

CPlugin::CPlugin(HANDLE handle)
{
    m_pHandle = handle;
    // No addConfigValue() registrations: the Lua config manager never commits them, so
    // every read needed a null check and a fallback. Configuration lives in SConfig and
    // is filled by hl.plugin.hyprdecor.setup().
}

static void destroyFrame(SFrame &F)
{
    if (F.activeSurface)
        cairo_surface_destroy(F.activeSurface);
    if (F.inactiveSurface && F.inactiveSurface != F.activeSurface)
        cairo_surface_destroy(F.inactiveSurface);
    F.activeSurface   = nullptr;
    F.inactiveSurface = nullptr;
}

CPlugin::~CPlugin()
{
    destroyFrame(frame);
    for (auto &[name, F] : themes)
        destroyFrame(F);
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
    out += std::format("  version:           {} ({} built {})\n", HYPRDECOR_VERSION, HYPRDECOR_GIT_COMMIT, HYPRDECOR_BUILD_DATE);
    out += std::format("  loaded:            yes\n");
    out += std::format("  setup() applied:   {}\n", setup_applied ? "yes" : "no");
    out += std::format("  texture (base):    {}\n", frame.ninepatch_texture.empty() ? "(none)" : frame.ninepatch_texture);
    out += std::format("  texture (active):  {}\n", frame.ninepatch_active.empty() ? "(none)" : frame.ninepatch_active);
    out += std::format("  texture (inactive): {}\n", frame.ninepatch_inactive.empty() ? "(none)" : frame.ninepatch_inactive);
    out += std::format("  surface loaded:    {}\n", frame.activeSurface ? "yes" : "NO");
    out += std::format("  ninepatch parsed:  {}\n", frame.activeNinepatch.defined ? "yes" : "NO");
    out += std::format("  border L/T/R/B:    {} {} {} {}\n",
                       frame.activeNinepatch.border[0], frame.activeNinepatch.border[1], frame.activeNinepatch.border[2], frame.activeNinepatch.border[3]);
    out += std::format("  padding L/T/R/B:   {} {} {} {}\n",
                       frame.activeNinepatch.padding[0], frame.activeNinepatch.padding[1], frame.activeNinepatch.padding[2], frame.activeNinepatch.padding[3]);
    out += std::format("  render fallback:   {}\n", frame.activeSurface ? "texture" : "solid bar_color (NO TEXTURE)");
    out += std::format("  offsets L/T/R/B:   {} {} {} {}\n",
                       frame.decoration_offset_left, frame.decoration_offset_top, frame.decoration_offset_right, frame.decoration_offset_bottom);
    out += std::format("  enabled:           {}\n", cfg.enabled ? "yes" : "no");
    std::string themeList;
    for (const auto &[name, F] : themes)
    {
        if (!themeList.empty())
            themeList += ", ";
        themeList += name;
    }
    out += std::format("  themes:            {}\n", themes.empty() ? "(none)" : themeList);
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

SFrame CPlugin::resolveFrame(const SConfig &C, const STheme *theme)
{
    SFrame F;

    const std::string TEXTURE = theme && theme->textureBase ? *theme->textureBase : C.textureBase;
    const int         SC      = std::max(1, theme && theme->artScale ? *theme->artScale : C.artScale);
    const bool        LINEAR  = theme && theme->linearFiltering ? *theme->linearFiltering : C.linearFiltering;
    const bool        REPEAT  = theme && theme->repeat ? *theme->repeat : C.repeat;

    const SFrameThickness TH = theme ? Sides::over(theme->thickness, C.thickness) : C.thickness;
    const SFrameThickness IN = theme ? Sides::over(theme->inset, C.inset) : C.inset;

    const std::string ACTIVE   = resolveTexturePath(TEXTURE, FRAME_SUFFIX_ACTIVE);
    const std::string INACTIVE = resolveTexturePath(TEXTURE, FRAME_SUFFIX_INACTIVE, ACTIVE);

    if (!TEXTURE.empty() && ACTIVE.empty())
        notifyError(std::format("texture not found: \"{}\"", TEXTURE));

    SNinePatchInfo newActiveNPI, newInactiveNPI;
    auto newActiveSurface = loadSurface(ACTIVE, &newActiveNPI);
    auto newInactiveSurface = (INACTIVE == ACTIVE || INACTIVE.empty()) ? nullptr : loadSurface(INACTIVE, &newInactiveNPI);

    if (!ACTIVE.empty() && !newActiveSurface)
        notifyError(std::format("failed to load texture: \"{}\"", ACTIVE));
    else if (newActiveSurface && !newActiveNPI.defined)
        Log::logger->log(Log::WARN, std::format("[HYPRDECOR] \"{}\" has no nine-patch metadata (1px marker border); stretched as a whole image", ACTIVE));

    F.activeSurface   = newActiveSurface;
    F.inactiveSurface = newInactiveSurface ? newInactiveSurface : newActiveSurface;
    F.activeNinepatch   = newActiveNPI;
    F.inactiveNinepatch = newInactiveSurface ? newInactiveNPI : newActiveNPI;
    F.ninepatch_texture  = TEXTURE;
    F.ninepatch_active   = ACTIVE;
    F.ninepatch_inactive = INACTIVE;
    F.artScale           = SC;
    F.linearFiltering    = LINEAR;
    F.repeat             = REPEAT;

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

    F.decoration_offset_left   = SIDE(TH.left, RESOLVE.padding[0], RESOLVE.border[0]) * SC;
    F.decoration_offset_top    = SIDE(TH.top, RESOLVE.padding[1], RESOLVE.border[1]) * SC;
    F.decoration_offset_right  = SIDE(TH.right, RESOLVE.padding[2], RESOLVE.border[2]) * SC;
    F.decoration_offset_bottom = SIDE(TH.bottom, RESOLVE.padding[3], RESOLVE.border[3]) * SC;

    const auto INSET = [&](double v) { return v >= 0 ? (int)std::lround(v * SC) : 0; };
    F.inset_left   = INSET(IN.left);
    F.inset_top    = INSET(IN.top);
    F.inset_right  = INSET(IN.right);
    F.inset_bottom = INSET(IN.bottom);

    Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] resolved frame \"{}\" extents L{} T{} R{} B{} at scale={} (markers: padding {} {} {} {}, border {} {} {} {})",
                                             TEXTURE, F.decoration_offset_left, F.decoration_offset_top, F.decoration_offset_right, F.decoration_offset_bottom, SC,
                                             (int)RESOLVE.padding[0], (int)RESOLVE.padding[1], (int)RESOLVE.padding[2], (int)RESOLVE.padding[3],
                                             (int)RESOLVE.border[0], (int)RESOLVE.border[1], (int)RESOLVE.border[2], (int)RESOLVE.border[3]));

    if (F.inset_left || F.inset_top || F.inset_right || F.inset_bottom)
        Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] inset L{} T{} R{} B{} (added to the art, reserved box grows; no gap is drawn)",
                                                 F.inset_left, F.inset_top, F.inset_right, F.inset_bottom));

    return F;
}

const SFrame &CPlugin::frameFor(const std::string &name) const
{
    if (name.empty())
        return frame;
    const auto IT = themes.find(name);
    return IT == themes.end() ? frame : IT->second;
}

void CPlugin::update()
{
    // All configuration now comes from gPlugin->cfg, filled by setup(). There are no
    // nullable config pointers left, so no fallback dance and no uninitialised state.
    const SConfig &C = cfg;



    // Build the default frame and every named theme from the same resolver.
    destroyFrame(frame);
    for (auto &[name, F] : themes)
        destroyFrame(F);
    themes.clear();

    frame = resolveFrame(C, nullptr);
    for (const auto &[name, theme] : C.themes)
        themes.emplace(name, resolveFrame(C, &theme));

    // Damage and update windows after all bars are invalidated
    for (auto bar : liveDecorators())
    {
        if (bar)
        {
            bar->updateRules();
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
