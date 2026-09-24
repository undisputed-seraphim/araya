#include "tui_view.hpp"

#include "input_route.hpp"
#include "tui_chrome.hpp"
#include "tui_palette.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The session screen: the conversation feed (the folded session
// messages), the command-output strip, the prompt box (with the command
// palette above it when open), and the status sidebar. Message rendering
// is role-marker rows for now; tokens, context, and cost stay
// placeholders.
namespace araya::tui {
namespace {

ftxui::Element section(std::string_view title, std::vector<std::string> const& rows) {
	using namespace ftxui;
	Elements out;
	out.push_back(text(std::string(title)) | color(dim_text()) | bold);
	for (auto const& row : rows)
		out.push_back(text(" " + row));
	if (rows.empty())
		out.push_back(text("  (none)") | color(dim_text()));
	return vbox(std::move(out));
}

ftxui::Color role_color(std::string_view role) {
	if (role == "assistant")
		return accent();
	if (role == "system")
		return dim_text();
	if (role == "tool")
		return ftxui::Color::Yellow;
	return ftxui::Color::White;
}

} // namespace

ftxui::Element render_session_screen(render_context const& rc) {
	using namespace ftxui;

	auto snap = rc.sh.snap.load(std::memory_order_acquire);

	int sidebar_width = std::max(26, rc.width / 6);
	sidebar_width = std::min(sidebar_width, rc.width / 3);
	int main_width = std::max(20, rc.width - sidebar_width - 1); // minus the separator

	// The conversation: the tail of the folded messages, one
	// role-marked paragraph each.
	Elements feed;
	std::size_t feed_begin = snap->messages.size() > 60 ? snap->messages.size() - 60 : 0;
	for (std::size_t i = feed_begin; i < snap->messages.size(); ++i) {
		auto const& message = snap->messages[i];
		feed.push_back(
			paragraph(std::string(araya::app::role_marker(message.role, rc.theme.ascii)) + " " + message.text) |
			color(role_color(message.role)));
	}
	if (feed.empty())
		feed.push_back(text("(no messages yet)") | color(dim_text()));
	Element feed_view = vbox(std::move(feed)) | border;

	// The command-output strip: engine lines (command output, session
	// events), kept close to the prompt. Entries may carry newlines (the
	// help text is one entry), so flatten to lines and tail those - a
	// tall multi-line output shows its end rather than being clipped to
	// its head. Sized to a fraction of the terminal.
	int output_height = std::clamp(rc.height / 4, 5, 12);
	std::size_t keep = static_cast<std::size_t>(std::max(1, output_height - 2));
	std::vector<std::string> output_lines;
	for (auto const& entry : snap->log) {
		std::string_view text_view = entry;
		std::size_t start = 0;
		while (true) {
			auto newline = text_view.find('\n', start);
			output_lines.emplace_back(
				text_view.substr(start, newline == std::string_view::npos ? newline : newline - start));
			if (newline == std::string_view::npos)
				break;
			start = newline + 1;
		}
	}
	Elements output;
	std::size_t output_begin = output_lines.size() > keep ? output_lines.size() - keep : 0;
	for (std::size_t i = output_begin; i < output_lines.size(); ++i)
		output.push_back(text(output_lines[i]) | color(dim_text()));
	if (output.empty())
		output.push_back(text("output") | color(dim_text()));
	Element output_view = vbox(std::move(output)) | border | size(HEIGHT, EQUAL, output_height);

	// The prompt box: input plus a placeholder metrics row.
	Element meta = hbox({
		text("tokens --") | color(dim_text()),
		text("   --%") | color(dim_text()),
		text("   $--") | color(dim_text()),
		filler(),
		text("type / for commands") | color(dim_text()),
	});
	Element prompt = prompt_box(vbox({rc.input->Render(), std::move(meta)}), rc.theme.ascii);

	Elements main_elements;
	main_elements.push_back(std::move(feed_view) | flex);
	main_elements.push_back(std::move(output_view));
	if (araya::app::palette_open(rc.input_text))
		main_elements.push_back(render_palette(
			rc.commands, araya::app::palette_query(rc.input_text), rc.ui.palette_selected, main_width, rc.theme.ascii));
	main_elements.push_back(std::move(prompt));
	main_elements.push_back(hbox({text(snap->cwd) | color(dim_text())}));
	Element main = vbox(std::move(main_elements)) | flex;

	// The sidebar: session title, placeholder context metrics, the
	// empty MCP/LSP sections, the component list, then cwd and version.
	Elements component_rows;
	for (auto const& c : snap->components) {
		auto style = state_style_for(rc.theme, c);
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
		component_rows.push_back(text(" (booting...)") | color(dim_text()));

	Element sidebar = vbox({
						  text(snap->title) | bold | color(accent()),
						  text("tokens   --"),
						  text("context  --%"),
						  text("cost     $0.00"),
						  separatorEmpty(),
						  section("MCP", {}),
						  section("LSP", {}),
						  separatorEmpty(),
						  text("Components") | color(dim_text()) | bold,
						  vbox(std::move(component_rows)),
						  filler(),
						  text(snap->cwd_branch) | color(dim_text()),
						  hbox({
							  text(rc.theme.ascii ? "*" : "\u25cf") | color(Color::Green),
							  text(" " + snap->version) | color(dim_text()),
						  }),
					  }) |
					  bgcolor(sidebar_bg());

	return hbox({
		std::move(main) | flex,
		separator(),
		std::move(sidebar) | size(WIDTH, EQUAL, sidebar_width),
	});
}

} // namespace araya::tui
