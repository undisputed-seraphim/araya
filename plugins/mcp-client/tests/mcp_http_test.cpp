#include <catch2/catch_test_macros.hpp>

#include "connection.hpp"

#include "araya/task.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using namespace araya::mcp;

// A one-request-per-connection HTTP server over a plain Beast acceptor.
struct http_test_server {
	explicit http_test_server(boost::asio::any_io_executor executor)
		: acceptor(executor, tcp::endpoint{net::ip::make_address("127.0.0.1"), 0}) {}

	std::uint16_t port() const { return acceptor.local_endpoint().port(); }

	std::function<http::response<http::string_body>(http::request<http::string_body> const&)> on_request;
	bool stopped = false;

	araya::task<void> serve() {
		while (!stopped) {
			boost::system::error_code ec;
			auto socket = co_await acceptor.async_accept(net::redirect_error(net::use_awaitable, ec));
			if (ec)
				co_return;
			boost::asio::co_spawn(acceptor.get_executor(), handle(std::move(socket)), boost::asio::detached);
		}
	}

	void stop() {
		stopped = true;
		boost::system::error_code ec;
		acceptor.close(ec);
	}

private:
	araya::task<void> handle(tcp::socket socket) {
		boost::system::error_code ec;
		beast::flat_buffer buffer;
		http::request<http::string_body> request;
		co_await http::async_read(socket, buffer, request, net::redirect_error(net::use_awaitable, ec));
		if (ec)
			co_return;
		auto response = on_request(request);
		co_await http::async_write(socket, response, net::redirect_error(net::use_awaitable, ec));
		socket.shutdown(tcp::socket::shutdown_send, ec);
	}

	tcp::acceptor acceptor;
};

template <class Fn>
void run_coro(boost::asio::io_context& io, Fn fn) {
	std::exception_ptr error;
	boost::asio::co_spawn(
		io,
		[&]() -> araya::task<void> {
			try {
				co_await fn();
			} catch (...) {
				error = std::current_exception();
			}
		},
		boost::asio::detached);
	io.run();
	if (error)
		std::rethrow_exception(error);
}

} // namespace

TEST_CASE("mcp client talks to a streamable-http server") {
	boost::asio::io_context io;
	http_test_server server(io.get_executor());

	server.on_request = [](http::request<http::string_body> const& request) {
		http::response<http::string_body> response{http::status::ok, 11};
		response.set("Mcp-Session-Id", "sess-1");
		auto const message = boost::json::parse(request.body()).as_object();
		auto const method = message.if_contains("method");
		auto const id = message.if_contains("id");
		std::string const name = method ? std::string(method->as_string()) : "";
		if (name == "initialize") {
			response.set(http::field::content_type, "application/json");
			response.body() = boost::json::serialize(boost::json::object{
				{"jsonrpc", "2.0"},
				{"id", id ? *id : boost::json::value{}},
				{"result",
				 boost::json::object{
					 {"protocolVersion", "2025-06-18"},
					 {"capabilities", boost::json::object{{"tools", boost::json::object{}}}},
					 {"instructions", "HTTP INSTRUCTIONS"},
					 {"serverInfo", boost::json::object{{"name", "http-fixture"}, {"version", "1"}}}}}});
		} else if (name == "notifications/initialized") {
			response.result(http::status::accepted);
		} else if (name == "tools/list") {
			response.set(http::field::content_type, "application/json");
			response.body() = boost::json::serialize(boost::json::object{
				{"jsonrpc", "2.0"},
				{"id", id ? *id : boost::json::value{}},
				{"result",
				 boost::json::object{
					 {"tools",
					  boost::json::array{boost::json::object{
						  {"name", "echo"},
						  {"description", "Echo."},
						  {"inputSchema", boost::json::object{{"type", "object"}}}}}}}}});
		} else if (name == "tools/call") {
			// Reply as an SSE stream to exercise the event-stream path.
			auto const params = message.if_contains("params")->as_object();
			auto const arguments = params.if_contains("arguments")->as_object();
			std::string const text = arguments.if_contains("text") ? std::string(arguments.at("text").as_string()) : "";
			response.set(http::field::content_type, "text/event-stream");
			std::string frame = boost::json::serialize(boost::json::object{
				{"jsonrpc", "2.0"},
				{"id", id ? *id : boost::json::value{}},
				{"result",
				 boost::json::object{
					 {"content",
					  boost::json::array{boost::json::object{{"type", "text"}, {"text", "http echo: " + text}}}}}}});
			response.body() = "event: message\ndata: " + frame + "\n\n";
		} else {
			response.set(http::field::content_type, "application/json");
			response.body() = R"({"jsonrpc":"2.0","error":{"code":-32601,"message":"not found"}})";
		}
		return response;
	};
	boost::asio::co_spawn(io.get_executor(), server.serve(), boost::asio::detached);

	server_config config;
	config.name = "http";
	config.transport = "remote";
	config.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/mcp";
	config.timeout_ms = 10'000;

	std::vector<mcp_tool> discovered;
	std::string instructions;
	connection_observer observer;
	observer.on_tools = [&](std::vector<mcp_tool> const& tools) { discovered = tools; };
	observer.on_instructions = [&](std::string const& text) { instructions = text; };

	auto connection = std::make_shared<server_connection>(io.get_executor(), config, observer);
	std::exception_ptr ready_error;
	boost::json::value call_result;

	run_coro(io, [&]() -> araya::task<void> {
		connection->start();
		ready_error = co_await connection->ready();
		if (!ready_error) {
			boost::json::object params;
			params["name"] = "echo";
			params["arguments"] = boost::json::object{{"text", "hello"}};
			call_result = co_await connection->request("tools/call", std::move(params), {});
		}
		connection->stop();
		server.stop();
	});

	REQUIRE_FALSE(ready_error);
	REQUIRE(discovered.size() == 1);
	REQUIRE(discovered[0].name == "echo");
	REQUIRE(instructions == "HTTP INSTRUCTIONS");
	REQUIRE(boost::json::serialize(call_result).find("http echo: hello") != std::string::npos);
}
