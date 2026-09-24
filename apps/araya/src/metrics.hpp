#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Pure formatting for the sidebar and prompt metrics: the latest
// request's token counts and the context-window fill. Header-only and
// dependency-free so it unit-tests without the app or any plugin.
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

} // namespace araya::app
