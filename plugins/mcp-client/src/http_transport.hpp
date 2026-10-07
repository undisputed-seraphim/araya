#pragma once

#include "config.hpp"
#include "transport.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <memory>

namespace araya::mcp {

// Creates a Streamable HTTP message channel for a `remote` server. Each write
// is an HTTP POST; the response body is either one JSON-RPC message
// (`application/json`) or an SSE stream of messages (`text/event-stream`).
// Throws std::invalid_argument on a malformed URL.
std::shared_ptr<message_channel> launch_http(boost::asio::any_io_executor executor, server_config const& config);

} // namespace araya::mcp
