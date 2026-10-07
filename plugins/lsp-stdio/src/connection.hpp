#pragma once

#include "araya/task.hpp"
#include "config.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/json/value.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace araya::lsp_stdio {

// A JSON-RPC endpoint over one language server process, framed with
// Content-Length. `start()` launches the child and the reader; `request`/
// `notify` run on the owning strand. A framing/write failure or process exit
// fails every pending request and marks the connection unusable.
class lsp_connection : public std::enable_shared_from_this<lsp_connection> {
public:
	lsp_connection(boost::asio::any_io_executor executor, server_config config);

	// Launches the process and starts reading. Throws on spawn failure.
	void start();

	araya::task<boost::json::value> request(std::string method, boost::json::value params, std::stop_token stop);
	araya::task<void> notify(std::string method, boost::json::value params);

	// Best-effort teardown: close stdin, SIGTERM, then SIGKILL after the grace.
	void close();

	bool failed() const noexcept { return failed_; }
	std::string stderr_tail() const { return stderr_tail_; }

private:
	struct rpc_response {
		bool is_error = false;
		boost::json::value value;
		std::string error;
	};
	using response_channel = boost::asio::experimental::channel<void(boost::system::error_code, rpc_response)>;
	using done_channel = boost::asio::experimental::channel<void(boost::system::error_code)>;
	struct write_item {
		std::string frame;
		std::shared_ptr<done_channel> done;
	};

	araya::task<void> reader();
	araya::task<void> drain_writes();
	araya::task<void> drain_stderr();
	araya::task<std::optional<boost::json::value>> read_frame();
	araya::task<boost::json::value> await_response(std::shared_ptr<response_channel> channel, std::stop_token stop);
	araya::task<void> write(boost::json::value message);
	void dispatch(boost::json::value const& message);
	void
	answer_server_request(boost::json::value const& id, std::string const& method, boost::json::value const& params);
	void fail(std::string reason);

	boost::asio::any_io_executor executor_;
	server_config config_;
	boost::asio::readable_pipe out_;
	boost::asio::writable_pipe in_;
	boost::asio::readable_pipe err_;
	std::optional<boost::process::v2::process> process_;
	std::deque<write_item> writes_;
	bool writing_ = false;
	bool closed_ = false;
	bool failed_ = false;
	std::string failure_;
	std::string stderr_tail_;
	std::string read_buffer_;
	std::map<std::int64_t, std::shared_ptr<response_channel>> pending_;
	std::int64_t next_id_ = 1;
};

} // namespace araya::lsp_stdio
