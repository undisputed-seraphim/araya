#include "process_worker.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace araya::workflow {
namespace {

namespace bp = boost::process::v2;

} // namespace

process_worker::process_worker(
	boost::asio::any_io_executor executor,
	std::string node_executable,
	std::string source,
	std::size_t max_message_bytes)
	: executor_(std::move(executor))
	, decoder_(max_message_bytes) {
	// Boost.Process v2 launches with execve, which does not search PATH: a
	// bare executable name must be resolved here, or the launch fails ENOENT.
	bp::filesystem::path exe(node_executable);
	if (!exe.has_parent_path() && !exe.is_absolute()) {
		auto resolved = bp::environment::find_executable(exe);
		if (resolved.empty())
			throw std::runtime_error("'" + node_executable + "' was not found on PATH");
		exe = std::move(resolved);
	}
	in_.emplace(executor_);
	out_.emplace(executor_);
	err_.emplace(executor_);
	// A vector, not an initializer_list (whose elements are const, so the
	// move would copy the whole script).
	std::vector<std::string> args{"--eval", std::move(source)};
	process_.emplace(executor_, exe, std::move(args), bp::process_stdio{.in = *in_, .out = *out_, .err = *err_});
}

process_worker::~process_worker() { kill(); }

void process_worker::send(boost::json::value message) {
	if (killed_ || !in_)
		return;
	writes_.push_back(encode_frame(message));
	if (writing_)
		return;
	writing_ = true;
	auto self = shared_from_this();
	boost::asio::co_spawn(
		executor_, [self]() -> araya::task<void> { co_await self->do_write(); }, boost::asio::detached);
}

araya::task<void> process_worker::do_write() {
	while (!killed_ && !writes_.empty()) {
		boost::system::error_code ec;
		co_await boost::asio::async_write(
			*in_, boost::asio::buffer(writes_.front()), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
		if (ec) {
			killed_ = true;
			break;
		}
		writes_.pop_front();
	}
	writing_ = false;
	co_return;
}

araya::task<void> process_worker::pump(std::function<void(boost::json::value)> on_message) {
	if (!out_)
		co_return;
	// Drain stderr so a chatty child cannot block on a full pipe.
	auto self = shared_from_this();
	boost::asio::co_spawn(
		executor_,
		[self]() -> araya::task<void> {
			std::array<char, 4096> buffer{};
			for (;;) {
				boost::system::error_code ec;
				auto const read = co_await self->err_->async_read_some(
					boost::asio::buffer(buffer), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
				if (ec)
					break;
				if (self->stderr_tail_.size() < 8192) {
					auto const keep = std::min<std::size_t>(read, 8192 - self->stderr_tail_.size());
					self->stderr_tail_.append(buffer.data(), keep);
				}
			}
		},
		boost::asio::detached);

	std::array<char, 8192> buffer{};
	for (;;) {
		boost::system::error_code ec;
		auto const read = co_await out_->async_read_some(
			boost::asio::buffer(buffer), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
		if (ec)
			break;
		bool ok = true;
		try {
			for (auto& message : decoder_.feed(std::string_view(buffer.data(), read)))
				on_message(std::move(message));
		} catch (std::exception const&) {
			ok = false;
		}
		if (!ok)
			break;
	}
	co_return;
}

void process_worker::kill() {
	if (killed_)
		return;
	killed_ = true;
	if (process_) {
		boost::system::error_code ec;
		process_->terminate(ec);
	}
	boost::system::error_code ec;
	if (in_)
		in_->close(ec);
	if (out_)
		out_->close(ec);
	if (err_)
		err_->close(ec);
}

} // namespace araya::workflow
