#pragma once

#include "tui_state.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <string>
#include <string_view>

// The TUI's presentation layer: every renderer, the glyph tier, and the
// layout. Pure view - it reads snapshots and calls the command/exit
// callbacks, and knows nothing about the engine.
namespace araya::tui {

struct tui_theme {
	// The ASCII tier (* ~ x o) for terminals whose fonts misrender the
	// unicode shapes.
	bool ascii = false;
};

struct state_style {
	std::string_view glyph;
	ftxui::Color color;
};

// One mapping from component state to glyph + color, shared by every
// surface that renders a component.
state_style state_style_for(tui_theme const& theme, component_row const& c);

// Builds the full UI (main column: feed, prompt box, hint line; and the
// sidebar). The input buffer is owned by the caller; a completed line
// goes to on_command (or on_exit for Ctrl+D and quit), and Ctrl+C is
// swallowed per TUI convention.
ftxui::Component build_ui(
	shared_state& sh,
	tui_theme const& theme,
	std::string& input_buffer,
	ftxui::ScreenInteractive& screen,
	std::function<void(std::string)> on_command,
	std::function<void()> on_exit);

} // namespace araya::tui
