#pragma once

#include "araya/lsp/lsp.hpp"
#include "araya/task.hpp"
#include "config.hpp"
#include "connection.hpp"
#include "host.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>

#include <deque>
#include <exception>
#include <memory>
#include <stop_token>

namespace araya::lsp_stdio {

// One initialized language-server process for one canonical workspace:
// initialize handshake, serialized transient didOpen -> request -> didClose
// queries, and teardown.
class lsp_instance : public std::enable_shared_from_this<lsp_instance> {
public:
	lsp_instance(boost::asio::any_io_executor executor, server_config config, host_workspace workspace);

	// Starts the process and the initialize handshake. Call once, after the
	// instance is owned by a shared_ptr.
	void start();

	// Completes once the initialize handshake settles; throws on failure.
	araya::task<void> ready(std::stop_token stop);

	araya::task<araya::lsp::lsp_query_result>
	query(araya::lsp::lsp_provider_query const& request, host_source const& source, std::stop_token stop);

	// Begins teardown: reject further work and close the process.
	void close();

	bool dead() const;

private:
	using done_channel = boost::asio::experimental::channel<void(boost::system::error_code)>;
	using ready_channel = boost::asio::experimental::channel<void(boost::system::error_code, std::exception_ptr)>;

	araya::task<void> initialize();
	araya::task<void> lock();
	void unlock();

	boost::asio::any_io_executor executor_;
	server_config config_;
	host_workspace workspace_;
	std::shared_ptr<lsp_connection> connection_;
	boost::json::value capabilities_;
	ready_channel ready_;
	bool ready_done_ = false;
	std::exception_ptr ready_error_;
	bool disposed_ = false;
	bool locked_ = false;
	std::deque<std::shared_ptr<done_channel>> waiters_;
};

} // namespace araya::lsp_stdio
