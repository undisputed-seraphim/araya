#pragma once

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <unistd.h>

// Filesystem-seam source access for the stdio LSP provider: canonicalize a
// workspace, read one contained, byte-bounded UTF-8 source, and build file
// URIs. Local host only (std::filesystem).
namespace araya::lsp_stdio {

// Resolves a configured command to an executable path: an absolute/relative
// path is returned when executable, otherwise the first match on PATH.
// Returns nullopt when the command is not installed (so a default or
// configured server can be skipped gracefully).
inline std::optional<std::string> find_executable(std::string const& command) {
	if (command.empty())
		return std::nullopt;
	if (command.find('/') != std::string::npos)
		return ::access(command.c_str(), X_OK) == 0 ? std::optional<std::string>(command) : std::nullopt;
	char const* path = std::getenv("PATH");
	if (!path)
		return std::nullopt;
	std::string_view remaining{path};
	while (!remaining.empty()) {
		auto const colon = remaining.find(':');
		std::string_view const dir = remaining.substr(0, colon);
		std::string candidate = (dir.empty() ? std::string{"."} : std::string{dir}) + "/" + command;
		if (::access(candidate.c_str(), X_OK) == 0)
			return candidate;
		if (colon == std::string_view::npos)
			break;
		remaining.remove_prefix(colon + 1);
	}
	return std::nullopt;
}

struct host_workspace {
	std::string canonical_path;
	std::string file_url;
};

struct host_source {
	std::string file_url;
	std::string text;
};

inline std::string percent_encode(std::string_view text) {
	static char const* hex = "0123456789ABCDEF";
	std::string out;
	out.reserve(text.size());
	for (unsigned char const c : text) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
			c == '.' || c == '~' || c == '/')
			out.push_back(static_cast<char>(c));
		else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0x0F]);
		}
	}
	return out;
}

inline std::string file_url_from_path(std::string const& path) { return "file://" + percent_encode(path); }

// Resolve `root` to a canonical directory. Returns false with a message on a
// missing/non-directory root.
inline bool canonicalize_workspace(std::string const& root, host_workspace& out, std::string& error) {
	std::error_code ec;
	std::filesystem::path const canonical = std::filesystem::weakly_canonical(root, ec);
	if (ec) {
		error = "workspace root \"" + root + "\" cannot be resolved: " + ec.message();
		return false;
	}
	if (!std::filesystem::is_directory(canonical, ec)) {
		error = "workspace root \"" + root + "\" is not a directory";
		return false;
	}
	out.canonical_path = canonical.string();
	out.file_url = file_url_from_path(out.canonical_path);
	return true;
}

// Read one source file: resolve against the workspace, require containment and
// a regular file, and bound the byte size. Returns false with a message.
inline bool read_host_source(
	std::string const& file_path,
	host_workspace const& workspace,
	std::size_t max_document_bytes,
	host_source& out,
	std::string& error) {
	std::error_code ec;
	std::filesystem::path path(file_path);
	if (path.is_relative())
		path = std::filesystem::path(workspace.canonical_path) / path;
	std::filesystem::path const canonical = std::filesystem::weakly_canonical(path, ec);
	if (ec) {
		error = "source \"" + file_path + "\" cannot be resolved: " + ec.message();
		return false;
	}
	auto const root = std::filesystem::path(workspace.canonical_path);
	auto const relative = canonical.lexically_relative(root);
	if (relative.empty() || relative.native().rfind("..", 0) == 0)
		if (canonical != root) {
			error = "source \"" + file_path + "\" resolves outside the workspace";
			return false;
		}
	if (!std::filesystem::is_regular_file(canonical, ec)) {
		error = "source \"" + file_path + "\" is not a regular file";
		return false;
	}
	auto const size = std::filesystem::file_size(canonical, ec);
	if (ec) {
		error = "source \"" + file_path + "\" could not be read: " + ec.message();
		return false;
	}
	if (size > max_document_bytes) {
		error = "source \"" + file_path + "\" exceeds the " + std::to_string(max_document_bytes) + "-byte limit";
		return false;
	}
	std::ifstream file(canonical, std::ios::binary);
	if (!file) {
		error = "source \"" + file_path + "\" could not be opened";
		return false;
	}
	std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	out.text = std::move(text);
	out.file_url = file_url_from_path(canonical.string());
	return true;
}

} // namespace araya::lsp_stdio
