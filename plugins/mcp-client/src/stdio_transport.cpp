#include "stdio_transport.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/write.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>

#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::mcp {
namespace {

namespace bp = boost::process::v2;
using boost::asio::awaitable;
using boost::asio::readable_pipe;
using boost::asio::writable_pipe;

std::string env_key(std::string_view entry) {
	auto const eq = entry.find('=');
	return std::string(entry.substr(0, eq == std::string_view::npos ? entry.size() : eq));
}

// Resolves a bare program name against PATH. Necessary because passing an
// explicit environment to the launcher skips its own PATH search (it execs the
// path directly). A name containing a separator is used verbatim.
std::string resolve_program(std::string const& program) {
	if (program.find('/') != std::string::npos)
		return program;
	char const* path = std::getenv("PATH");
	if (!path)
		return program;
	std::string_view remaining{path};
	while (!remaining.empty()) {
		auto const colon = remaining.find(':');
		std::string_view const dir = remaining.substr(0, colon);
		std::string candidate = (dir.empty() ? std::string{"."} : std::string{dir}) + "/" + program;
		if (::access(candidate.c_str(), X_OK) == 0)
			return candidate;
		if (colon == std::string_view::npos)
			break;
		remaining.remove_prefix(colon + 1);
	}
	return program;
}

// The child environment: the ambient environment with credential-shaped and
// ARAYA_* names dropped, plus the configured overrides verbatim (so an
// explicit override always survives the scrub).
std::vector<std::string> build_child_env(std::vector<std::pair<std::string, std::string>> const& extra) {
	std::vector<std::string> env;
	for (char** it = environ; it && *it; ++it) {
		std::string_view entry{*it};
		if (sensitive_env_name(env_key(entry)))
			continue;
		env.emplace_back(entry);
	}
	for (auto const& [key, value] : extra)
		env.push_back(key + "=" + value);
	return env;
}

// One stdio child: writable stdin and readable stdout/stderr pipes, with a
// single-writer drain (multiple callers enqueue frames; the strand serializes
// them) and a bounded stderr tail for diagnostics.
class stdio_channel : public message_channel, public std::enable_shared_from_this<stdio_channel> {
public:
	explicit stdio_channel(boost::asio::any_io_executor executor)
		: executor_(std::move(executor))
		, out_(executor_)
		, in_(executor_)
		, err_(executor_) {}

	void start(server_config const& config) {
		std::vector<std::string> args(config.command.begin() + 1, config.command.end());
		std::string const program = resolve_program(config.command.front());
		auto io = bp::process_stdio{.in = in_, .out = out_, .err = err_};
		auto env = build_child_env(config.environment);
		try {
			if (!config.cwd.empty()) {
				process_.emplace(
					executor_,
					program,
					std::move(args),
					io,
					bp::process_start_dir(config.cwd),
					bp::process_environment(std::move(env)));
			} else {
				process_.emplace(executor_, program, std::move(args), io, bp::process_environment(std::move(env)));
			}
		} catch (std::exception const& e) {
			throw std::runtime_error("mcp: cannot start '" + config.command.front() + "': " + std::string(e.what()));
		}
		boost::asio::co_spawn(executor_, drain_stderr(), boost::asio::detached);
	}

	void write(std::string frame) override {
		if (closed_)
			return;
		frame.push_back('\n');
		queue_.push_back(std::move(frame));
		if (!writing_) {
			writing_ = true;
			auto self = shared_from_this();
			boost::asio::co_spawn(executor_, [self] { return self->drain_writes(); }, boost::asio::detached);
		}
	}

	araya::task<std::optional<std::string>> read() override {
		for (;;) {
			if (auto const newline = buffer_.find('\n'); newline != std::string::npos) {
				std::string line = buffer_.substr(0, newline);
				buffer_.erase(0, newline + 1);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty())
					continue;
				co_return line;
			}
			std::array<char, 8192> chunk{};
			boost::system::error_code ec;
			auto const read = co_await out_.async_read_some(
				boost::asio::buffer(chunk), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			if (ec || read == 0) {
				if (buffer_.empty())
					co_return std::nullopt;
				std::string line = std::move(buffer_);
				buffer_.clear();
				co_return line;
			}
			buffer_.append(chunk.data(), read);
			if (buffer_.size() > max_frame_)
				throw std::runtime_error("mcp: incoming frame exceeds the size limit");
		}
	}

	void close() override {
		if (closed_)
			return;
		closed_ = true;
		boost::system::error_code ec;
		in_.close(ec);
		out_.close(ec);
		err_.close(ec);
		if (process_) {
			process_->terminate(ec);
			auto process = std::make_shared<bp::process>(std::move(*process_));
			process_.reset();
			boost::asio::co_spawn(
				executor_,
				[process]() -> awaitable<void> {
					boost::system::error_code wait_ec;
					co_await process->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_ec));
				},
				boost::asio::detached);
		}
	}

	std::string stderr_tail() const override { return stderr_tail_; }

private:
	araya::task<void> drain_writes() {
		while (!queue_.empty()) {
			std::string frame = std::move(queue_.front());
			queue_.pop_front();
			boost::system::error_code ec;
			co_await boost::asio::async_write(
				in_, boost::asio::buffer(frame), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			if (ec || closed_)
				break;
		}
		writing_ = false;
	}

	araya::task<void> drain_stderr() {
		std::array<char, 4096> chunk{};
		for (;;) {
			boost::system::error_code ec;
			auto const read = co_await err_.async_read_some(
				boost::asio::buffer(chunk), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			if (ec || read == 0)
				co_return;
			append_stderr(chunk.data(), read);
		}
	}

	void append_stderr(char const* data, std::size_t size) {
		static constexpr std::size_t cap = 4096;
		stderr_tail_.append(data, size);
		if (stderr_tail_.size() > cap)
			stderr_tail_.erase(0, stderr_tail_.size() - cap);
	}

	boost::asio::any_io_executor executor_;
	readable_pipe out_;
	writable_pipe in_;
	readable_pipe err_;
	std::optional<bp::process> process_;
	std::deque<std::string> queue_;
	bool writing_ = false;
	bool closed_ = false;
	std::string buffer_;
	std::string stderr_tail_;
	std::size_t max_frame_ = 8 * 1024 * 1024;
};

} // namespace

std::shared_ptr<message_channel> launch_stdio(boost::asio::any_io_executor executor, server_config const& config) {
	if (config.command.empty())
		throw std::runtime_error("mcp: server '" + config.name + "' has no command");
	auto channel = std::make_shared<stdio_channel>(std::move(executor));
	channel->start(config);
	return channel;
}

} // namespace araya::mcp
