#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Small shared string helpers that would otherwise be copy-pasted across
// plugins: whitespace trim, the two XML escapes used when embedding model
// text into the prompt frames, and the fixed 16-digit hex formatter used for
// opaque ids.
namespace araya::util::string {

inline std::string trim(std::string_view text) {
	auto const first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos)
		return {};
	auto const last = text.find_last_not_of(" \t\r\n");
	return std::string(text.substr(first, last - first + 1));
}

// Escape element text: &, <, >.
inline std::string xml_escape(std::string_view value) {
	std::string out;
	out.reserve(value.size());
	for (char const c : value) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		default:
			out.push_back(c);
			break;
		}
	}
	return out;
}

// Escape a quoted attribute value: &, ", <.
inline std::string xml_attr_escape(std::string_view value) {
	std::string out;
	out.reserve(value.size());
	for (char const c : value) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '"':
			out += "&quot;";
			break;
		case '<':
			out += "&lt;";
			break;
		default:
			out.push_back(c);
			break;
		}
	}
	return out;
}

// The low 64 bits as exactly 16 lowercase hex digits (zero padded).
inline std::string hex16(std::uint64_t value) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string out(16, '0');
	for (int i = 15; i >= 0; --i) {
		out[static_cast<std::size_t>(i)] = digits[value & 0xF];
		value >>= 4;
	}
	return out;
}

} // namespace araya::util::string
