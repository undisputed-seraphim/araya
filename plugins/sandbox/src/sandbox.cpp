#include "araya/sandbox/sandbox.hpp"

#include <algorithm>
#include <cctype>

namespace araya::sandbox {

std::string_view mode_name(sandbox_mode mode) noexcept {
	switch (mode) {
	case sandbox_mode::read_only:
		return "read-only";
	case sandbox_mode::workspace_write:
		return "workspace-write";
	case sandbox_mode::danger_full_access:
		return "danger-full-access";
	}
	return "danger-full-access";
}

std::optional<sandbox_mode> parse_mode(std::string_view name) noexcept {
	if (name == "read-only")
		return sandbox_mode::read_only;
	if (name == "workspace-write")
		return sandbox_mode::workspace_write;
	if (name == "danger-full-access")
		return sandbox_mode::danger_full_access;
	return std::nullopt;
}

bool matches_denial(int exit_code, std::string_view stderr_text, std::string_view signature) {
	if (exit_code == 0 || stderr_text.empty() || signature.empty())
		return false;
	auto const lower = [](char value) { return static_cast<char>(std::tolower(static_cast<unsigned char>(value))); };
	std::string haystack{stderr_text};
	std::transform(haystack.begin(), haystack.end(), haystack.begin(), lower);
	std::string needle{signature};
	std::transform(needle.begin(), needle.end(), needle.begin(), lower);
	return haystack.find(needle) != std::string::npos;
}

std::string denial_marker(std::string_view mode) {
	return "[sandbox: file access denied under " + std::string(mode) + " mode]";
}

} // namespace araya::sandbox
