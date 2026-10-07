#pragma once

#include "araya/task.hpp"

#include <optional>
#include <string>
#include <string_view>

// A framed message channel to one MCP server: the connection layer owns
// protocol/ids; the channel owns bytes. `write` enqueues one already-encoded
// frame (single-line JSON); `read` yields the next frame or nullopt at EOF.
// All calls run on the owning strand.
namespace araya::mcp {

class message_channel {
public:
	virtual ~message_channel() = default;

	virtual void write(std::string frame) = 0;
	virtual araya::task<std::optional<std::string>> read() = 0;
	virtual void close() = 0;

	// The tail of the child's stderr, for diagnostics on failure.
	virtual std::string stderr_tail() const = 0;
};

} // namespace araya::mcp
