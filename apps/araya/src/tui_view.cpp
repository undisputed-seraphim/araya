#include "tui_view.hpp"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>

#include <algorithm>
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
	std::string& input_buffer,
	ftxui::ScreenInteractive& screen,
	std::function<void(std::string)> on_command,
	std::function<void()> on_exit) {
	using namespace ftxui;

	InputOption input_options;
	input_options.placeholder = "command (help)";
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
		if (cmd == "quit") {
			on_exit();
			return true;
		}
		if (!cmd.empty())
			on_command(std::move(cmd));
		screen.PostEvent(Event::Custom);
		return true;
	});

	auto log_view = Renderer([&] {
		auto snap = sh.snap.load(std::memory_order_acquire);
		Elements lines;
		std::size_t begin = snap->log.size() > 40 ? snap->log.size() - 40 : 0;
		for (std::size_t i = begin; i < snap->log.size(); ++i)
			lines.push_back(text(snap->log[i]));
		return vbox(lines) | border;
	});

	// The prompt box: the input at the top of an 8-row-tall area with
	// the opencode-style dark background.
	Color const k_prompt_bg = Color::RGB(0x28, 0x28, 0x28);

	auto prompt_box = Renderer(input, [&, input, k_prompt_bg] {
		return vbox({
				   input->Render(),
				   filler(),
			   }) |
			   size(HEIGHT, EQUAL, 8) | bgcolor(k_prompt_bg);
	});

	// The hint line under the prompt: the working directory on the
	// right, and the ctrl+p hint flushed to the far right of the main
	// column. ctrl+p is a no-op for now.
	auto hint_view = Renderer([&, k_prompt_bg] {
		auto snap = sh.snap.load(std::memory_order_acquire);
		return hbox({
				   filler(),
				   text(snap->cwd) | dim,
				   text("   ctrl+p commands") | dim,
			   }) |
			   bgcolor(k_prompt_bg);
	});

	// The right-hand status pane, opencode-style: solid dark background,
	// session title at the top (the session id until titles exist), the
	// placeholder metric rows, the MCP/LSP sections, the component list
	// in the middle space, and the cwd:branch and version lines flushed
	// to the bottom.
	Color const k_sidebar_bg = Color::RGB(0x12, 0x12, 0x12);
	Color const k_sidebar_dim = Color::GrayDark;
	Color const k_accent = Color::Cyan;

	auto sidebar_view = Renderer([&, k_sidebar_bg, k_sidebar_dim, k_accent] {
		auto snap = sh.snap.load(std::memory_order_acquire);
		auto section = [&](std::string_view title, std::vector<std::string> const& rows) {
			Elements out;
			out.push_back(text(title) | color(k_sidebar_dim) | bold);
			for (auto const& row : rows)
				out.push_back(text(" " + row));
			if (rows.empty())
				out.push_back(text("  (none)") | color(k_sidebar_dim));
			return vbox(out);
		};

		// The component list: glyph, name, and state per row; errors as
		// a dim red line underneath.
		Elements component_rows;
		for (auto const& c : snap->components) {
			auto style = state_style_for(theme, c);
			component_rows.push_back(hbox({
				text(std::string(style.glyph)) | color(style.color),
				text(" " + c.name),
				filler(),
				text(c.state) | color(style.color),
			}));
			if (!c.error.empty())
				component_rows.push_back(text("  " + c.error) | color(Color::Red) | dim);
		}
		if (component_rows.empty())
			component_rows.push_back(text(" (booting...)") | color(k_sidebar_dim));

		return vbox({
				   text(snap->session) | bold | color(k_accent),
				   text("tokens   --"),
				   text("context  --%"),
				   text("cost     $0.00"),
				   separatorEmpty(),
				   section("MCP", {}),
				   section("LSP", {}),
				   separatorEmpty(),
				   vbox(component_rows),
				   filler(),
				   text(snap->cwd_branch) | color(k_sidebar_dim),
				   hbox({
					   text(theme.ascii ? "*" : "●") | color(Color::Green),
					   text(" " + snap->version) | color(k_sidebar_dim),
				   }),
			   }) |
			   bgcolor(k_sidebar_bg);
	});

	// The main column: the feed on top, the prompt box, then the hint
	// line at the bottom. The component list lives in the sidebar.
	// (Component is a shared_ptr wrapper: the lambdas below capture the
	// panes by value so they outlive build_ui's scope.)
	auto container = Container::Vertical({log_view, prompt_box});
	auto root = Renderer(container, [&, container, sidebar_view, hint_view] {
		int sidebar_width = std::max(26, screen.dimx() / 6);
		sidebar_width = std::min(sidebar_width, screen.dimx() / 3);
		return hbox({
			vbox({
				container->Render() | flex,
				hint_view->Render(),
			}) | flex,
			separator(),
			sidebar_view->Render() | size(WIDTH, EQUAL, sidebar_width),
		});
	});

	// The container starts focused on its first child (the feed pane),
	// which would swallow every keystroke: put the focus on the command
	// input explicitly.
	input->TakeFocus();
	return root;
}

} // namespace araya::tui
