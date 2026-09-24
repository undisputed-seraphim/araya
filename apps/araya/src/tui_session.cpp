#include "tui_view.hpp"

#include "input_route.hpp"
#include "metrics.hpp"
#include "tui_chrome.hpp"
#include "tui_palette.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <mutex>
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

// The tail of the folded messages, one role-marked paragraph each, with
// the in-flight model text appended (dim, cursor-marked) while a turn
// streams before it settles into an assistant/message.
ftxui::Element build_feed(snapshot const& snap, std::string const& stream, bool ascii) {
	using namespace ftxui;
	Elements feed;
	std::size_t begin = snap.messages.size() > 60 ? snap.messages.size() - 60 : 0;
	for (std::size_t i = begin; i < snap.messages.size(); ++i) {
		auto const& message = snap.messages[i];
		feed.push_back(
			paragraph(std::string(araya::app::role_marker(message.role, ascii)) + " " + message.text) |
			color(role_color(message.role)));
	}
	if (!stream.empty()) {
		// A block cursor marks the row as still streaming; the ASCII tier
		// uses a caret (the prompt bar owns '|' and the accent bar the
		// half block).
		auto const cursor = ascii ? "^" : "\u2588";
		feed.push_back(
			paragraph(std::string(araya::app::role_marker("assistant", ascii)) + " " + stream + cursor) |
			color(role_color("assistant")) | dim);
	}
	if (feed.empty())
		feed.push_back(text("(no messages yet)") | color(dim_text()));
	return vbox(std::move(feed)) | border;
}

// The command-output strip: engine lines (command output, session
// events), kept close to the prompt. Entries may carry newlines (the
// help text is one entry), so flatten to lines and tail those - a tall
// multi-line output shows its end rather than being clipped to its
// head. Sized to a fraction of the terminal.
ftxui::Element build_output(snapshot const& snap, int height) {
	using namespace ftxui;
	int output_height = std::clamp(height / 4, 5, 12);
	std::size_t keep = static_cast<std::size_t>(std::max(1, output_height - 2));
	std::vector<std::string> output_lines;
	for (auto const& entry : snap.log) {
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
	std::size_t begin = output_lines.size() > keep ? output_lines.size() - keep : 0;
	for (std::size_t i = begin; i < output_lines.size(); ++i)
		output.push_back(text(output_lines[i]) | color(dim_text()));
	if (output.empty())
		output.push_back(text("output") | color(dim_text()));
	return vbox(std::move(output)) | border | size(HEIGHT, EQUAL, output_height);
}

// The prompt box: input plus the latest request's token/context metrics.
ftxui::Element build_prompt(render_context const& rc, snapshot const& snap) {
	using namespace ftxui;
	auto const& tokens = snap.tokens;
	Element meta = hbox({
		text("tokens " + araya::app::token_count_text(tokens.input_tokens, tokens.output_tokens, tokens.has_usage)) |
			color(dim_text()),
		text("   " + araya::app::context_percent_text(tokens.input_tokens, tokens.context_window, tokens.has_usage)) |
			color(dim_text()),
		text("   $--") | color(dim_text()),
		filler(),
		text("type / for commands") | color(dim_text()),
	});
	return prompt_box(vbox({rc.input->Render(), std::move(meta)}), rc.theme.ascii);
}

// The sidebar: session title, the token/context metrics (cost is a stub
// until a pricing pass), the empty MCP/LSP sections, the component list,
// then cwd and version.
ftxui::Element build_sidebar(snapshot const& snap, tui_theme const& theme, int sidebar_width) {
	using namespace ftxui;
	Elements component_rows;
	for (auto const& c : snap.components) {
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
		component_rows.push_back(text(" (booting...)") | color(dim_text()));

	return vbox({
			   text(snap.title) | bold | color(accent()),
			   text("provider " + (snap.provider.empty() ? std::string("--") : snap.provider)) | color(dim_text()),
			   text("model    " + (snap.model.empty() ? std::string("--") : araya::app::elide(snap.model, 24))) |
				   color(dim_text()),
			   text(
				   "tokens   " + araya::app::token_count_text(
									 snap.tokens.input_tokens, snap.tokens.output_tokens, snap.tokens.has_usage)),
			   text(
				   "context  " + araya::app::context_percent_text(
									 snap.tokens.input_tokens, snap.tokens.context_window, snap.tokens.has_usage)),
			   text("cost     $0.00"),
			   separatorEmpty(),
			   section("MCP", {}),
			   section("LSP", {}),
			   separatorEmpty(),
			   text("Components") | color(dim_text()) | bold,
			   vbox(std::move(component_rows)),
			   filler(),
			   text(snap.cwd_branch) | color(dim_text()),
			   hbox({
				   text(theme.ascii ? "*" : "\u25cf") | color(Color::Green),
				   text(" " + snap.version) | color(dim_text()),
			   }),
		   }) |
		   bgcolor(sidebar_bg()) | size(WIDTH, EQUAL, sidebar_width);
}

} // namespace

ftxui::Element render_session_screen(render_context const& rc) {
	using namespace ftxui;

	auto snap = rc.sh.snap.load(std::memory_order_acquire);
	// The in-flight model text, if a turn is streaming right now.
	std::string stream;
	{
		std::lock_guard lock(rc.sh.stream_mutex);
		if (rc.sh.stream_active)
			stream = rc.sh.stream_content;
	}

	int sidebar_width = std::max(26, rc.width / 6);
	sidebar_width = std::min(sidebar_width, rc.width / 3);
	int main_width = std::max(20, rc.width - sidebar_width - 1); // minus the separator

	Elements main_elements;
	main_elements.push_back(build_feed(*snap, stream, rc.theme.ascii) | flex);
	main_elements.push_back(build_output(*snap, rc.height));
	if (araya::app::palette_open(rc.input_text))
		main_elements.push_back(render_palette(
			rc.commands, araya::app::palette_query(rc.input_text), rc.ui.palette_selected, main_width, rc.theme.ascii));
	main_elements.push_back(build_prompt(rc, *snap));
	main_elements.push_back(hbox({text(snap->cwd) | color(dim_text())}));

	return hbox({
		vbox(std::move(main_elements)) | flex,
		separator(),
		build_sidebar(*snap, rc.theme, sidebar_width),
	});
}

} // namespace araya::tui
