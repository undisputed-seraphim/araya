#pragma once

#include "input_route.hpp"
#include "tui_state.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

// The TUI's presentation layer: the shared theme/glyph mapping, the
// transient overlay state, the phase root, and the two screen renderers.
// Pure view - it reads snapshots and calls the command/exit callbacks,
// and knows nothing about the engine.
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

// Which pane the keyboard drives: the input line or the conversation
// feed (Tab cycles).
enum class pane_focus : std::uint8_t {
	input,
	feed,
};

// The UI thread's transient overlay state, owned by the driver: the
// command palette's selection, the session picker's open/filter/
// selection, the focused pane, and the feed's scroll offset (rows from
// the bottom; the renderer clamps and records the bounds). The palette's
// open state is derived from the input text.
struct ui_state {
	int palette_selected = 0;
	bool picker_open = false;
	std::string picker_filter;
	int picker_selected = 0;
	pane_focus focus = pane_focus::input;
	int feed_scroll = 0;
	int feed_total_lines = 0;
	int feed_view_lines = 1;
	// Advanced once per frame while a turn streams; drives the spinner.
	int spinner = 0;
};

// Everything a screen renderer reads for one frame. `ui` is mutable: the
// feed renderer clamps and records the scroll bounds as it draws.
struct render_context {
	shared_state const& sh;
	tui_theme const& theme;
	ui_state& ui;
	std::span<araya::app::command_info const> commands;
	std::string_view input_text;
	ftxui::Component const& input;
	int width = 0;
	int height = 0;
};

// One mapping from component state to glyph + color, shared by every
// surface that renders a component.
state_style state_style_for(tui_theme const& theme, component_row const& c);

// The startup screen: the wordmark, the prompt box (with the palette
// above it when open), the tip line, and the bottom status bar.
ftxui::Element render_entry_screen(render_context const& rc);

// The session screen: the conversation feed, the command-output strip,
// the prompt box (with the palette above it when open), and the sidebar.
ftxui::Element render_session_screen(render_context const& rc);

// Builds the full UI: one input (shared by both phases) plus a root
// renderer that shows the entry screen until the first submit, then the
// session screen, with the session picker as a centered modal. A
// completed line goes to on_command (or on_exit for Ctrl+D and /quit),
// and Ctrl+C is swallowed per TUI convention.
ftxui::Component build_ui(
	shared_state& sh,
	tui_theme const& theme,
	ui_state& ui,
	std::string& input_buffer,
	ftxui::ScreenInteractive& screen,
	std::function<void(std::string)> on_command,
	std::function<void()> on_exit);

} // namespace araya::tui
