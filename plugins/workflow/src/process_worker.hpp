#pragma once

#include "araya/workflow/workflow.hpp"
#include "protocol.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/process/v2/process.hpp>

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>

namespace araya::workflow {

// The real out-of-process worker: a Node child spoken to over framed
// stdin/stdout, with a drained stderr tail for diagnostics.
class process_worker : public worker, public std::enable_shared_from_this<process_worker> {
public:
	process_worker(
		boost::asio::any_io_executor executor,
		std::string node_executable,
		std::string source,
		std::size_t max_message_bytes);
	~process_worker() override;

	void send(boost::json::value message) override;
	araya::task<void> pump(std::function<void(boost::json::value)> on_message) override;
	void kill() override;

	// The captured stderr tail, for a failure diagnostic. Reserved API: the
	// worker reports failures through the protocol, so no caller reads it yet.
	[[maybe_unused]] std::string const& stderr_tail() const noexcept { return stderr_tail_; }

private:
	araya::task<void> do_write();

	boost::asio::any_io_executor executor_;
	std::optional<boost::asio::writable_pipe> in_;
	std::optional<boost::asio::readable_pipe> out_;
	std::optional<boost::asio::readable_pipe> err_;
	std::optional<boost::process::v2::process> process_;
	frame_decoder decoder_;
	std::deque<std::string> writes_;
	bool writing_ = false;
	bool killed_ = false;
	std::string stderr_tail_;
};

} // namespace araya::workflow
