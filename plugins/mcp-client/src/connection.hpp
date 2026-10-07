#pragma once

#include "araya/task.hpp"
#include "config.hpp"
#include "transport.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace araya::mcp {

// One discovered tool, as the server advertised it.
struct mcp_tool {
	std::string name;
	std::string description;
	boost::json::value input_schema;
};

// What the owning plugin must be told when a connection's state changes. Both
// callbacks run on the owning strand.
struct connection_observer {
	// A fresh tool generation after a successful (re)discovery.
	std::function<void(std::vector<mcp_tool> const&)> on_tools;
	// The server's instructions after a successful (re)connection.
	std::function<void(std::string const&)> on_instructions;
};

// One server connection: a supervisor that establishes the transport, runs the
// initialize handshake, discovers tools, services requests, and reconnects on
// loss. A dedicated reader coroutine resolves in-flight requests and raises
// resync/lost signals; the manager performs setup and waits on those signals.
// All methods run on the owning strand.
class server_connection : public std::enable_shared_from_this<server_connection> {
public:
	server_connection(boost::asio::any_io_executor executor, server_config config, connection_observer observer);

	// Spawns the supervisor. Follow with ready() to observe the first attempt.
	void start();

	// Completes with the outcome of the initial connection+discovery (null on
	// success). Safe to call once, after start().
	araya::task<std::exception_ptr> ready();

	// Runs one MCP method and returns the protocol result. Throws on a
	// JSON-RPC error, timeout, cancellation, or a lost connection.
	araya::task<boost::json::value> request(std::string_view method, boost::json::value params, std::stop_token stop);

	// Requests shutdown: stops reconnecting and closes the transport.
	void stop();

	// The latest successfully connected instructions (empty when none).
	std::string instructions() const { return instructions_; }

	server_config const& config() const noexcept { return config_; }

private:
	struct rpc_response {
		bool is_error = false;
		boost::json::value value;
	};
	using response_channel = boost::asio::experimental::channel<void(boost::system::error_code, rpc_response)>;
	using ready_channel = boost::asio::experimental::channel<void(boost::system::error_code, std::exception_ptr)>;
	using signal_channel = boost::asio::experimental::channel<void(boost::system::error_code)>;

	araya::task<void> run();
	araya::task<void> reader();
	void open_session();
	araya::task<void> handshake();
	araya::task<void> discover();
	araya::task<boost::json::value> await_response(std::shared_ptr<response_channel> channel, std::stop_token stop);
	araya::task<void> delay(std::uint64_t milliseconds);
	void fail_pending();

	boost::asio::any_io_executor executor_;
	server_config config_;
	connection_observer observer_;
	std::shared_ptr<message_channel> channel_;
	std::int64_t next_id_ = 1;
	std::map<std::string, std::shared_ptr<response_channel>> pending_;
	std::string instructions_;
	std::vector<std::string> tool_names_;
	bool supports_tools_ = false;
	bool stopped_ = false;
	bool ready_sent_ = false;
	std::shared_ptr<signal_channel> wake_;
	bool session_lost_ = false;
	bool resync_queued_ = false;
	ready_channel ready_;
};

} // namespace araya::mcp
