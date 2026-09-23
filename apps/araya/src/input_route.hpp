#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// The pure input routing shared by the TUI surfaces: how one submitted
// line is classified, and the role markers the feed renders. Header-only
// and dependency-free so it unit-tests without the app or any plugin.
namespace araya::app {

enum class input_kind : std::uint8_t {
	empty,	 // nothing but whitespace
	command, // the first token names a command the surface knows
	message, // free text: a conversation line
};

// Classifies one submitted line: empty for whitespace only, command when
// the first token satisfies `is_known_command`, message otherwise. Lines
// are trimmed first, so leading/trailing spaces do not matter.
inline input_kind classify_input(std::string_view line, std::function<bool(std::string_view)> const& is_known_command) {
	auto first = line.find_first_not_of(" \t");
	if (first == std::string_view::npos)
		return input_kind::empty;
	auto last = line.find_last_not_of(" \t");
	line = line.substr(first, last - first + 1);

	auto end = line.find_first_of(" \t");
	auto name = line.substr(0, end);
	return is_known_command(name) ? input_kind::command : input_kind::message;
}

// The feed's per-role prefix. The unicode tier uses the opencode-style
// markers; the ASCII tier substitutes plain characters.
inline std::string_view role_marker(std::string_view role, bool ascii) {
	if (role == "assistant")
		return ascii ? "*" : "\u25cf"; // ●
	if (role == "system")
		return ascii ? "~" : "\u25cb"; // ○
	if (role == "tool")
		return ascii ? "#" : "\u2699"; // ⚙
	// user (the default)
	return ascii ? ">" : "\u276f"; // ❯
}

// A short display title for a conversation: the first user message with
// whitespace collapsed and truncated (on a codepoint boundary) to
// k_title_max bytes, or the fallback (the session id) when there is no
// user text. Derived, not persisted.
inline std::string session_title(std::string_view first_user_text, std::string_view fallback) {
	constexpr std::size_t k_title_max = 48;
	std::string collapsed;
	bool pending_space = false;
	for (unsigned char c : first_user_text) {
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			pending_space = !collapsed.empty();
			continue;
		}
		if (pending_space) {
			collapsed.push_back(' ');
			pending_space = false;
		}
		collapsed.push_back(static_cast<char>(c));
	}
	if (collapsed.empty())
		return std::string(fallback);
	if (collapsed.size() > k_title_max) {
		std::size_t cut = k_title_max;
		// Never split a UTF-8 sequence: back off continuation bytes.
		while (cut > 0 && (static_cast<unsigned char>(collapsed[cut]) & 0xC0) == 0x80)
			--cut;
		collapsed.resize(cut);
		collapsed += "\u2026"; // …
	}
	return collapsed;
}

} // namespace araya::app
