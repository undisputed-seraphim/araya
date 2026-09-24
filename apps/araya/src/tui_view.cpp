#include "tui_view.hpp"

#include "tui_palette.hpp"
#include "tui_picker.hpp"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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
	ui_state& ui,
	std::string& input_buffer,
	ftxui::ScreenInteractive& screen,
	std::function<void(std::string)> on_command,
	std::function<void()> on_exit) {
	using namespace ftxui;

	auto commands = araya::app::command_list();

	InputOption input_options;
	input_options.placeholder = "ask anything, or / for commands";
	Component input = Input(&input_buffer, input_options);

	auto open_picker = [&] {
		ui.picker_open = true;
		ui.picker_filter.clear();
		ui.picker_selected = 0;
		sh.sessions_request.store(true, std::memory_order_relaxed);
		screen.PostEvent(Event::Custom);
	};

	// on_command/on_exit are build_ui parameters: capture them by value -
	// the component outlives this frame, and a [&] capture of them is a
	// dangling reference (the freed slots get reused at -O2). The command
	// list is a view over static storage, so it is safe to copy too.
	input |=
		CatchEvent([&, input, commands, open_picker, on_command = std::move(on_command), on_exit = std::move(on_exit)](
					   Event event) {
			if (event == Event::CtrlD) {
				on_exit();
				return true;
			}
			if (event == Event::CtrlC)
				return true; // the TUI convention: Ctrl+D exits, Ctrl+C does nothing

			// The session picker is modal: it consumes every key.
			if (ui.picker_open) {
				if (event == Event::Escape) {
					ui.picker_open = false;
					screen.PostEvent(Event::Custom);
					return true;
				}
				if (event == Event::ArrowUp) {
					ui.picker_selected = std::max(0, ui.picker_selected - 1);
					return true;
				}
				if (event == Event::ArrowDown) {
					++ui.picker_selected; // clamped at use
					return true;
				}
				if (event == Event::Backspace) {
					if (!ui.picker_filter.empty())
						ui.picker_filter.pop_back();
					ui.picker_selected = 0;
					screen.PostEvent(Event::Custom);
					return true;
				}
				if (event == Event::Return) {
					auto snap = sh.snap.load(std::memory_order_acquire);
					auto matches = filter_sessions(snap->sessions, ui.picker_filter);
					if (!matches.empty()) {
						int sel = std::clamp(ui.picker_selected, 0, static_cast<int>(matches.size()) - 1);
						sh.started_ui.store(true, std::memory_order_relaxed);
						on_command("/session restore " + matches[static_cast<std::size_t>(sel)].id);
					}
					ui.picker_open = false;
					screen.PostEvent(Event::Custom);
					return true;
				}
				if (event.is_character()) {
					ui.picker_filter += event.character();
					ui.picker_selected = 0;
					screen.PostEvent(Event::Custom);
					return true;
				}
				return true; // swallow anything else while the modal is up
			}

			// The '/' command palette: filter on the typed command token.
			if (araya::app::palette_open(input_buffer)) {
				auto query = araya::app::palette_query(input_buffer);
				auto matches = araya::app::filter_commands(commands, query);
				int count = static_cast<int>(matches.size());
				if (event == Event::Escape) {
					input_buffer.clear();
					ui.palette_selected = 0;
					screen.PostEvent(Event::Custom);
					return true;
				}
				if (event == Event::ArrowUp) {
					ui.palette_selected = std::max(0, ui.palette_selected - 1);
					return true;
				}
				if (event == Event::ArrowDown) {
					ui.palette_selected = std::min(std::max(0, count - 1), ui.palette_selected + 1);
					return true;
				}
				if (event == Event::Return || event == Event::Tab) {
					if (count > 0) {
						int sel = std::clamp(ui.palette_selected, 0, count - 1);
						auto const& command = commands[matches[static_cast<std::size_t>(sel)]];
						if (command.name == "session") {
							input_buffer.clear();
							open_picker();
						} else if (command.name == "quit") {
							on_exit();
						} else if (araya::app::takes_args(command)) {
							// Complete to '/name ' so the arguments can be
							// typed; the next Enter submits.
							input_buffer = "/" + std::string(command.name) + " ";
						} else {
							input_buffer.clear();
							sh.started_ui.store(true, std::memory_order_relaxed);
							on_command("/" + std::string(command.name));
						}
					}
					ui.palette_selected = 0;
					screen.PostEvent(Event::Custom);
					return true;
				}
				// Any other edit re-filters: reset the selection and let
				// the Input handle it.
				if (event.is_character() || event == Event::Backspace) {
					ui.palette_selected = 0;
					return false;
				}
				return false;
			}

			if (event != Event::Return)
				return false;
			auto cmd = input_buffer;
			input_buffer.clear();
			ui.palette_selected = 0;
			if (cmd == "/quit" || cmd == "/exit") {
				on_exit();
				return true;
			}
			if (cmd == "/session") {
				open_picker();
				return true;
			}
			// The first submit leaves the entry phase immediately.
			sh.started_ui.store(true, std::memory_order_relaxed);
			if (!cmd.empty())
				on_command(std::move(cmd));
			screen.PostEvent(Event::Custom);
			return true;
		});

	// One input, two phases: the root renders the entry screen until the
	// engine (or the UI's optimistic flag) says the conversation began,
	// with the session picker layered over either screen as a modal.
	auto root = Renderer(input, [&, input, commands] {
		bool started = sh.started_ui.load(std::memory_order_relaxed);
		auto snap = sh.snap.load(std::memory_order_acquire);
		if (!started)
			started = snap->started;
		render_context rc{
			.sh = sh,
			.theme = theme,
			.ui = ui,
			.commands = commands,
			.input_text = input_buffer,
			.input = input,
			.width = screen.dimx(),
			.height = screen.dimy(),
		};
		Element base = started ? render_session_screen(rc) : render_entry_screen(rc);
		if (ui.picker_open)
			return dbox({
				std::move(base),
				center(render_picker(*snap, ui.picker_filter, ui.picker_selected, rc.width, rc.height, theme.ascii)),
			});
		return base;
	});

	// The renderer is the only component in the tree that handles events:
	// put the focus on the input explicitly.
	input->TakeFocus();
	return root;
}

} // namespace araya::tui
