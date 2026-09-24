#pragma once

#include <ftxui/dom/elements.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

// The chrome shared by the entry and session screens: the palette, the
// accent-barred prompt box, and the wordmark. Header-only so both
// screen translation units see the same values.
namespace araya::tui {

inline ftxui::Color prompt_bg() { return ftxui::Color::RGB(0x28, 0x28, 0x28); }

inline ftxui::Color sidebar_bg() { return ftxui::Color::RGB(0x12, 0x12, 0x12); }

inline ftxui::Color dim_text() { return ftxui::Color::GrayDark; }

inline ftxui::Color accent() { return ftxui::Color::Cyan; }

// The palette and picker share a slightly darker surface than the prompt
// box, with an amber selection bar.
inline ftxui::Color palette_bg() { return ftxui::Color::RGB(0x1c, 0x1c, 0x1c); }

inline ftxui::Color palette_selected_bg() { return ftxui::Color::RGB(0xc8, 0xa2, 0x5a); }

// A prompt box: a two-row accent bar on the left of `content` (the input
// row plus a meta row), on the prompt background.
inline ftxui::Element prompt_box(ftxui::Element content, bool ascii) {
	auto glyph = ascii ? "|" : "\u258c"; // ▌
	ftxui::Element bar = ftxui::vbox({ftxui::text(glyph), ftxui::text(glyph)}) | ftxui::color(accent());
	return ftxui::hbox({std::move(bar), std::move(content) | ftxui::flex}) | ftxui::bgcolor(prompt_bg());
}

// The styled-text wordmark: spaced bold letters in the accent color.
inline ftxui::Element wordmark() {
	ftxui::Elements letters;
	std::string_view word = "araya";
	for (std::size_t i = 0; i < word.size(); ++i) {
		if (i)
			letters.push_back(ftxui::text(" "));
		letters.push_back(ftxui::text(std::string(1, word[i])) | ftxui::bold | ftxui::color(accent()));
	}
	return ftxui::hbox(std::move(letters));
}

} // namespace araya::tui
