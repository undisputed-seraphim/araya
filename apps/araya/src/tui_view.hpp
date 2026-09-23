#pragma once

#include "tui_state.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <string>
#include <string_view>

// The TUI's presentation layer: the shared theme/glyph mapping, the
// phase root, and the two screen renderers. Pure view - it reads
// snapshots and calls the command/exit callbacks, and knows nothing
// about the engine.
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

// The startup screen: the wordmark, the prompt box, the tip line, and
// the bottom status bar.
ftxui::Element render_entry_screen(
	shared_state const& sh,
	tui_theme const& theme,
	ftxui::Component const& input,
	int width,
	int height);

// The session screen: the conversation feed, the command-output strip,
// the prompt box, and the status sidebar.
ftxui::Element render_session_screen(
	shared_state const& sh,
	tui_theme const& theme,
	ftxui::Component const& input,
	int width,
	int height);

// Builds the full UI: one input (shared by both phases) plus a root
// renderer that shows the entry screen until the first submit, then the
// session screen. A completed line goes to on_command (or on_exit for
// Ctrl+D and quit), and Ctrl+C is swallowed per TUI convention.
ftxui::Component build_ui(
	shared_state& sh,
	tui_theme const& theme,
	std::string& input_buffer,
	ftxui::ScreenInteractive& screen,
	std::function<void(std::string)> on_command,
	std::function<void()> on_exit);

} // namespace araya::tui
