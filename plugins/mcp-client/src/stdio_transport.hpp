#pragma once

#include "config.hpp"
#include "transport.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <memory>

namespace araya::mcp {

// Spawns the configured stdio server and returns a connected channel. Throws
// std::runtime_error when the process cannot be started.
std::shared_ptr<message_channel> launch_stdio(boost::asio::any_io_executor executor, server_config const& config);

} // namespace araya::mcp
