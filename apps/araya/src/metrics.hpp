#pragma once

#include <cstdint>
#include <string>

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

} // namespace araya::app
