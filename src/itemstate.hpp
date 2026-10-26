// Window-state predicates for `visible_when` (C4).
//
// Pure, no Hyprland types: the decorator gathers the five booleans and this decides
// whether an item may be shown. Keeping the rule here means the "hidden items keep their
// slot" behaviour is testable without a compositor.

#pragma once

#include <cstdint>
#include <optional>

namespace ItemState
{

// What an item declares it needs. An unset field is "don't care", which is different
// from "must be false": `visible_when = { pinned = true }` must not hide the item on
// every unpinned window in a config that only ever meant to test pinned.
struct SVisibleWhen
{
    std::optional<bool> focused;
    std::optional<bool> floating;
    std::optional<bool> maximised;
    std::optional<bool> fullscreen;
    std::optional<bool> pinned;

    bool any() const { return focused || floating || maximised || fullscreen || pinned; }
};

// The window's state, as observed at layout time.
struct SWindowState
{
    bool focused    = false;
    bool floating   = false;
    bool maximised  = false;
    bool fullscreen = false;
    bool pinned     = false;
};

// Every declared condition must hold. No conditions means always visible.
inline bool visible(const SVisibleWhen &when, const SWindowState &state)
{
    if (when.focused && *when.focused != state.focused)
        return false;
    if (when.floating && *when.floating != state.floating)
        return false;
    if (when.maximised && *when.maximised != state.maximised)
        return false;
    if (when.fullscreen && *when.fullscreen != state.fullscreen)
        return false;
    if (when.pinned && *when.pinned != state.pinned)
        return false;

    return true;
}

}
