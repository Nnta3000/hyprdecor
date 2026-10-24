// Small compatibility shims so the port to newer Hyprland API stays localized.
#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/render/OpenGL.hpp>

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

    // Split a comma separated config value, trimming whitespace (old Hyprland CVarList).
    inline std::vector<std::string> splitVars(const std::string& in) {
        std::vector<std::string> out;
        std::string              cur;
        for (const char c : in) {
            if (c == ',') {
                out.emplace_back(cur);
                cur.clear();
            } else
                cur += c;
        }
        out.emplace_back(cur);

        for (auto& v : out) {
            const auto START = v.find_first_not_of(" \t");
            const auto END   = v.find_last_not_of(" \t");
            v                = START == std::string::npos ? "" : v.substr(START, END - START + 1);
        }

        return out;
    }

    // Minimal replacement for the removed configStringToInt. Supports decimal, hex,
    // and rgba(rrggbbaa) / rgb(rrggbb) colors.
    inline std::optional<int64_t> configStringToInt(const std::string& in) {
        try {
            const auto COLOROPEN = in.find('(');
            if (COLOROPEN != std::string::npos && in.back() == ')') {
                const auto HEX = in.substr(COLOROPEN + 1, in.size() - COLOROPEN - 2);
                if (HEX.size() == 6 || HEX.size() == 8) {
                    const auto VALUE = std::stoull(HEX, nullptr, 16);
                    return (int64_t)(HEX.size() == 6 ? (VALUE << 8) | 0xFF : VALUE);
                }
                return std::nullopt;
            }

            if (in.starts_with("0x"))
                return std::stoll(in, nullptr, 16);

            return std::stoll(in, nullptr, 10);
        } catch (...) { return std::nullopt; }
    }
}
