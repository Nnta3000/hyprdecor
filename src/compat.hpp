// Small compatibility shims so the port to newer Hyprland API stays localized.
#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/render/OpenGL.hpp>

#include "colorparse.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Compat {
    // Newer Hyprland removed the global window list from CCompositor.
    inline const std::vector<PHLWINDOW>& windows() {
        return Desktop::windowState()->windows();
    }

    inline const std::vector<PHLMONITOR>& monitors() {
        return State::monitorState()->monitors();
    }

    inline PHLMONITOR monitorFromName(const std::string& name) {
        if (name.empty())
            return nullptr;
        for (const auto& m : monitors()) {
            if (m && m->m_name == name)
                return m;
        }
        return nullptr;
    }

    /**
     * Parses a colour or integer config value into 0xAARRGGBB (see ColorParse::parse for
     * the accepted forms). Returns nullopt for malformed input; callers must report it.
     */
    inline std::optional<int64_t> configStringToInt(const std::string& in) {
        return ColorParse::parse(in);
    }
}
