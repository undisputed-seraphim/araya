#include "tui_view.hpp"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>

#include <string>
#include <utility>

namespace araya::tui {

state_style state_style_for(tui_theme const& theme, component_row const& c) {
	if (c.state == "active")
		return {theme.ascii ? "*" : "●", ftxui::Color::Green};
	if (c.state == "loading" || c.state == "unloading")
		return {theme.ascii ? "~" : "◐", ftxui::Color::Yellow};
	if (c.state == "inactive" && !c.error.empty())
		return {theme.ascii ? "x" : "✗", ftxui::Color::Red};
	// Retired or never mounted: dim, not a failure.
	return {theme.ascii ? "o" : "○", ftxui::Color::GrayDark};
}

ftxui::Component build_ui(
	shared_state& sh,
	tui_theme const& theme,
	std::string& input_buffer,
	ftxui::ScreenInteractive& screen,
	std::function<void(std::string)> on_command,
	std::function<void()> on_exit) {
	using namespace ftxui;

	InputOption input_options;
	input_options.placeholder = "ask anything...";
	Component input = Input(&input_buffer, input_options);
	// on_command/on_exit are build_ui parameters: capture them by value -
	// the component outlives this frame, and a [&] capture of them is a
	// dangling reference (the freed slots get reused at -O2).
	input |= CatchEvent([&, on_command = std::move(on_command), on_exit = std::move(on_exit)](Event event) {
		if (event == Event::CtrlD) {
			on_exit();
			return true;
		}
		if (event == Event::CtrlC)
			return true; // the TUI convention: Ctrl+D exits, Ctrl+C does nothing
		if (event != Event::Return)
			return false;
		auto cmd = input_buffer;
		input_buffer.clear();
		// The first submit leaves the entry phase immediately, without
		// waiting for the engine's next snapshot publish.
		sh.started_ui.store(true, std::memory_order_relaxed);
		if (cmd == "quit") {
			on_exit();
			return true;
		}
		if (!cmd.empty())
			on_command(std::move(cmd));
		screen.PostEvent(Event::Custom);
		return true;
	});

	// One input, two phases: the root renders the entry screen until the
	// engine (or the UI's optimistic flag) says the conversation began.
	auto root = Renderer(input, [&, input] {
		bool started = sh.started_ui.load(std::memory_order_relaxed);
		if (!started)
			started = sh.snap.load(std::memory_order_acquire)->started;
		int width = screen.dimx();
		int height = screen.dimy();
		if (!started)
			return render_entry_screen(sh, theme, input, width, height);
		return render_session_screen(sh, theme, input, width, height);
	});

	// The renderer is the only component in the tree that handles events:
	// put the focus on the input explicitly.
	input->TakeFocus();
	return root;
}

} // namespace araya::tui
