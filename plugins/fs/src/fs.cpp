#include "araya/fs/events.hpp"

#include <system_error>

namespace araya::fs {

std::optional<fs_version> read_version(std::filesystem::path const& path) {
	std::error_code ec;
	if (!std::filesystem::is_regular_file(path, ec) || ec)
		return std::nullopt;
	auto const mtime = std::filesystem::last_write_time(path, ec);
	if (ec)
		return std::nullopt;
	auto const size = std::filesystem::file_size(path, ec);
	if (ec)
		return std::nullopt;
	return std::to_string(mtime.time_since_epoch().count()) + ":" + std::to_string(size);
}

} // namespace araya::fs
