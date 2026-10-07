#include "connection.hpp"

#include "araya/lsp/lsp.hpp"
#include "framing.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::lsp_stdio {
namespace {

namespace bp = boost::process::v2;

std::string env_key(std::string_view entry) {
	auto const eq = entry.find('=');
	return std::string(entry.substr(0, eq == std::string_view::npos ? entry.size() : eq));
}

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

} // namespace

lsp_connection::lsp_connection(boost::asio::any_io_executor executor, server_config config)
	: executor_(std::move(executor))
	, config_(std::move(config))
	, out_(executor_)
	, in_(executor_)
	, err_(executor_) {}

void lsp_connection::start() {
	std::string const program = resolve_program(config_.command);
	auto io = bp::process_stdio{.in = in_, .out = out_, .err = err_};
	auto env = build_child_env(config_.env);
	try {
		process_.emplace(
			executor_,
			program,
			config_.args,
			io,
			bp::process_start_dir(bp::filesystem::path(config_.cwd)),
			bp::process_environment(std::move(env)));
	} catch (std::exception const& e) {
		throw std::runtime_error("lsp-stdio: cannot start '" + config_.command + "': " + std::string(e.what()));
	}
	boost::asio::co_spawn(executor_, [self = shared_from_this()] { return self->reader(); }, boost::asio::detached);
	boost::asio::co_spawn(
		executor_, [self = shared_from_this()] { return self->drain_stderr(); }, boost::asio::detached);
}

araya::task<void> lsp_connection::write(boost::json::value message) {
	auto done = std::make_shared<done_channel>(executor_, 1);
	writes_.push_back(write_item{encode_message(message), done});
	if (!writing_) {
		writing_ = true;
		auto self = shared_from_this();
		boost::asio::co_spawn(executor_, [self] { return self->drain_writes(); }, boost::asio::detached);
	}
	boost::system::error_code ec;
	co_await done->async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
	if (ec || failed_)
		throw std::runtime_error(failure_.empty() ? "lsp-stdio: write failed" : failure_);
}

araya::task<void> lsp_connection::drain_writes() {
	while (!writes_.empty()) {
		write_item item = std::move(writes_.front());
		writes_.pop_front();
		boost::system::error_code ec;
		co_await boost::asio::async_write(
			in_, boost::asio::buffer(item.frame), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
		if (item.done)
			item.done->try_send(ec);
		if (ec) {
			fail("lsp-stdio: write to the language server failed");
			break;
		}
	}
	writing_ = false;
}

araya::task<void> lsp_connection::drain_stderr() {
	std::array<char, 4096> chunk{};
	for (;;) {
		boost::system::error_code ec;
		auto const read = co_await err_.async_read_some(
			boost::asio::buffer(chunk), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
		if (ec || read == 0)
			co_return;
		stderr_tail_.append(chunk.data(), read);
		if (stderr_tail_.size() > config_.max_stderr_bytes)
			stderr_tail_.erase(0, stderr_tail_.size() - config_.max_stderr_bytes);
	}
}

araya::task<void> lsp_connection::reader() {
	message_decoder decoder(config_.max_message_bytes);
	std::array<char, 8192> chunk{};
	for (;;) {
		if (closed_)
			co_return;
		boost::system::error_code ec;
		auto const read = co_await out_.async_read_some(
			boost::asio::buffer(chunk), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
		if (ec || read == 0) {
			if (!closed_)
				fail("language server exited" + (stderr_tail_.empty() ? std::string{} : "; stderr: " + stderr_tail_));
			co_return;
		}
		std::vector<boost::json::value> messages;
		try {
			messages = decoder.push(std::string_view(chunk.data(), read));
		} catch (std::exception const& e) {
			fail(e.what());
			close();
			co_return;
		}
		for (auto& message : messages)
			dispatch(message);
	}
}

void lsp_connection::dispatch(boost::json::value const& message) {
	if (!message.is_object())
		return;
	auto const& object = message.as_object();
	auto const* method = object.if_contains("method");
	auto const* id = object.if_contains("id");
	auto const* params = object.if_contains("params");
	if (method && method->is_string() && id) {
		answer_server_request(*id, std::string(method->as_string()), params ? *params : boost::json::value{});
		return;
	}
	if (method)
		return; // server notification: ignored by this host
	if (!id)
		return;
	std::int64_t numeric = 0;
	if (id->is_int64())
		numeric = id->as_int64();
	else if (id->is_uint64())
		numeric = static_cast<std::int64_t>(id->as_uint64());
	else
		return;
	auto it = pending_.find(numeric);
	if (it == pending_.end())
		return;
	auto channel = it->second;
	pending_.erase(it);
	rpc_response response;
	if (auto const* error = object.if_contains("error")) {
		response.is_error = true;
		if (auto const* error_object = error->if_object()) {
			if (auto const* text = error_object->if_contains("message"); text && text->is_string())
				response.error = std::string(text->as_string());
		}
		if (response.error.empty())
			response.error = "LSP error response";
	} else if (auto const* result = object.if_contains("result")) {
		response.value = *result;
	}
	channel->try_send(boost::system::error_code{}, std::move(response));
}

void lsp_connection::answer_server_request(
	boost::json::value const& id,
	std::string const& method,
	boost::json::value const& params) {
	boost::json::object response;
	response["jsonrpc"] = "2.0";
	response["id"] = id;
	if (method == "workspace/configuration") {
		boost::json::array items;
		if (auto const* object = params.if_object()) {
			if (auto const* requested = object->if_contains("items"); requested && requested->is_array()) {
				for (std::size_t i = 0; i < requested->as_array().size(); ++i)
					items.push_back(config_.configuration);
			}
		}
		response["result"] = std::move(items);
	} else if (
		method == "window/workDoneProgress/create" || method == "client/registerCapability" ||
		method == "client/unregisterCapability") {
		response["result"] = nullptr;
	} else {
		boost::json::object error;
		error["code"] = -32601;
		error["message"] = method == "workspace/applyEdit" ? "workspace/applyEdit is not permitted by this host"
														   : "unsupported server request: " + method;
		response["error"] = std::move(error);
	}
	auto done = std::make_shared<done_channel>(executor_, 1);
	writes_.push_back(write_item{encode_message(boost::json::value(std::move(response))), done});
	if (!writing_) {
		writing_ = true;
		auto self = shared_from_this();
		boost::asio::co_spawn(executor_, [self] { return self->drain_writes(); }, boost::asio::detached);
	}
}

araya::task<boost::json::value>
lsp_connection::request(std::string method, boost::json::value params, std::stop_token stop) {
	if (failed_)
		throw std::runtime_error(failure_.empty() ? "lsp-stdio: connection failed" : failure_);
	if (stop.stop_requested())
		throw araya::lsp::lsp_error("LSP query was cancelled", std::string(araya::lsp::error_code::disposed));
	auto const id = next_id_++;
	auto channel = std::make_shared<response_channel>(executor_, 1);
	pending_[id] = channel;
	boost::json::object message;
	message["jsonrpc"] = "2.0";
	message["id"] = id;
	message["method"] = method;
	message["params"] = params;
	boost::asio::co_spawn(
		executor_,
		[self = shared_from_this(), message = boost::json::value(std::move(message))]() mutable {
			return self->write(std::move(message));
		},
		boost::asio::detached);
	try {
		boost::json::value result = co_await await_response(channel, stop);
		pending_.erase(id);
		co_return result;
	} catch (...) {
		pending_.erase(id);
		throw;
	}
}

araya::task<void> lsp_connection::notify(std::string method, boost::json::value params) {
	boost::json::object message;
	message["jsonrpc"] = "2.0";
	message["method"] = method;
	message["params"] = params;
	co_await write(boost::json::value(std::move(message)));
}

araya::task<boost::json::value>
lsp_connection::await_response(std::shared_ptr<response_channel> channel, std::stop_token stop) {
	using namespace boost::asio::experimental::awaitable_operators;
	boost::asio::steady_timer timer(executor_);
	timer.expires_after(std::chrono::milliseconds(config_.request_timeout_ms));
	std::stop_callback cancel(
		stop, [executor = executor_, channel] { boost::asio::post(executor, [channel] { channel->close(); }); });
	auto outcome =
		co_await (channel->async_receive(boost::asio::use_awaitable) || timer.async_wait(boost::asio::use_awaitable));
	if (outcome.index() == 1)
		throw std::runtime_error(
			"lsp-stdio: request to '" + config_.id + "' timed out after " + std::to_string(config_.request_timeout_ms) +
			" ms");
	rpc_response response = std::get<0>(std::move(outcome));
	if (response.is_error)
		throw std::runtime_error(response.error);
	co_return std::move(response.value);
}

void lsp_connection::fail(std::string reason) {
	if (!failed_) {
		failed_ = true;
		failure_ = std::move(reason);
	}
	for (auto& [id, channel] : pending_)
		channel->close();
	pending_.clear();
}

void lsp_connection::close() {
	if (closed_)
		return;
	closed_ = true;
	boost::system::error_code ec;
	in_.close(ec);
	out_.close(ec);
	err_.close(ec);
	if (process_) {
		process_->request_exit(ec);
		auto process = std::make_shared<bp::process>(std::move(*process_));
		process_.reset();
		auto const grace = config_.kill_grace_ms;
		boost::asio::co_spawn(
			executor_,
			[process, grace]() -> araya::task<void> {
				boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
				timer.expires_after(std::chrono::milliseconds(grace));
				boost::system::error_code timer_ec;
				co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, timer_ec));
				boost::system::error_code term_ec;
				process->terminate(term_ec);
				co_await process->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, term_ec));
			},
			boost::asio::detached);
	}
}

} // namespace araya::lsp_stdio
