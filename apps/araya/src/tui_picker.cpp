#include "tui_picker.hpp"

#include "input_route.hpp"
#include "tui_chrome.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

// The session picker modal: title + esc hint, a search line with a block
// cursor, the stored sessions (dim date over a selectable title), and a
// footer. The list scrolls a window around the selection.
namespace araya::tui {
namespace {

std::string clip(std::string_view text, std::size_t max) {
	if (text.size() <= max || max == 0)
		return std::string(text);
	std::size_t cut = max;
	while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
		--cut;
	return std::string(text.substr(0, cut)) + "\u2026";
}

} // namespace

std::vector<session_row> filter_sessions(std::vector<session_row> const& sessions, std::string_view filter) {
	std::vector<session_row> out;
	for (auto const& session : sessions) {
		if (filter.empty() || araya::app::contains_ci(session.title, filter) ||
			araya::app::contains_ci(session.id, filter))
			out.push_back(session);
	}
	return out;
}

ftxui::Element
render_picker(snapshot const& snap, std::string_view filter, int selected, int width, int /*height*/, bool ascii) {
	using namespace ftxui;

	auto sessions = filter_sessions(snap.sessions, filter);
	int sel = sessions.empty() ? 0 : std::clamp(selected, 0, static_cast<int>(sessions.size()) - 1);
	int panel_width = std::min(64, std::max(36, width - 8));

	constexpr std::size_t k_max_rows = 6;
	std::size_t first = 0;
	if (sessions.size() > k_max_rows && static_cast<std::size_t>(sel) >= k_max_rows / 2)
		first = std::min(sessions.size() - k_max_rows, static_cast<std::size_t>(sel) - k_max_rows / 2);

	Elements rows;
	if (sessions.empty()) {
		rows.push_back(text(" (no stored sessions)") | color(dim_text()));
	} else {
		std::size_t inner = static_cast<std::size_t>(std::max(8, panel_width - 4));
		for (std::size_t i = first; i < sessions.size() && i < first + k_max_rows; ++i) {
			rows.push_back(text(sessions[i].date) | color(dim_text()));
			Element title = hbox({text(" " + clip(sessions[i].title, inner - 1)), filler()});
			if (static_cast<int>(i) == sel)
				title = title | bgcolor(palette_selected_bg()) | color(Color::Black);
			rows.push_back(std::move(title));
		}
	}

	Element search = hbox({
		text(ascii ? "search  " : "\u2315 ") | color(dim_text()),
		text(std::string(filter)),
		text(ascii ? "|" : "\u258c") | color(accent()),
	});

	return vbox({
			   hbox({
				   text("Sessions") | bold | color(accent()),
				   filler(),
				   text("esc") | color(dim_text()),
			   }),
			   std::move(search),
			   separatorEmpty(),
			   vbox(std::move(rows)),
			   separatorEmpty(),
			   hbox({
				   text("enter restore") | color(dim_text()),
				   text("   esc close") | color(dim_text()),
			   }),
		   }) |
		   border | bgcolor(palette_bg()) | size(WIDTH, EQUAL, panel_width);
}

} // namespace araya::tui
