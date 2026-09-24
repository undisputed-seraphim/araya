#include <catch2/catch_test_macros.hpp>

#include "araya/llm-openai/openai.hpp"
#include "support/stream_chunks.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The adapter against an in-process Beast SSE server on 127.0.0.1
// (plain HTTP - the TLS path is exercised manually against a real
// provider). Covers the full stream, error statuses, tool-call deltas,
// the idle watchdog, and stop-token cancellation.
namespace {

using namespace araya::llm;
using namespace araya::llm_openai;
using namespace araya_test::llm;
using namespace std::chrono_literals;

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Serves one request per run: accepts, records the body, and either
// answers through the handler or (for the stall tests) writes only the
// headers and goes quiet.
struct test_server {
	explicit test_server(boost::asio::any_io_executor executor)
		: acceptor(executor, tcp::endpoint{net::ip::make_address("127.0.0.1"), 0}) {}

	std::uint16_t port() const { return acceptor.local_endpoint().port(); }

	using handler_type = std::function<http::response<http::string_body>(http::request<http::string_body> const&)>;
	handler_type on_request;
	bool stall = false;
	// Serve the response with chunked transfer encoding: the wire carries
	// chunk-size lines and CRLFs around the body, so the framing must be
	// stripped before the consumer sees it.
	bool chunked = false;
	std::chrono::milliseconds stall_delay{1000};
	std::string request_body;

	araya::task<void> run_one() {
		auto socket = co_await acceptor.async_accept(net::use_awaitable);
		beast::flat_buffer buffer;
		http::request<http::string_body> request;
		co_await http::async_read(socket, buffer, request, net::use_awaitable);
		request_body = request.body();
		if (stall) {
			http::response<http::empty_body> header{http::status::ok, 11};
			header.set(http::field::content_type, "text/event-stream");
			header.set(http::field::content_length, "1024");
			http::response_serializer<http::empty_body> serializer{header};
			co_await http::async_write_header(socket, serializer, net::use_awaitable);
			net::steady_timer timer(socket.get_executor(), stall_delay);
			co_await timer.async_wait(net::use_awaitable);
			co_return;
		}
		auto response = on_request(request);
		response.set(http::field::content_type, "text/event-stream");
		if (chunked)
			response.chunked(true);
		co_await http::async_write(socket, response, net::use_awaitable);
	}

	tcp::acceptor acceptor;
};

struct harness {
	boost::asio::io_context io;

	template <typename Fn>
	void run(Fn&& fn) {
		boost::asio::co_spawn(io.get_executor(), std::forward<Fn>(fn), boost::asio::detached);
		io.run();
		io.restart();
	}

	void serve(test_server& server) {
		boost::asio::co_spawn(
			io.get_executor(), [&server]() -> araya::task<void> { co_await server.run_one(); }, boost::asio::detached);
	}

	araya::task<void> delay(std::chrono::milliseconds ms) {
		net::steady_timer timer(io.get_executor(), ms);
		co_await timer.async_wait(net::use_awaitable);
	}
};

openai_config config_for(test_server const& server) {
	openai_config config;
	config.base_url = "http://127.0.0.1:" + std::to_string(server.port());
	return config;
}

generate_options chat(std::string_view model = "gpt-4o-mini") {
	generate_options options;
	options.provider = "openai";
	options.model = std::string(model);
	options.messages = {{message_role::user, {text_block{"hi"}}, std::nullopt}};
	return options;
}

std::optional<finish_chunk> last_finish(std::vector<stream_chunk> const& chunks) {
	if (chunks.empty())
		return std::nullopt;
	auto const* finish = std::get_if<finish_chunk>(&chunks.back());
	return finish ? std::optional<finish_chunk>(*finish) : std::nullopt;
}

araya::task<std::vector<stream_chunk>> run_stream(openai_adapter& adapter, generate_options const& options) {
	std::vector<stream_chunk> got;
	chunk_sink sink = [&got](stream_chunk const& chunk) -> araya::task<void> {
		got.push_back(chunk);
		co_return;
	};
	co_await adapter.stream(options, sink);
	co_return got;
}

} // namespace

TEST_CASE("a canned SSE stream assembles text, usage, and finish") {
	harness h;
	h.run([&]() -> araya::task<void> {
		test_server server(h.io.get_executor());
		server.on_request = [](http::request<http::string_body> const&) {
			http::response<http::string_body> response{http::status::ok, 11};
			response.body() = sse({
				R"({"choices":[{"delta":{"content":"Hello"}}]})",
				R"({"choices":[{"delta":{"content":" there"}}],"usage":{"prompt_tokens":7,"completion_tokens":2,"total_tokens":9}})",
			});
			response.prepare_payload();
			return response;
		};
		h.serve(server);

		openai_adapter adapter(config_for(server));
		auto got = co_await run_stream(adapter, chat());

		REQUIRE(got.size() == 6);
		CHECK(std::holds_alternative<block_start_chunk>(got[0]));
		CHECK(std::get<text_delta_chunk>(got[1]).text == "Hello");
		CHECK(std::get<text_delta_chunk>(got[2]).text == " there");
		auto const& block_end = std::get<block_end_chunk>(got[3]);
		CHECK(std::get<text_block>(block_end.block).text == "Hello there");
		CHECK(std::get<usage_chunk>(got[4]).usage.input_tokens == 7);
		CHECK(std::get<usage_chunk>(got[4]).usage.output_tokens == 2);
		auto const finish = last_finish(got);
		REQUIRE(finish.has_value());
		CHECK(finish->why == finish_chunk::reason::stop);

		// The server saw the assembled request.
		CHECK(server.request_body.find("gpt-4o-mini") != std::string::npos);
		CHECK(server.request_body.find("stream") != std::string::npos);
		CHECK(server.request_body.find("\"hi\"") != std::string::npos);
	});
}

TEST_CASE("chunked transfer encoding is de-framed before the translator") {
	// Real SSE servers stream with chunked transfer encoding: the body
	// arrives wrapped in chunk-size lines and CRLFs. The read loop must
	// hand the consumer the payload, not the framing, or the first bytes
	// of the stream are corrupted (size lines, NULs) and every event
	// after the first framing boundary is misparsed.
	harness h;
	h.run([&]() -> araya::task<void> {
		test_server server(h.io.get_executor());
		server.chunked = true;
		server.on_request = [](http::request<http::string_body> const&) {
			http::response<http::string_body> response{http::status::ok, 11};
			response.body() = sse({
				R"({"choices":[{"delta":{"content":"Hello"}}]})",
				R"({"choices":[{"delta":{"content":" there"}}],"usage":{"prompt_tokens":7,"completion_tokens":2,"total_tokens":9}})",
			});
			response.chunked(true);
			return response;
		};
		h.serve(server);

		openai_adapter adapter(config_for(server));
		auto got = co_await run_stream(adapter, chat());

		REQUIRE(got.size() == 6);
		CHECK(std::holds_alternative<block_start_chunk>(got[0]));
		CHECK(std::get<text_delta_chunk>(got[1]).text == "Hello");
		CHECK(std::get<text_delta_chunk>(got[2]).text == " there");
		CHECK(std::get<text_block>(std::get<block_end_chunk>(got[3]).block).text == "Hello there");
		CHECK(std::get<usage_chunk>(got[4]).usage.input_tokens == 7);
		CHECK(std::get<usage_chunk>(got[4]).usage.output_tokens == 2);
		auto const finish = last_finish(got);
		REQUIRE(finish.has_value());
		CHECK(finish->why == finish_chunk::reason::stop);
	});
}

TEST_CASE("tool-call deltas round-trip through the adapter") {
	harness h;
	h.run([&]() -> araya::task<void> {
		test_server server(h.io.get_executor());
		server.on_request = [](http::request<http::string_body> const&) {
			http::response<http::string_body> response{http::status::ok, 11};
			response.body() = sse({
				R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call-1","function":{"name":"weather","arguments":"{\"ci"}}]}}]})",
				R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"ty\":\"X\"}"}}]},"finish_reason":"tool_calls"}]})",
			});
			response.prepare_payload();
			return response;
		};
		h.serve(server);

		openai_adapter adapter(config_for(server));
		auto got = co_await run_stream(adapter, chat());

		REQUIRE(got.size() >= 4);
		CHECK(std::holds_alternative<block_start_chunk>(got[0]));
		CHECK(std::holds_alternative<tool_call_delta_chunk>(got[1]));
		CHECK(std::holds_alternative<tool_call_delta_chunk>(got[2]));
		auto const& block_end = std::get<block_end_chunk>(got[3]);
		auto const& call = std::get<tool_call_block>(block_end.block);
		CHECK(call.id == "call-1");
		CHECK(call.name == "weather");
		CHECK(call.arguments == "{\"city\":\"X\"}");
		auto const finish = last_finish(got);
		REQUIRE(finish.has_value());
		CHECK(finish->why == finish_chunk::reason::tool_calls);
	});
}

TEST_CASE("a non-2xx status finishes with the mapped failure") {
	harness h;
	h.run([&]() -> araya::task<void> {
		test_server server(h.io.get_executor());
		server.on_request = [](http::request<http::string_body> const&) {
			http::response<http::string_body> response{http::status::unauthorized, 11};
			response.body() = R"({"error":{"message":"Incorrect API key"}})";
			response.prepare_payload();
			return response;
		};
		h.serve(server);

		openai_adapter adapter(config_for(server));
		auto got = co_await run_stream(adapter, chat());

		REQUIRE(got.size() == 1);
		auto const finish = last_finish(got);
		REQUIRE(finish.has_value());
		CHECK(finish->why == finish_chunk::reason::error);
		REQUIRE(finish->failure.has_value());
		CHECK(finish->failure->code == llm_error_code::auth);
		CHECK(finish->failure->status == 401);
		CHECK(finish->failure->message == "Incorrect API key");
	});
}

TEST_CASE("429 with Retry-After carries the retry delay") {
	harness h;
	h.run([&]() -> araya::task<void> {
		test_server server(h.io.get_executor());
		server.on_request = [](http::request<http::string_body> const&) {
			http::response<http::string_body> response{http::status::too_many_requests, 11};
			response.set(http::field::retry_after, "42");
			response.body() = R"({"error":{"message":"slow down"}})";
			response.prepare_payload();
			return response;
		};
		h.serve(server);

		openai_adapter adapter(config_for(server));
		auto got = co_await run_stream(adapter, chat());

		auto const finish = last_finish(got);
		REQUIRE(finish.has_value());
		REQUIRE(finish->failure.has_value());
		CHECK(finish->failure->code == llm_error_code::rate_limit);
		CHECK(finish->failure->provider_retry_after == std::chrono::seconds(42));
	});
}

TEST_CASE("a stalled response body trips the idle watchdog") {
	harness h;
	h.run([&]() -> araya::task<void> {
		test_server server(h.io.get_executor());
		server.stall = true;
		server.stall_delay = 1000ms;
		h.serve(server);

		auto config = config_for(server);
		config.idle_timeout = 100ms;
		openai_adapter adapter(std::move(config));
		auto got = co_await run_stream(adapter, chat());

		auto const finish = last_finish(got);
		REQUIRE(finish.has_value());
		CHECK(finish->why == finish_chunk::reason::error);
		REQUIRE(finish->failure.has_value());
		CHECK(finish->failure->code == llm_error_code::timeout);
	});
}

TEST_CASE("stop_token mid-stream finishes as aborted") {
	harness h;
	h.run([&]() -> araya::task<void> {
		test_server server(h.io.get_executor());
		server.stall = true;
		server.stall_delay = 2000ms;
		h.serve(server);

		auto config = config_for(server);
		config.idle_timeout = 5000ms;
		auto adapter = std::make_shared<openai_adapter>(std::move(config));
		std::stop_source stop;
		auto options = chat();
		options.stop_token = stop.get_token();

		std::vector<stream_chunk> got;
		chunk_sink sink = [&got](stream_chunk const& chunk) -> araya::task<void> {
			got.push_back(chunk);
			co_return;
		};
		bool done = false;
		boost::asio::co_spawn(
			h.io.get_executor(),
			[&, adapter]() -> araya::task<void> {
				co_await adapter->stream(options, sink);
				done = true;
			},
			boost::asio::detached);

		co_await h.delay(100ms);
		stop.request_stop();
		for (int i = 0; i < 200 && !done; ++i)
			co_await h.delay(10ms);
		CHECK(done);

		auto const finish = last_finish(got);
		REQUIRE(finish.has_value());
		CHECK(finish->why == finish_chunk::reason::aborted);
		REQUIRE(finish->failure.has_value());
		CHECK(finish->failure->code == llm_error_code::aborted);
	});
}

TEST_CASE("malformed SSE payloads finish as malformed_response") {
	harness h;
	h.run([&]() -> araya::task<void> {
		test_server server(h.io.get_executor());
		server.on_request = [](http::request<http::string_body> const&) {
			http::response<http::string_body> response{http::status::ok, 11};
			response.body() = "data: this is not json\n\ndata: [DONE]\n\n";
			response.prepare_payload();
			return response;
		};
		h.serve(server);

		openai_adapter adapter(config_for(server));
		auto got = co_await run_stream(adapter, chat());

		auto const finish = last_finish(got);
		REQUIRE(finish.has_value());
		CHECK(finish->why == finish_chunk::reason::error);
		REQUIRE(finish->failure.has_value());
		CHECK(finish->failure->code == llm_error_code::malformed_response);
	});
}

TEST_CASE("a stream that ends without [DONE] finishes as stream_closed") {
	harness h;
	h.run([&]() -> araya::task<void> {
		test_server server(h.io.get_executor());
		server.on_request = [](http::request<http::string_body> const&) {
			http::response<http::string_body> response{http::status::ok, 11};
			response.body() = sse({R"({"choices":[{"delta":{"content":"cut off"}}]})"}, false);
			response.prepare_payload();
			return response;
		};
		h.serve(server);

		openai_adapter adapter(config_for(server));
		auto got = co_await run_stream(adapter, chat());

		auto const finish = last_finish(got);
		REQUIRE(finish.has_value());
		CHECK(finish->why == finish_chunk::reason::error);
		REQUIRE(finish->failure.has_value());
		CHECK(finish->failure->code == llm_error_code::stream_closed);
	});
}
