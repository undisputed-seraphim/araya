#include "tui_view.hpp"

#include "tui_palette.hpp"
#include "tui_picker.hpp"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
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

namespace {

// The per-event handler's context: the shared/UI state plus the callbacks
// the input component owns. Handlers are free functions over this so the
// key routing stays readable.
struct overlay_env {
	shared_state& sh;
	ui_state& ui;
	std::string& input;
	ftxui::ScreenInteractive& screen;
	std::span<araya::app::command_info const> commands;
	std::function<void(std::string)> const& on_command;
	std::function<void()> const& on_exit;

	void wake() { screen.PostEvent(ftxui::Event::Custom); }

	void open_picker() {
		ui.picker_open = true;
		ui.picker_filter.clear();
		ui.picker_selected = 0;
		sh.sessions_request.store(true, std::memory_order_relaxed);
		wake();
	}

	void submit(std::string cmd) {
		if (cmd == "/quit" || cmd == "/exit") {
			on_exit();
			return;
		}
		if (cmd == "/session") {
			open_picker();
			return;
		}
		// The first submit leaves the entry phase immediately.
		sh.started_ui.store(true, std::memory_order_relaxed);
		if (!cmd.empty())
			on_command(std::move(cmd));
		wake();
	}
};

// The session picker is modal: it consumes every key.
bool handle_picker_key(overlay_env& env, ftxui::Event event) {
	ui_state& ui = env.ui;
	if (event == ftxui::Event::Escape) {
		ui.picker_open = false;
		env.wake();
		return true;
	}
	if (event == ftxui::Event::ArrowUp) {
		ui.picker_selected = std::max(0, ui.picker_selected - 1);
		return true;
	}
	if (event == ftxui::Event::ArrowDown) {
		++ui.picker_selected; // clamped at use
		return true;
	}
	if (event == ftxui::Event::Backspace) {
		if (!ui.picker_filter.empty())
			ui.picker_filter.pop_back();
		ui.picker_selected = 0;
		env.wake();
		return true;
	}
	if (event == ftxui::Event::Return) {
		auto snap = env.sh.snap.load(std::memory_order_acquire);
		auto matches = filter_sessions(snap->sessions, ui.picker_filter);
		if (!matches.empty()) {
			int sel = std::clamp(ui.picker_selected, 0, static_cast<int>(matches.size()) - 1);
			env.sh.started_ui.store(true, std::memory_order_relaxed);
			env.on_command("/session restore " + matches[static_cast<std::size_t>(sel)]->id);
		}
		ui.picker_open = false;
		env.wake();
		return true;
	}
	if (event.is_character()) {
		ui.picker_filter += event.character();
		ui.picker_selected = 0;
		env.wake();
		return true;
	}
	return true; // swallow anything else while the modal is up
}

// The '/' command palette: filter on the typed command token.
bool handle_palette_key(overlay_env& env, ftxui::Event event) {
	ui_state& ui = env.ui;
	auto matches = araya::app::filter_commands(env.commands, araya::app::palette_query(env.input));
	int count = static_cast<int>(matches.size());
	if (event == ftxui::Event::Escape) {
		env.input.clear();
		ui.palette_selected = 0;
		env.wake();
		return true;
	}
	if (event == ftxui::Event::ArrowUp) {
		ui.palette_selected = std::max(0, ui.palette_selected - 1);
		return true;
	}
	if (event == ftxui::Event::ArrowDown) {
		ui.palette_selected = std::min(std::max(0, count - 1), ui.palette_selected + 1);
		return true;
	}
	if (event == ftxui::Event::Return || event == ftxui::Event::Tab) {
		if (count > 0) {
			int sel = std::clamp(ui.palette_selected, 0, count - 1);
			auto const& command = env.commands[matches[static_cast<std::size_t>(sel)]];
			if (command.name == "session") {
				env.input.clear();
				env.open_picker();
			} else if (command.name == "quit") {
				env.on_exit();
			} else if (araya::app::takes_args(command)) {
				// Complete to '/name ' so the arguments can be typed; the
				// next Enter submits.
				env.input = "/" + std::string(command.name) + " ";
			} else {
				env.input.clear();
				env.sh.started_ui.store(true, std::memory_order_relaxed);
				env.on_command("/" + std::string(command.name));
			}
		}
		ui.palette_selected = 0;
		env.wake();
		return true;
	}
	// Any other edit re-filters: reset the selection and let the Input
	// handle it.
	if (event.is_character() || event == ftxui::Event::Backspace)
		ui.palette_selected = 0;
	return false;
}

bool handle_key(overlay_env& env, ftxui::Event event) {
	if (event == ftxui::Event::CtrlD) {
		env.on_exit();
		return true;
	}
	if (event == ftxui::Event::CtrlC)
		return true; // the TUI convention: Ctrl+D exits, Ctrl+C does nothing
	if (env.ui.picker_open)
		return handle_picker_key(env, event);
	if (araya::app::palette_open(env.input))
		return handle_palette_key(env, event);

	// The feed pane: Tab cycles back to the input, the scroll keys walk
	// the wrapped rows, and typing pulls focus back to the input.
	if (env.ui.focus == pane_focus::feed) {
		ui_state& ui = env.ui;
		int const page = std::max(1, ui.feed_view_lines - 1);
		if (event == ftxui::Event::Tab)
			ui.focus = pane_focus::input;
		else if (event == ftxui::Event::ArrowUp)
			++ui.feed_scroll;
		else if (event == ftxui::Event::ArrowDown)
			ui.feed_scroll = std::max(0, ui.feed_scroll - 1);
		else if (event == ftxui::Event::PageUp)
			ui.feed_scroll += page;
		else if (event == ftxui::Event::PageDown)
			ui.feed_scroll = std::max(0, ui.feed_scroll - page);
		else if (event == ftxui::Event::Home)
			ui.feed_scroll = ui.feed_total_lines; // clamped at render
		else if (event == ftxui::Event::End)
			ui.feed_scroll = 0;
		else if (event.is_character() || event == ftxui::Event::Backspace)
			ui.focus = pane_focus::input; // let the Input take it
		else
			return false;
		env.wake();
		return true;
	}

	// Input focused: Tab moves to the feed; Enter submits; anything else
	// goes to the Input component.
	if (event == ftxui::Event::Tab) {
		env.ui.focus = pane_focus::feed;
		env.wake();
		return true;
	}
	if (event != ftxui::Event::Return)
		return false;
	auto cmd = env.input;
	env.input.clear();
	env.ui.palette_selected = 0;
	env.submit(std::move(cmd));
	return true;
}

} // namespace

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

	// on_command/on_exit are build_ui parameters: capture them by value -
	// the component outlives this frame, and a [&] capture of them is a
	// dangling reference (the freed slots get reused at -O2). The command
	// list is a view over static storage, so it is safe to copy too.
	input |= CatchEvent([&, commands, on_command = std::move(on_command), on_exit = std::move(on_exit)](Event event) {
		overlay_env env{sh, ui, input_buffer, screen, commands, on_command, on_exit};
		return handle_key(env, event);
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
