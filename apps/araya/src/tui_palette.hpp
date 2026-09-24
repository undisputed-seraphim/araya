#pragma once

#include "input_route.hpp"

#include <ftxui/dom/elements.hpp>

#include <span>
#include <string_view>

// The '/' command palette: a bordered two-column list (name, summary)
// rendered above the prompt box, width-matched to it, with the selected
// row highlighted. Pure view over the command metadata + query.
namespace araya::tui {

ftxui::Element render_palette(
	std::span<araya::app::command_info const> commands,
	std::string_view query,
	int selected,
	int width,
	bool ascii);

} // namespace araya::tui
