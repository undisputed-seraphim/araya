#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The pure input routing and command-palette helpers shared by the TUI
// surfaces. Header-only and dependency-free so it unit-tests without the
// app or any plugin.
namespace araya::app {

enum class input_kind : std::uint8_t {
	empty,	 // nothing but whitespace
	command, // the line starts with '/'
	message, // free text: a conversation line
};

// Classifies one submitted line: empty for whitespace only, command when
// the trimmed line starts with '/', message otherwise.
inline input_kind classify_input(std::string_view line) {
	auto first = line.find_first_not_of(" \t");
	if (first == std::string_view::npos)
		return input_kind::empty;
	return line[first] == '/' ? input_kind::command : input_kind::message;
}

// One parsed slash command: the name (without the leading '/') and the
// raw argument text after the first whitespace run.
struct parsed_command {
	std::string_view name;
	std::string_view args;
};

inline parsed_command parse_command(std::string_view line) {
	auto first = line.find_first_not_of(" \t");
	if (first == std::string_view::npos)
		return {};
	line.remove_prefix(first);
	if (!line.empty() && line.front() == '/')
		line.remove_prefix(1);
	auto space = line.find_first_of(" \t");
	if (space == std::string_view::npos)
		return {line, {}};
	auto args = line.substr(space);
	auto arg_first = args.find_first_not_of(" \t");
	return {line.substr(0, space), arg_first == std::string_view::npos ? std::string_view{} : args.substr(arg_first)};
}

// One palette entry. `usage` drives whether the command takes arguments
// (any usage longer than the bare name does); the palette shows name and
// summary.
struct command_info {
	std::string_view name;
	std::string_view usage;
	std::string_view summary;
};

inline bool takes_args(command_info const& command) { return command.usage != command.name; }

inline bool equals_ci(std::string_view a, std::string_view b) {
	if (a.size() != b.size())
		return false;
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	}
	return true;
}

inline bool starts_with_ci(std::string_view hay, std::string_view needle) {
	return hay.size() >= needle.size() && equals_ci(hay.substr(0, needle.size()), needle);
}

inline bool contains_ci(std::string_view hay, std::string_view needle) {
	if (needle.empty())
		return true;
	if (needle.size() > hay.size())
		return false;
	for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
		if (equals_ci(hay.substr(i, needle.size()), needle))
			return true;
	return false;
}

// The palette's filter: entries whose name starts with `query` first,
// then entries that merely contain it, both case-insensitive. An empty
// query matches every entry in order. Returns indices into `commands`.
inline std::vector<std::size_t> filter_commands(std::span<command_info const> commands, std::string_view query) {
	std::vector<std::size_t> prefix;
	std::vector<std::size_t> substring;
	for (std::size_t i = 0; i < commands.size(); ++i) {
		if (query.empty()) {
			prefix.push_back(i);
			continue;
		}
		auto name = commands[i].name;
		if (starts_with_ci(name, query))
			prefix.push_back(i);
		else if (contains_ci(name, query))
			substring.push_back(i);
	}
	prefix.insert(prefix.end(), substring.begin(), substring.end());
	return prefix;
}

// The palette's open condition: the input is a command token being typed
// (starts with '/', no whitespace yet). The query is the text after '/'.
inline bool palette_open(std::string_view input) {
	return !input.empty() && input.front() == '/' && input.find_first_of(" \t") == std::string_view::npos;
}

inline std::string_view palette_query(std::string_view input) {
	return input.empty() ? std::string_view{} : input.substr(1);
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
