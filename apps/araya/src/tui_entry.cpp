#include "tui_view.hpp"

#include "tui_chrome.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <string>
#include <utility>

// The startup screen: a wordmark over the prompt box, a tip line, and a
// bottom status bar. Shown until the first submit (snapshot.started).
// Every connection-derived value is a placeholder for now.
namespace araya::tui {

ftxui::Element render_entry_screen(
	shared_state const& sh,
	tui_theme const& theme,
	ftxui::Component const& input,
	int width,
	int /*height*/) {
	using namespace ftxui;

	auto snap = sh.snap.load(std::memory_order_acquire);
	int box_width = std::clamp(width - 8, 32, 76);

	Element build_row = hbox({
		text("Build") | color(dim_text()),
		text(" \u00b7 ") | color(accent()),
		text("-- --") | color(accent()),
		filler(),
		text("ctrl+p commands") | color(dim_text()),
	});
	Element content = vbox({
		input->Render(),
		std::move(build_row),
	});
	Element box = prompt_box(std::move(content), theme.ascii) | size(WIDTH, EQUAL, box_width);

	Element tip = hbox({
		text(theme.ascii ? "*" : "\u25cf") | color(Color::Yellow),
		text(" Tip  ") | bold,
		text("Press ctrl+p to see all available actions and commands") | color(dim_text()),
	});

	Element status = hbox({
		text(snap->cwd_branch) | color(dim_text()),
		filler(),
		text("araya " + snap->version) | color(dim_text()),
	});

	return vbox({
		filler(),
		hbox({
			filler(),
			vbox({
				wordmark(),
				text(""),
				std::move(box),
				text(""),
				std::move(tip),
			}),
			filler(),
		}),
		filler(),
		std::move(status),
	});
}

} // namespace araya::tui
