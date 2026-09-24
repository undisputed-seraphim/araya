#include "tui_palette.hpp"

#include "tui_chrome.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

// The command palette list: name on the left, summary on the right, the
// selected row on an amber bar. The caller only inserts it while the
// palette is open.
namespace araya::tui {
namespace {

// Clips to `max` bytes on a codepoint boundary, appending an ellipsis
// when it had to cut.
std::string clip(std::string_view text, std::size_t max) {
	if (text.size() <= max || max == 0)
		return std::string(text);
	std::size_t cut = max;
	while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
		--cut;
	return std::string(text.substr(0, cut)) + "\u2026";
}

int display_width(std::string_view text) { return static_cast<int>(text.size()); }

} // namespace

ftxui::Element render_palette(
	std::span<araya::app::command_info const> commands,
	std::string_view query,
	int selected,
	int width,
	bool /*ascii*/) {
	using namespace ftxui;

	auto matches = araya::app::filter_commands(commands, query);
	// A fixed name column keeps the summaries aligned; the summaries are
	// clipped so no row overflows the panel and overlaps its neighbour.
	int inner = std::max(8, width - 4); // border + one space of padding each side
	int name_col = 0;
	for (auto const& command : commands)
		name_col = std::max(name_col, display_width(command.name) + 2);
	name_col = std::min(name_col, std::max(4, inner / 2));
	int summary_col = std::max(0, inner - name_col);

	Elements rows;
	if (matches.empty()) {
		rows.push_back(text(" (no matching commands)") | color(dim_text()));
	} else {
		int sel = std::clamp(selected, 0, static_cast<int>(matches.size()) - 1);
		for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
			auto const& command = commands[matches[static_cast<std::size_t>(i)]];
			std::string label = "/" + std::string(command.name);
			label.append(static_cast<std::size_t>(std::max(0, name_col - display_width(label))), ' ');
			std::string summary = clip(command.summary, static_cast<std::size_t>(summary_col));
			if (i == sel) {
				rows.push_back(
					hbox({text(label), text(std::move(summary)), filler()}) | bgcolor(palette_selected_bg()) |
					color(Color::Black));
			} else {
				rows.push_back(hbox({
					text(label),
					text(std::move(summary)) | color(dim_text()),
					filler(),
				}));
			}
		}
	}
	return vbox(std::move(rows)) | border | bgcolor(palette_bg()) | size(WIDTH, EQUAL, width);
}

} // namespace araya::tui
