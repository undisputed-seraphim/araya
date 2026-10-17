#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
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

// The full prompt's tokens as a whole percent of the model's context
// window, or "--%" when the usage or the window is unknown. Pass the
// whole prompt (uncached input + cache reads), not just the uncached
// part. A tiny prompt against a large window reads "0%" - honest to one
// percent.
inline std::string context_percent_text(std::uint64_t context_tokens, std::uint64_t context_window, bool has_usage) {
	if (!has_usage || context_window == 0)
		return "--%";
	auto const percent = (context_tokens * 100 + context_window / 2) / context_window;
	return std::to_string(percent) + "%";
}

// The cumulative inputs for the cost line: session totals plus the active
// route's advisory prices (USD per million tokens).
struct cost_input {
	std::uint64_t input_tokens = 0;
	std::uint64_t output_tokens = 0;
	std::uint64_t cache_read_tokens = 0;
	std::uint64_t cache_write_tokens = 0;
	std::optional<double> input_price;
	std::optional<double> output_price;
	std::optional<double> cache_read_price;
	std::optional<double> cache_write_price;
};

// Formats the cumulative estimated cost. A route with no prices (a local or
// free endpoint) shows "$0.00"; otherwise the total with four decimals.
inline std::string cost_text(cost_input const& input) {
	double total = 0.0;
	auto add = [&total](std::uint64_t tokens, std::optional<double> const& price) {
		if (price)
			total += static_cast<double>(tokens) * *price / 1'000'000.0;
	};
	add(input.input_tokens, input.input_price);
	add(input.output_tokens, input.output_price);
	add(input.cache_read_tokens, input.cache_read_price);
	add(input.cache_write_tokens, input.cache_write_price);
	if (total <= 0.0)
		return "$0.00";
	char buffer[32] = {};
	std::snprintf(buffer, sizeof buffer, "$%.4f", total);
	return buffer;
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

// Decodes one UTF-8 codepoint at `pos`; `len` receives its byte length.
// Invalid or truncated bytes decode as the single byte (len 1) so wrapping
// never drops input.
inline std::uint32_t utf8_decode(std::string_view text, std::size_t pos, std::size_t& len) {
	auto const byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
	unsigned char const first = byte(pos);
	if (first < 0x80) {
		len = 1;
		return first;
	}
	std::size_t need = 0;
	std::uint32_t cp = 0;
	if ((first & 0xE0) == 0xC0) {
		need = 1;
		cp = first & 0x1F;
	} else if ((first & 0xF0) == 0xE0) {
		need = 2;
		cp = first & 0x0F;
	} else if ((first & 0xF8) == 0xF0) {
		need = 3;
		cp = first & 0x07;
	} else {
		len = 1;
		return first;
	}
	if (pos + need >= text.size()) {
		len = 1;
		return first;
	}
	for (std::size_t i = 1; i <= need; ++i) {
		unsigned char const next = byte(pos + i);
		if ((next & 0xC0) != 0x80) {
			len = 1;
			return first;
		}
		cp = (cp << 6) | (next & 0x3F);
	}
	// Reject overlong, surrogate, and out-of-range encodings.
	if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) || cp > 0x10FFFF ||
		(cp >= 0xD800 && cp <= 0xDFFF)) {
		len = 1;
		return first;
	}
	len = need + 1;
	return cp;
}

inline bool in_codepoint_range(std::uint32_t cp, std::uint32_t lo, std::uint32_t hi) { return cp >= lo && cp <= hi; }

// Terminal columns for one codepoint: combining/zero-width marks are 0,
// East Asian wide and emoji are 2, everything else is 1. Ambiguous-width
// characters stay 1, matching the common terminal default.
inline std::size_t codepoint_width(std::uint32_t cp) {
	if (cp == 0 || cp < 0x20 || in_codepoint_range(cp, 0x7F, 0x9F))
		return 0;
	if (in_codepoint_range(cp, 0x0300, 0x036F) || in_codepoint_range(cp, 0x1AB0, 0x1AFF) ||
		in_codepoint_range(cp, 0x1DC0, 0x1DFF) || in_codepoint_range(cp, 0x20D0, 0x20FF) ||
		in_codepoint_range(cp, 0xFE00, 0xFE0F) || in_codepoint_range(cp, 0xFE20, 0xFE2F) ||
		in_codepoint_range(cp, 0x200B, 0x200F) || cp == 0xFEFF)
		return 0;
	if (in_codepoint_range(cp, 0x1100, 0x115F) || in_codepoint_range(cp, 0x2329, 0x232A) ||
		in_codepoint_range(cp, 0x2E80, 0x303E) || in_codepoint_range(cp, 0x3041, 0x33FF) ||
		in_codepoint_range(cp, 0x3400, 0x4DBF) || in_codepoint_range(cp, 0x4E00, 0x9FFF) ||
		in_codepoint_range(cp, 0xA000, 0xA4CF) || in_codepoint_range(cp, 0xA960, 0xA97F) ||
		in_codepoint_range(cp, 0xAC00, 0xD7A3) || in_codepoint_range(cp, 0xF900, 0xFAFF) ||
		in_codepoint_range(cp, 0xFE10, 0xFE19) || in_codepoint_range(cp, 0xFE30, 0xFE6F) ||
		in_codepoint_range(cp, 0xFF00, 0xFF60) || in_codepoint_range(cp, 0xFFE0, 0xFFE6) ||
		in_codepoint_range(cp, 0x1F300, 0x1FAFF) || in_codepoint_range(cp, 0x20000, 0x3FFFD))
		return 2;
	return 1;
}

// Display columns in `text` (UTF-8 aware).
inline std::size_t display_width(std::string_view text) {
	std::size_t width = 0;
	for (std::size_t pos = 0; pos < text.size();) {
		std::size_t len = 1;
		width += codepoint_width(utf8_decode(text, pos, len));
		pos += len;
	}
	return width;
}

// The byte prefix of `text` that fits in `max_columns` display columns.
// Always consumes at least one codepoint (so callers make progress even
// when a single wide glyph exceeds the line); never splits a codepoint.
inline std::size_t utf8_prefix_bytes(std::string_view text, std::size_t max_columns) {
	std::size_t pos = 0;
	std::size_t width = 0;
	while (pos < text.size()) {
		std::size_t len = 1;
		auto const cp_width = codepoint_width(utf8_decode(text, pos, len));
		if (pos > 0 && width + cp_width > max_columns)
			break;
		width += cp_width;
		pos += len;
	}
	if (pos == 0 && !text.empty())
		return 1;
	return pos;
}

// Greedy word wrap to `width` display columns: explicit newlines always
// break, runs of spaces collapse, and a word wider than the line is
// hard-split on codepoint boundaries. Always returns at least one
// (possibly empty) line. Used by the feed so scrolling can address real
// rows rather than relying on the renderer's wrap.
inline std::vector<std::string> wrap_lines(std::string_view text, std::size_t width) {
	if (width == 0)
		width = 1;
	std::vector<std::string> lines;
	std::string current;
	std::size_t current_width = 0;
	auto flush = [&] {
		lines.push_back(current);
		current.clear();
		current_width = 0;
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

		std::size_t const word_width = display_width(word);
		std::size_t const extra = current.empty() ? 0 : 1;
		if (current_width + extra + word_width <= width) {
			if (!current.empty()) {
				current.push_back(' ');
				++current_width;
			}
			current.append(word);
			current_width += word_width;
			continue;
		}
		if (!current.empty())
			flush();
		while (display_width(word) > width) {
			auto const cut = utf8_prefix_bytes(word, width);
			lines.push_back(std::string(word.substr(0, cut)));
			word.remove_prefix(cut);
		}
		current.assign(word);
		current_width = display_width(word);
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
