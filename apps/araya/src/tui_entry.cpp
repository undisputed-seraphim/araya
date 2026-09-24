#include "tui_view.hpp"

#include "tui_chrome.hpp"
#include "tui_palette.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <string>
#include <utility>

// The startup screen: a wordmark over the prompt box (with the command
// palette above it when open), a tip line, and a bottom status bar.
// Shown until the first submit (snapshot.started). Every
// connection-derived value is a placeholder for now.
namespace araya::tui {

ftxui::Element render_entry_screen(render_context const& rc) {
	using namespace ftxui;

	auto snap = rc.sh.snap.load(std::memory_order_acquire);
	int box_width = std::clamp(rc.width - 8, 32, 76);

	Element build_row = hbox({
		text("Build") | color(dim_text()),
		text(" \u00b7 ") | color(accent()),
		text("-- --") | color(accent()),
		filler(),
		text("type / for commands") | color(dim_text()),
	});
	Element content = vbox({
		rc.input->Render(),
		std::move(build_row),
	});
	Element box = prompt_box(std::move(content), rc.theme.ascii) | size(WIDTH, EQUAL, box_width);

	Element tip = hbox({
		text(rc.theme.ascii ? "*" : "\u25cf") | color(Color::Yellow),
		text(" Tip  ") | bold,
		text("Type / to see all available commands") | color(dim_text()),
	});

	Element status = hbox({
		text(snap->cwd_branch) | color(dim_text()),
		filler(),
		text("araya " + snap->version) | color(dim_text()),
	});

	Elements centered;
	centered.push_back(wordmark());
	centered.push_back(text(""));
	if (araya::app::palette_open(rc.input_text))
		centered.push_back(render_palette(
			rc.commands, araya::app::palette_query(rc.input_text), rc.ui.palette_selected, box_width, rc.theme.ascii));
	centered.push_back(std::move(box));
	centered.push_back(text(""));
	centered.push_back(std::move(tip));

	return vbox({
		filler(),
		hbox({
			filler(),
			vbox(std::move(centered)),
			filler(),
		}),
		filler(),
		std::move(status),
	});
}

} // namespace araya::tui
