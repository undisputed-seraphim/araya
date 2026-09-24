#pragma once

#include "tui_state.hpp"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <string_view>
#include <vector>

// The stored-session picker: a centered modal with a search line and the
// enumerated sessions. Selection restores via '/session restore <id>'.
namespace araya::tui {

// The sessions whose title or id contains the filter (case-insensitive).
std::vector<session_row> filter_sessions(std::vector<session_row> const& sessions, std::string_view filter);

ftxui::Element
render_picker(snapshot const& snap, std::string_view filter, int selected, int width, int height, bool ascii);

} // namespace araya::tui
