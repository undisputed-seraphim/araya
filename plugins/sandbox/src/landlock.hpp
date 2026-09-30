#pragma once

#include <cstdint>
#include <string>

namespace araya::sandbox::detail {

// The one-time host probe: whether the kernel enforces Landlock, the ABI it
// reports, and (when unusable) why.
struct probe_result {
	bool available = false;
	std::uint32_t abi = 0;
	std::string reason;
};

probe_result probe_landlock() noexcept;

} // namespace araya::sandbox::detail
