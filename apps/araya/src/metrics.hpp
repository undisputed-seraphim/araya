#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Pure formatting for the TUI surfaces: the latest request's token
// counts, the context-window fill, and the feed's word wrapping.
// Header-only and dependency-free so it unit-tests without the app or
// any plugin.
namespace araya::app {

// "in/out" for the latest request, or "--" before any usage is folded.
inline std::string token_count_text(std::uint64_t input_tokens, std::uint64_t output_tokens, bool has_usage) {
	if (!has_usage)
		return "--";
	return std::to_string(input_tokens) + "/" + std::to_string(output_tokens);
}

// The latest request's prompt tokens as a whole percent of the model's
// context window, or "--%" when the usage or the window is unknown. A
// tiny prompt against a large window reads "0%" - honest to one percent.
inline std::string context_percent_text(std::uint64_t input_tokens, std::uint64_t context_window, bool has_usage) {
	if (!has_usage || context_window == 0)
		return "--%";
	auto const percent = (input_tokens * 100 + context_window / 2) / context_window;
	return std::to_string(percent) + "%";
}

// A short display form of `text`: the head plus an ellipsis once it
// exceeds `max` bytes (backing off a split UTF-8 codepoint). Long model
// ids have to fit the narrow sidebar and the entry build row.
inline std::string elide(std::string_view text, std::size_t max) {
	if (text.size() <= max)
		return std::string(text);
	std::size_t cut = max;
	while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
		--cut;
	return std::string(text.substr(0, cut)) + "\u2026";
}

// Greedy word wrap to `width` columns: explicit newlines always break,
// runs of spaces collapse, and a word wider than the line is hard-split.
// Always returns at least one (possibly empty) line. Used by the feed so
// scrolling can address real rows rather than relying on the renderer's
// wrap.
inline std::vector<std::string> wrap_lines(std::string_view text, std::size_t width) {
	if (width == 0)
		width = 1;
	std::vector<std::string> lines;
	std::string current;
	auto flush = [&] {
		lines.push_back(current);
		current.clear();
	};

	std::size_t pos = 0;
	while (pos < text.size()) {
		if (text[pos] == '\n') {
			flush();
			++pos;
			continue;
		}
		if (text[pos] == ' ' || text[pos] == '\t') {
			++pos;
			continue;
		}
		std::size_t end = pos;
		while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\n')
			++end;
		std::string_view word = text.substr(pos, end - pos);
		pos = end;

		std::size_t const extra = current.empty() ? 0 : 1;
		if (current.size() + extra + word.size() <= width) {
			if (!current.empty())
				current.push_back(' ');
			current.append(word);
			continue;
		}
		if (!current.empty())
			flush();
		while (word.size() > width) {
			lines.push_back(std::string(word.substr(0, width)));
			word.remove_prefix(width);
		}
		current.assign(word);
	}
	if (!current.empty() || lines.empty())
		flush();
	return lines;
}

// One frame of the streaming spinner. The ASCII tier uses the classic
// four-stroke spinner; the unicode tier uses braille dots.
inline std::string_view spinner_glyph(int frame, bool ascii) {
	static constexpr std::string_view k_ascii[] = {"|", "/", "-", "\\"};
	static constexpr std::string_view k_braille[] = {
		"\u280b", "\u2819", "\u2839", "\u2838", "\u283c", "\u2834", "\u2826", "\u2827", "\u2807", "\u280f"};
	if (ascii)
		return k_ascii[static_cast<std::size_t>(frame) % 4];
	return k_braille[static_cast<std::size_t>(frame) % 10];
}

} // namespace araya::app
