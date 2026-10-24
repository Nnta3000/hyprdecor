#pragma once

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <pango/pangocairo.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <format>
#include <libdrm/drm_fourcc.h>
#include "plugin.hpp"
#include "ninepatch.hpp"

// Nine-patch parsing and composition live in ninepatch.hpp so they can be unit
// tested without Hyprland (see tests/ninepatch_test.cpp). These wrappers only add
// the plugin-specific bits: logging and optional icon pre-scaling.

inline cairo_surface_t *loadSurface(std::string path, SNinePatchInfo *pInfo = nullptr, int targetSize = 0)
{
    if (path.empty())
    {
        Log::logger->log(Log::WARN, "[HYPRDECOR] loadSurface called with an empty path");
        return nullptr;
    }

    // Icon pre-scaling path: load raw, then resample to targetSize.
    if (!pInfo && targetSize > 0)
    {
        cairo_surface_t *raw = NinePatch::load(path, nullptr);
        if (!raw)
        {
            Log::logger->log(Log::ERR, std::format("[HYPRDECOR] failed to read \"{}\"", path));
            return nullptr;
        }

        const int w = cairo_image_surface_get_width(raw);
        const int h = cairo_image_surface_get_height(raw);

        cairo_surface_t *scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, targetSize, targetSize);
        cairo_t *cr = cairo_create(scaled);
        const double SCALE = (double)targetSize / std::max(w, h);
        cairo_scale(cr, SCALE, SCALE);
        cairo_set_source_surface(cr, raw, (targetSize / SCALE - w) / 2.0, (targetSize / SCALE - h) / 2.0);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(raw);
        return scaled;
    }

    cairo_surface_t *out = NinePatch::load(path, pInfo);

    if (!out)
        Log::logger->log(Log::ERR, std::format("[HYPRDECOR] cairo failed to load \"{}\"", path));
    else if (pInfo && !pInfo->defined)
        Log::logger->log(Log::WARN, std::format("[HYPRDECOR] \"{}\" has no nine-patch markers", path));

    return out;
}

inline void drawSizedSurface(cairo_t *cr, cairo_surface_t *surface, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh)
{
    NinePatch::drawSized(cr, surface, sx, sy, sw, sh, dx, dy, dw, dh);
}

inline void drawRepeatedSurface(cairo_t *cr, cairo_surface_t *surface, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh)
{
    NinePatch::drawRepeated(cr, surface, sx, sy, sw, sh, dx, dy, dw, dh);
}

// Uploads a cairo image surface into an OpenGL texture.
//
// Cairo ARGB32 is premultiplied BGRA in little-endian memory. Uploading it through
// CGLTexture::update(DRM_FORMAT_ARGB8888, ...) produced an empty (black) texture, so
// every texture in this plugin goes through glTexImage2D with an R/B swizzle instead.
// Keeping it in one place means the title, buttons, icons and nine-patch canvas can
// never diverge again.
inline void uploadCairoSurface(const SP<Render::ITexture> &tex, cairo_surface_t *surface, bool linear)
{
    if (!tex || !surface)
        return;

    cairo_surface_flush(surface);

    const auto DATA = cairo_image_surface_get_data(surface);
    const int W = cairo_image_surface_get_width(surface);
    const int H = cairo_image_surface_get_height(surface);

    tex->allocate(Vector2D{(double)W, (double)H});
    glBindTexture(GL_TEXTURE_2D, tex->m_texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
#ifndef GLES2
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
#endif
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, DATA);

    tex->m_size = {(double)W, (double)H};
}

inline void loadTexture(std::string path, SP<Render::ITexture> &out, bool linear = true, int targetSize = 0)
{
    if (path.empty() || out->m_texID != 0)
        return;

    const auto CAIROSURFACE = loadSurface(path, nullptr, targetSize);
    if (!CAIROSURFACE)
        return;

    uploadCairoSurface(out, CAIROSURFACE, linear);

    cairo_surface_destroy(CAIROSURFACE);
}

inline std::string findDesktopFile(const std::string &appId)
{
    std::vector<std::string> searchPaths;

    // Use XDG_DATA_HOME (defaults to ~/.local/share)
    const char *xdgDataHome = getenv("XDG_DATA_HOME");
    if (xdgDataHome && xdgDataHome[0] != '\0')
    {
        searchPaths.push_back(std::string(xdgDataHome) + "/applications/");
    }
    else
    {
        const char *home = getenv("HOME");
        if (home)
            searchPaths.push_back(std::string(home) + "/.local/share/applications/");
    }

    // Use XDG_DATA_DIRS (defaults to /usr/local/share:/usr/share)
    const char *xdgDataDirs = getenv("XDG_DATA_DIRS");
    std::string dataDirs = xdgDataDirs ? xdgDataDirs : "/usr/local/share:/usr/share";

    std::stringstream ss(dataDirs);
    std::string dir;
    while (std::getline(ss, dir, ':'))
    {
        if (!dir.empty())
            searchPaths.push_back(dir + "/applications/");
    }

    // Also add Flatpak paths
    const char *home = getenv("HOME");
    if (home)
    {
        searchPaths.push_back(std::string(home) + "/.local/share/flatpak/exports/share/applications/");
    }
    searchPaths.push_back("/var/lib/flatpak/exports/share/applications/");

    // Search for desktop file
    for (const auto &path : searchPaths)
    {
        if (!std::filesystem::exists(path))
            continue;

        std::string desktop = path + appId + ".desktop";
        if (std::filesystem::exists(desktop))
            return desktop;
    }

    return "";
}

inline std::string getIconFromDesktop(const std::string &desktopPath)
{
    std::ifstream file(desktopPath);
    if (!file.is_open())
        return "";

    std::string line;
    bool inDesktopEntry = false;

    while (std::getline(file, line))
    {
        // Check if we're in the [Desktop Entry] section
        if (line == "[Desktop Entry]")
            inDesktopEntry = true;
        else if (!line.empty() && line[0] == '[')
            inDesktopEntry = false;

        if (inDesktopEntry && line.find("Icon=") == 0)
        {
            return line.substr(5);
        }
    }
    return "";
}

inline std::string resolveIconPath(const std::string &iconName)
{
    if (iconName.empty())
        return "";

    // If already an absolute path
    if (iconName[0] == '/' && std::filesystem::exists(iconName))
        return iconName;

    std::vector<std::string> iconPaths;
    const char *home = getenv("HOME");

    // Common icon sizes to search (in order of preference)
    std::vector<std::string> sizes = {"48x48", "64x64", "128x128", "scalable"};

    // Common icon themes (hicolor is the fallback theme per freedesktop spec)
    std::vector<std::string> themes = {"hicolor", "breeze", "oxygen", "Adwaita", "Papirus"};

    // Get XDG data directories for icons
    std::vector<std::string> dataDirs;

    // XDG_DATA_HOME
    const char *xdgDataHome = getenv("XDG_DATA_HOME");
    if (xdgDataHome && xdgDataHome[0] != '\0')
    {
        dataDirs.push_back(std::string(xdgDataHome));
    }
    else if (home)
    {
        dataDirs.push_back(std::string(home) + "/.local/share");
    }

    // XDG_DATA_DIRS
    const char *xdgDataDirs = getenv("XDG_DATA_DIRS");
    std::string dataDir = xdgDataDirs ? xdgDataDirs : "/usr/local/share:/usr/share";

    std::stringstream ss(dataDir);
    std::string dir;
    while (std::getline(ss, dir, ':'))
    {
        if (!dir.empty())
        {
            dataDirs.push_back(dir);
        }
    }

    // Search in icon themes
    for (const auto &baseDir : dataDirs)
    {
        for (const auto &theme : themes)
        {
            for (const auto &size : sizes)
            {
                // Standard structure: icons/{theme}/{size}/apps/{icon}
                iconPaths.push_back(baseDir + "/icons/" + theme + "/" + size + "/apps/" + iconName + ".png");
                // Oxygen structure: icons/{theme}/base/{size}/apps/{icon}
                iconPaths.push_back(baseDir + "/icons/" + theme + "/base/" + size + "/apps/" + iconName + ".png");
                // Breeze structure: icons/{theme}/apps/{size}/{icon} (size without 'x')
                std::string sizeNum = size;
                if (size.find("x") != std::string::npos)
                    sizeNum = size.substr(0, size.find("x")); // "48x48" -> "48"
                iconPaths.push_back(baseDir + "/icons/" + theme + "/apps/" + sizeNum + "/" + iconName + ".png");
            }
        }
        // Also check pixmaps as fallback
        iconPaths.push_back(baseDir + "/pixmaps/" + iconName + ".png");
        iconPaths.push_back(baseDir + "/pixmaps/" + iconName);
    }

    for (const auto &path : iconPaths)
    {
        if (std::filesystem::exists(path) && path.ends_with(".png"))
        {
            return path;
        }
    }

    return "";
}

inline void loadAppIcon(const std::string &appId, SP<Render::ITexture> &out, int targetSize)
{
    if (appId.empty())
        return;

    auto desktopFile = findDesktopFile(appId);
    if (desktopFile.empty())
        return;

    auto iconName = getIconFromDesktop(desktopFile);
    if (iconName.empty())
        return;

    auto iconPath = resolveIconPath(iconName);
    if (iconPath.empty() || !iconPath.ends_with(".png"))
        return;

    loadTexture(iconPath, out, true, targetSize);
}
