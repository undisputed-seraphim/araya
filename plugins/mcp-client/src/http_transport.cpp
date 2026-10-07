#include "http_transport.hpp"

#include "araya/llm/http.hpp"
#include "araya/llm/sse.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
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

using araya::llm::http::endpoint;
using araya::llm::http::request;
using araya::llm::http::response;

constexpr std::size_t max_body = 8 * 1024 * 1024;
constexpr char const* protocol_version = "2025-06-18";

bool content_type_is_event_stream(response const& resp) {
	std::string type = araya::llm::http::header_value(resp.headers, "content-type");
	for (char& c : type)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return type.find("text/event-stream") != std::string::npos;
}

// One Streamable HTTP connection: writes become POSTs, responses are unpacked
// (single JSON or an SSE stream of messages) into an inbound queue the
// connection's reader drains. A transport failure closes the channel, which
// the connection treats as a lost session.
class http_channel : public message_channel, public std::enable_shared_from_this<http_channel> {
public:
	http_channel(
		boost::asio::any_io_executor executor,
		endpoint server,
		std::vector<std::pair<std::string, std::string>> headers,
		std::uint64_t timeout_ms)
		: executor_(std::move(executor))
		, server_(std::move(server))
		, headers_(std::move(headers))
		, timeout_ms_(timeout_ms)
		, wake_(executor_, 1) {}

	void write(std::string frame) override {
		if (closed_)
			return;
		auto self = shared_from_this();
		boost::asio::co_spawn(
			executor_,
			[self, frame = std::move(frame)]() mutable -> araya::task<void> { co_await self->post(std::move(frame)); },
			boost::asio::detached);
	}

	araya::task<std::optional<std::string>> read() override {
		for (;;) {
			if (!queue_.empty()) {
				std::string frame = std::move(queue_.front());
				queue_.pop_front();
				co_return frame;
			}
			if (closed_)
				co_return std::nullopt;
			boost::system::error_code ec;
			co_await wake_.async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
		}
	}

	void close() override { lose(); }

	std::string stderr_tail() const override { return {}; }

private:
	araya::task<void> post(std::string frame) {
		request http_request;
		http_request.server = server_;
		http_request.target = server_.path.empty() ? std::string{"/"} : server_.path;
		http_request.method = boost::beast::http::verb::post;
		http_request.body = std::move(frame);
		http_request.headers.push_back({"content-type", "application/json"});
		http_request.headers.push_back({"accept", "application/json, text/event-stream"});
		http_request.headers.push_back({"mcp-protocol-version", protocol_version});
		if (!session_.empty())
			http_request.headers.push_back({"mcp-session-id", session_});
		for (auto const& [key, value] : headers_)
			http_request.headers.push_back({key, value});

		auto body = std::make_shared<std::string>();
		auto options = araya::llm::http::request_options{
			.connect_timeout = std::chrono::milliseconds(timeout_ms_),
			.idle_timeout = std::chrono::milliseconds(timeout_ms_),
		};
		response resp;
		try {
			resp = co_await araya::llm::http::stream_request(
				executor_,
				http_request,
				[body](std::string_view chunk) -> araya::task<void> {
					if (body->size() < max_body) {
						auto const keep = std::min<std::size_t>(chunk.size(), max_body - body->size());
						body->append(chunk.data(), keep);
					}
					co_return;
				},
				options,
				{});
		} catch (...) {
			lose();
			co_return;
		}
		if (closed_)
			co_return;
		if (resp.status < 200 || resp.status >= 300) {
			lose();
			co_return;
		}
		if (auto const session = araya::llm::http::header_value(resp.headers, "mcp-session-id"); !session.empty())
			session_ = session;
		if (content_type_is_event_stream(resp)) {
			araya::llm::sse_parser parser;
			auto self = shared_from_this();
			parser.on_event = [self](std::string_view, std::string_view data) {
				if (!data.empty())
					self->enqueue(std::string(data));
			};
			parser.feed(*body);
		} else if (!body->empty()) {
			std::string frame = std::move(*body);
			enqueue(std::move(frame));
		}
	}

	void enqueue(std::string frame) {
		if (closed_)
			return;
		queue_.push_back(std::move(frame));

		wake_.try_send(boost::system::error_code{});
	}

	void lose() {
		closed_ = true;

		wake_.try_send(boost::system::error_code{});
	}

	boost::asio::any_io_executor executor_;
	endpoint server_;
	std::vector<std::pair<std::string, std::string>> headers_;
	std::uint64_t timeout_ms_;
	std::string session_;
	std::deque<std::string> queue_;
	bool closed_ = false;
	boost::asio::experimental::channel<void(boost::system::error_code)> wake_;
};

} // namespace

std::shared_ptr<message_channel> launch_http(boost::asio::any_io_executor executor, server_config const& config) {
	if (config.url.empty())
		throw std::invalid_argument("mcp: remote server '" + config.name + "' has no url");
	auto server = araya::llm::http::parse_url(config.url);
	return std::make_shared<http_channel>(std::move(executor), std::move(server), config.headers, config.timeout_ms);
}

} // namespace araya::mcp
