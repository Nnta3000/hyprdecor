#include "plugin.hpp"
#include "compat.hpp"
#include "hyprWindowDecorator.hpp"
#include "util.hpp"
#include <hyprland/src/config/ConfigManager.hpp>
#include <filesystem>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>

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

    for (const auto &deco : pWindow->m_windowDecorations)
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

Hyprlang::CParseResult onNewButtonTextured(const char *K, const char *V)
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
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:bar_color", Hyprlang::INT{*Compat::configStringToInt("rgba(33333388)")});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_offset_top", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:col.text", Hyprlang::INT{*Compat::configStringToInt("rgba(ffffffff)")});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_title_size", Hyprlang::INT{10});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_title_enabled", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:bar_blur", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:bar_text_font", Hyprlang::STRING{"Sans"});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_title_align", Hyprlang::FLOAT{0.5});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_title_placement", Hyprlang::STRING{"top"});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:bar_part_of_window", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:bar_precedence_over_border", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:bar_buttons_alignment", Hyprlang::STRING{"right"});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_padding", Hyprlang::INT{7});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:bar_button_padding", Hyprlang::INT{5});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:enabled", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:on_double_click", Hyprlang::STRING{""});

    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:ninepatch_texture", Hyprlang::STRING{""});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:ninepatch_active", Hyprlang::STRING{""});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:ninepatch_inactive", Hyprlang::STRING{""});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:ninepatch_middle_alpha", Hyprlang::FLOAT{0.0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:ninepatch_repeat", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:ninepatch_linear_filtering", Hyprlang::INT{1});

    // Plain solid outline around the window, drawn on top of (or instead of) the nine-patch.
    // Handy as a placeholder while there is no texture yet.
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:bar_outline_color", Hyprlang::INT{*(Compat::configStringToInt("rgba(ffffffff)"))});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:bar_outline_width", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:bar_outline_always", Hyprlang::INT{0});

    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_inset", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_offset_left", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_offset_right", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_offset_bottom", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_appicon_enabled", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_render_above", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(m_pHandle, "plugin:hyprdecor:decoration_appicon_offset", Hyprlang::VEC2{0, 0});

    HyprlandAPI::addConfigKeyword(m_pHandle, "plugin:hyprdecor:hyprdecor-button", onNewButtonTextured, Hyprlang::SHandlerOptions{});
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
template <typename T>
static T *const *pluginConfigPtr(HANDLE handle, const char *name)
{
    const auto VALUE = HyprlandAPI::getConfigValue(handle, name);
    if (!VALUE)
        return nullptr;
    return (T *const *)VALUE->getDataStaticPtr();
}

// Dereference a plugin STRING config pointer safely.
static std::string pluginConfigString(const Hyprlang::STRING *const *ptr)
{
    return (ptr && *ptr) ? std::string(**ptr) : std::string();
}

// Safe reads: legacy addConfigValue() registrations are not committed by the Lua
// config manager, so getDataStaticPtr() can be null. Dereferencing it unguarded
// crashed the plugin; these fall back to the built-in default instead.
static int64_t   cfgInt(Hyprlang::INT *const *p, int64_t fallback) { return (p && *p) ? (int64_t)**p : fallback; }
static float     cfgFloat(Hyprlang::FLOAT *const *p, float fallback) { return (p && *p) ? (float)**p : fallback; }
static std::string cfgString(Hyprlang::STRING *const *p, const char *fallback) { return (p && *p && **p) ? std::string(**p) : std::string(fallback); }

void CPlugin::notifyError(const std::string &message)
{
    Log::logger->log(Log::ERR, std::format("[HYPRDECOR] {}", message));

    HyprlandAPI::addNotificationV2(m_pHandle,
                                   {
                                       {"text", std::string(message)},
                                       {"time", (uint64_t)10000},
                                       {"color", CHyprColor(1.0, 0.2, 0.2, 1.0)},
                                   });
}

void CPlugin::requestUpdate()
{
    // Mark dirty and let the next main-loop tick run update(). Calling update()
    // re-entrantly from a Lua callback would drive layout and damage while the
    // config manager is still evaluating.
    m_bUpdatePending = true;
    Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] update requested (this={})", (void *)this));
}

void CPlugin::requestRelayout()
{
    m_bRelayoutPending = true;
}

void CPlugin::processPendingUpdate()
{
    // Runs before the update check: relayout may be queued from a render pass.
    if (m_bRelayoutPending)
    {
        m_bRelayoutPending = false;
        Log::logger->log(Log::DEBUG, "[HYPRDECOR] performing deferred relayout");
        for (auto bar : liveDecorators())
        {
            if (auto win = bar->getOwner())
                g_layoutManager->recalculateMonitor(win->m_monitor.lock());
        }
    }

    if (!m_bUpdatePending)
        return;

    m_bUpdatePending = false;

    // The config manager may not have committed this plugin's values yet. Give it a
    // few ticks, then run regardless: update() itself tolerates missing values, and
    // retrying forever meant it never ran at all (the config never became "ready"
    // because nothing was reading it).
    if (!configReady())
    {
        if (++m_iUpdateRetries < 60)
        {
            m_bUpdatePending = true;
            return;
        }

        Log::logger->log(Log::WARN, "[HYPRDECOR] config values still unavailable after retrying; running update anyway");
    }

    m_iUpdateRetries = 0;

    Log::logger->log(Log::DEBUG, std::format("[HYPRDECOR] running deferred update (this={})", (void *)this));
    update();
}

bool CPlugin::configReady() const
{
    // enabled is registered before any other value; if it is readable, the whole
    // config block has been committed and it is safe to read the rest.
    const auto VALUE = HyprlandAPI::getConfigValue(m_pHandle, "plugin:hyprdecor:enabled");
    return VALUE && VALUE->getDataStaticPtr() != nullptr;
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
    out += std::format("  middle alpha:      {}\n", ninepatch_middle_alpha);
    out += std::format("  enabled:           {}\n", enabled ? "yes" : "no");
    out += std::format("  decorated windows: {}\n", liveDecorators().size());
    out += std::format("  canvas (last):     {}x{}\n", lastBarW, lastBarH);
    out += std::format("  drawn (last):      {}\n", lastDrawn ? "yes" : "no");

    return out;
}

void CPlugin::update()
{
    Log::logger->log(Log::DEBUG, "[HYPRDECOR] update() entered");
    auto *const PENABLED = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:enabled");
    // The legacy addConfigValue() registrations are not committed by the Lua config
    // manager, so these pointers can stay null forever. That must not abort the whole
    // update: setup() supplies its own values and is authoritative.
    if (!PENABLED || !*PENABLED)
        Log::logger->log(Log::WARN, "[HYPRDECOR] update(): config values unavailable (legacy registration); using built-in defaults and setup() values");
    else
        enabled = cfgInt(PENABLED, 1);

    if (!enabled)
    {
        Log::logger->log(Log::DEBUG, "[HYPRDECOR] update(): disabled");
        return;
    }

    auto *const PBARCOLOR = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:bar_color");
    auto *const PHEIGHT = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:decoration_offset_top");
    auto *const PTEXTCOL = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:col.text");
    auto *const PTEXTSIZE = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:decoration_title_size");
    auto *const PTITLEENABLED = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:decoration_title_enabled");
    auto *const PBARBLUR = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:bar_blur");
    auto *const PTEXTFONT = pluginConfigPtr<Hyprlang::STRING>(m_pHandle, "plugin:hyprdecor:bar_text_font");
    auto *const PTEXTALIGN = pluginConfigPtr<Hyprlang::FLOAT>(m_pHandle, "plugin:hyprdecor:decoration_title_align");
    auto *const PTEXTPLACE = pluginConfigPtr<Hyprlang::STRING>(m_pHandle, "plugin:hyprdecor:decoration_title_placement");
    auto *const PPARTOW = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:bar_part_of_window");
    auto *const PPRECEDENCE = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:bar_precedence_over_border");
    auto *const PALIGNBUTTONS = pluginConfigPtr<Hyprlang::STRING>(m_pHandle, "plugin:hyprdecor:bar_buttons_alignment");
    auto *const PPADDING = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:decoration_padding");
    auto *const PBUTPADDING = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:bar_button_padding");
    auto *const PONDOUBLECLICK = pluginConfigPtr<Hyprlang::STRING>(m_pHandle, "plugin:hyprdecor:on_double_click");

    auto *const PTEXTURE = pluginConfigPtr<Hyprlang::STRING>(m_pHandle, "plugin:hyprdecor:ninepatch_texture");
    auto *const PTEXACTIVE = pluginConfigPtr<Hyprlang::STRING>(m_pHandle, "plugin:hyprdecor:ninepatch_active");
    auto *const PTEXINACTIVE = pluginConfigPtr<Hyprlang::STRING>(m_pHandle, "plugin:hyprdecor:ninepatch_inactive");
    auto *const PMIDDLEALPHA = pluginConfigPtr<Hyprlang::FLOAT>(m_pHandle, "plugin:hyprdecor:ninepatch_middle_alpha");
    auto *const PREPEAT = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:ninepatch_repeat");
    auto *const PINSET = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:decoration_inset");
    auto *const PLEFTWIDTH = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:decoration_offset_left");
    auto *const PRIGHTWIDTH = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:decoration_offset_right");
    auto *const PBOTTOMHT = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:decoration_offset_bottom");
    auto *const PLINEAR = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:ninepatch_linear_filtering");
    auto *const PSHOWAPPICON = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:decoration_appicon_enabled");
    auto *const PBARABOVE = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:decoration_render_above");
    auto *const PAPPICONOFFSET = pluginConfigPtr<Hyprlang::VEC2>(m_pHandle, "plugin:hyprdecor:decoration_appicon_offset");
    auto *const POUTLINECOLOR = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:bar_outline_color");
    auto *const POUTLINEWIDTH = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:bar_outline_width");
    auto *const POUTLINEALWAYS = pluginConfigPtr<Hyprlang::INT>(m_pHandle, "plugin:hyprdecor:bar_outline_always");

    bar_color = CHyprColor(cfgInt(PBARCOLOR, 0));
    decoration_offset_top = cfgInt(PHEIGHT, 0);
    col_text = CHyprColor(cfgInt(PTEXTCOL, 0));
    decoration_title_size = cfgInt(PTEXTSIZE, 10);
    decoration_title_enabled = cfgInt(PTITLEENABLED, 1);
    bar_blur = cfgInt(PBARBLUR, 0);
    bar_text_font = pluginConfigString(PTEXTFONT);
    decoration_title_align = cfgFloat(PTEXTALIGN, 0.5F);
    decoration_title_placement = pluginConfigString(PTEXTPLACE);
    bar_part_of_window = cfgInt(PPARTOW, 1);
    bar_precedence_over_border = cfgInt(PPRECEDENCE, 0);
    bar_buttons_alignment = pluginConfigString(PALIGNBUTTONS);
    decoration_padding = cfgInt(PPADDING, 7);
    bar_button_padding = cfgInt(PBUTPADDING, 5);
    enabled = cfgInt(PENABLED, 1);
    on_double_click = pluginConfigString(PONDOUBLECLICK);

    const std::string PTEXTURE_STR = pluginConfigString(PTEXTURE);

    // setup() is the structured API and wins over the flat config values.
    // ninepatch_texture is a pointer into the Lua string, so it must be copied
    // here (Lua may collect it after setup() returns).
    const std::string SETUP_TEXTURE = setup_applied ? ninepatch_texture : std::string();
    const std::string PTEXACT = setup_applied ? std::string() : pluginConfigString(PTEXACTIVE);
    const std::string PTEXINACT = setup_applied ? std::string() : pluginConfigString(PTEXINACTIVE);

    std::string NINEPATCHACTIVE = PTEXACT;
    std::string NINEPATCHINACTIVE = !PTEXINACT.empty() ? PTEXINACT : NINEPATCHACTIVE;

    const std::string BASE = !SETUP_TEXTURE.empty() ? SETUP_TEXTURE : PTEXTURE_STR;

    if (!BASE.empty())
    {
        if (NINEPATCHACTIVE.empty())
            NINEPATCHACTIVE = resolveTexturePath(BASE, {"_active", "_focused", "active", ""});
        if (NINEPATCHINACTIVE.empty() || NINEPATCHINACTIVE == NINEPATCHACTIVE)
            NINEPATCHINACTIVE = resolveTexturePath(BASE, {"_inactive", "_unfocused", "inactive"}, NINEPATCHACTIVE);

        // Hard error: a texture was configured but nothing could be resolved.
        // Falling back to a plain bar here is what made misconfiguration silent.
        if (NINEPATCHACTIVE.empty())
        {
            notifyError(std::format("texture not found: \"{}\" (tried {}_active.png, {}_focused.png, {}active.png, {}.png)",
                                    BASE, BASE, BASE, BASE, BASE));
        }
    }

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
    ninepatch_texture = BASE;
    activeSurface = newActiveSurface;
    inactiveSurface = newInactiveSurface ? newInactiveSurface : newActiveSurface;
    activeNinepatch = newActiveNPI;
    inactiveNinepatch = newInactiveSurface ? newInactiveNPI : newActiveNPI;

    ninepatch_middle_alpha = cfgFloat(PMIDDLEALPHA, 0.F);
    decoration_inset = cfgInt(PINSET, 0);
    decoration_offset_left = cfgInt(PLEFTWIDTH, 0);
    decoration_offset_right = cfgInt(PRIGHTWIDTH, 0);
    decoration_offset_bottom = cfgInt(PBOTTOMHT, 0);
    ninepatch_linear_filtering = cfgInt(PLINEAR, 1);
    ninepatch_repeat = cfgInt(PREPEAT, 0);
    decoration_appicon_enabled = cfgInt(PSHOWAPPICON, 0);
    decoration_render_above = cfgInt(PBARABOVE, 0);
    decoration_appicon_offset = (PAPPICONOFFSET && *PAPPICONOFFSET) ? Vector2D((*PAPPICONOFFSET)->x, (*PAPPICONOFFSET)->y) : Vector2D(0, 0);

    // setup() border overrides: one thickness for all four sides.
    if (setup_applied)
    {
        if (setup_border_thickness >= 0)
            decoration_offset_top = decoration_offset_left = decoration_offset_right = decoration_offset_bottom = setup_border_thickness;

        if (setup_border_alpha >= 0.F)
            ninepatch_middle_alpha = setup_border_alpha;
    }
    bar_outline_color = POUTLINECOLOR ? CHyprColor(cfgInt(POUTLINECOLOR, 0)) : CHyprColor(1, 1, 1, 1);
    bar_outline_width = cfgInt(POUTLINEWIDTH, 0);
    bar_outline_always = POUTLINEALWAYS ? cfgInt(POUTLINEALWAYS, 0) : 1;

    // Damage and update windows after all bars are invalidated
    for (auto bar : liveDecorators())
    {
        if (bar)
        {
            bar->invalidateTextures();
            g_pDecorationPositioner->repositionDeco(bar);

            if (auto pWindow = bar->getOwner())
            {
                pWindow->updateWindowDecos();
                g_pHyprRenderer->damageWindow(pWindow);
            }
            bar->damageEntire();
        }
    }

    loadAllTextures();
}
