#include "connection.hpp"

#include "http_transport.hpp"
#include "jsonrpc.hpp"
#include "stdio_transport.hpp"

#include "araya/util/json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::mcp {
namespace {

constexpr char const* k_client_name = "araya";
constexpr char const* k_client_version = "1.0";
constexpr char const* k_protocol_version = "2025-06-18";

std::uint64_t backoff_ms(reconnect_config const& policy, std::uint64_t attempt) {
	std::uint64_t delay = policy.initial_delay_ms;
	for (std::uint64_t i = 1; i < attempt && delay < policy.max_delay_ms; ++i)
		delay *= 2;
	return std::min(delay, policy.max_delay_ms);
}

} // namespace

server_connection::server_connection(
	boost::asio::any_io_executor executor,
	server_config config,
	connection_observer observer)
	: executor_(std::move(executor))
	, config_(std::move(config))
	, observer_(std::move(observer))
	, wake_(std::make_shared<signal_channel>(executor_, 1))
	, ready_(executor_, 1) {}

void server_connection::start() {
	auto self = shared_from_this();
	boost::asio::co_spawn(executor_, [self] { return self->run(); }, boost::asio::detached);
}

araya::task<std::exception_ptr> server_connection::ready() {
	co_return co_await ready_.async_receive(boost::asio::use_awaitable);
}

void server_connection::stop() {
	stopped_ = true;
	if (channel_)
		channel_->close();
}

araya::task<void> server_connection::run() {
	std::uint64_t attempt = 0;
	for (;;) {
		std::exception_ptr error;
		bool opened = false;
		try {
			open_session();
			opened = true;
		} catch (...) {
			error = std::current_exception();
		}

		if (opened) {
			auto self = shared_from_this();
			boost::asio::co_spawn(executor_, [self] { return self->reader(); }, boost::asio::detached);
			try {
				co_await handshake();
				co_await discover();
			} catch (...) {
				error = std::current_exception();
			}
		}

		if (!ready_sent_) {
			ready_sent_ = true;
			ready_.try_send(boost::system::error_code{}, error);
		}
		if (error)
			fail_pending();

		if (!opened) {
			if (stopped_ || !config_.reconnect.enabled)
				break;
			if (++attempt >= config_.reconnect.max_attempts)
				break;
			co_await delay(backoff_ms(config_.reconnect, attempt));
			continue;
		}

		// Reader is running: wait for loss, servicing any resync signal.
		for (;;) {
			boost::system::error_code ec;
			co_await wake_->async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			if (stopped_ || session_lost_)
				break;
			if (resync_queued_) {
				resync_queued_ = false;
				try {
					co_await discover();
				} catch (...) {
					// A failed re-sync keeps the previous generation registered.
				}
			}
		}

		if (stopped_)
			break;
		fail_pending();
		if (channel_)
			channel_->close();

		if (!config_.reconnect.enabled) {
			stopped_ = true;
			break;
		}
		if (error) {
			if (++attempt >= config_.reconnect.max_attempts) {
				stopped_ = true;
				break;
			}
		} else {
			attempt = 1;
		}
		co_await delay(backoff_ms(config_.reconnect, attempt));
	}
	stopped_ = true;
	if (channel_) {
		channel_->close();
		channel_.reset();
	}
}

void server_connection::open_session() {
	if (stopped_)
		throw std::runtime_error("mcp: connection stopped");
	if (config_.transport == "local")
		channel_ = launch_stdio(executor_, config_);
	else if (config_.transport == "remote")
		channel_ = launch_http(executor_, config_);
	else
		throw std::runtime_error("mcp: server '" + config_.name + "' has unknown type '" + config_.transport + "'");
	tool_names_.clear();
	instructions_.clear();
	session_lost_ = false;
	resync_queued_ = false;
	wake_ = std::make_shared<signal_channel>(executor_, 1);
}

araya::task<void> server_connection::reader() {
	try {
		for (;;) {
			if (stopped_)
				break;
			auto frame = co_await channel_->read();
			if (!frame)
				break;
			auto const message = jsonrpc::decode(*frame);
			if (!message)
				continue;
			if (message->has_id && !message->has_method) {
				auto it = pending_.find(message->id);
				if (it == pending_.end())
					continue;
				auto channel = it->second;
				pending_.erase(it);
				rpc_response response;
				response.is_error = message->is_error;
				response.value = message->is_error ? message->error : message->result;
				channel->try_send(boost::system::error_code{}, std::move(response));
			} else if (message->has_method && !message->has_id) {
				if (message->method == "notifications/tools/list_changed") {
					resync_queued_ = true;
					if (wake_)
						wake_->try_send(boost::system::error_code{});
				}
			}
		}
	} catch (...) {
		// Fall through to the loss signal.
	}
	session_lost_ = true;
	if (wake_)
		wake_->try_send(boost::system::error_code{});
}

araya::task<void> server_connection::handshake() {
	boost::json::object client;
	client["name"] = k_client_name;
	client["version"] = k_client_version;
	boost::json::object params;
	params["protocolVersion"] = k_protocol_version;
	params["capabilities"] = boost::json::object{};
	params["clientInfo"] = std::move(client);

	boost::json::value const result = co_await request("initialize", std::move(params), {});
	auto const* result_object = result.if_object();
	if (result_object) {
		if (auto const value = araya::util::json::opt_string(*result_object, "instructions"))
			instructions_ = *value;
		if (auto const* capabilities = result_object->if_contains("capabilities");
			capabilities && capabilities->is_object())
			supports_tools_ = capabilities->as_object().if_contains("tools") != nullptr;
	}
	if (instructions_.size() > config_.max_instruction_bytes)
		throw std::runtime_error(
			"mcp: server '" + config_.name + "' instructions exceed maxInstructionBytes (" +
			std::to_string(config_.max_instruction_bytes) + ")");
	if (observer_.on_instructions)
		observer_.on_instructions(instructions_);
	channel_->write(boost::json::serialize(jsonrpc::make_notification("notifications/initialized", nullptr)));
}

araya::task<void> server_connection::discover() {
	std::vector<mcp_tool> tools;
	if (supports_tools_) {
		std::string cursor;
		for (;;) {
			boost::json::object params;
			if (!cursor.empty())
				params["cursor"] = cursor;
			boost::json::value const result = co_await request("tools/list", std::move(params), {});
			auto const* result_object = result.if_object();
			if (result_object) {
				if (auto const* list = result_object->if_contains("tools"); list && list->is_array()) {
					for (auto const& entry : list->as_array()) {
						auto const* tool = entry.if_object();
						if (!tool)
							continue;
						mcp_tool parsed;
						if (auto const* name = tool->if_contains("name"); name && name->is_string())
							parsed.name = std::string(name->as_string());
						if (parsed.name.empty())
							continue;
						if (auto const* description = tool->if_contains("description");
							description && description->is_string())
							parsed.description = std::string(description->as_string());
						if (auto const* schema = tool->if_contains("inputSchema"))
							parsed.input_schema = *schema;
						else
							parsed.input_schema = boost::json::object{{"type", "object"}};
						tools.push_back(std::move(parsed));
					}
				}
				cursor = araya::util::json::get_string(*result_object, "nextCursor");
			} else {
				cursor.clear();
			}
			if (cursor.empty())
				break;
		}
	}

	tool_names_.clear();
	for (auto const& tool : tools)
		tool_names_.push_back(tool.name);
	if (observer_.on_tools)
		observer_.on_tools(tools);
}

araya::task<boost::json::value>
server_connection::request(std::string_view method, boost::json::value params, std::stop_token stop) {
	if (stopped_ || !channel_)
		throw std::runtime_error("mcp: server '" + config_.name + "' is not connected");
	if (stop.stop_requested())
		throw std::runtime_error("mcp: request to '" + config_.name + "' was aborted");
	auto const id = next_id_++;
	auto channel = std::make_shared<response_channel>(executor_, 1);
	std::string const key = std::to_string(id);
	pending_[key] = channel;
	channel_->write(boost::json::serialize(jsonrpc::make_request(id, method, std::move(params))));
	try {
		boost::json::value result = co_await await_response(channel, stop);
		pending_.erase(key);
		co_return result;
	} catch (...) {
		pending_.erase(key);
		throw;
	}
}

araya::task<boost::json::value>
server_connection::await_response(std::shared_ptr<response_channel> channel, std::stop_token stop) {
	using namespace boost::asio::experimental::awaitable_operators;
	boost::asio::steady_timer timer(executor_);
	timer.expires_after(std::chrono::milliseconds(config_.timeout_ms));
	std::stop_callback cancel(
		stop, [executor = executor_, channel] { boost::asio::post(executor, [channel] { channel->close(); }); });
	auto outcome =
		co_await (channel->async_receive(boost::asio::use_awaitable) || timer.async_wait(boost::asio::use_awaitable));
	if (outcome.index() == 1)
		throw std::runtime_error(
			"mcp: server '" + config_.name + "' timed out after " + std::to_string(config_.timeout_ms) + " ms");
	rpc_response response = std::get<0>(std::move(outcome));
	if (response.is_error)
		throw std::runtime_error("mcp: server '" + config_.name + "' error: " + jsonrpc::error_text(response.value));
	co_return std::move(response.value);
}

araya::task<void> server_connection::delay(std::uint64_t milliseconds) {
	while (milliseconds > 0 && !stopped_) {
		auto const slice = std::min<std::uint64_t>(milliseconds, 100);
		boost::asio::steady_timer timer(executor_);
		timer.expires_after(std::chrono::milliseconds(slice));
		co_await timer.async_wait(boost::asio::use_awaitable);
		milliseconds -= slice;
	}
}

void server_connection::fail_pending() {
	for (auto& [key, channel] : pending_)
		channel->close();
	pending_.clear();
}

} // namespace araya::mcp
