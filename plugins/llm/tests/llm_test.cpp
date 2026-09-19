#include <catch2/catch_test_macros.hpp>

#include "araya/llm/http.hpp"
#include "araya/llm/llm.hpp"
#include "araya/llm/sse.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace araya::llm;
using namespace std::chrono_literals;

araya::task<void> delay(boost::asio::any_io_executor ex, std::chrono::milliseconds ms) {
	boost::asio::steady_timer timer(ex, ms);
	co_await timer.async_wait(boost::asio::use_awaitable);
}

// A scripted adapter: replays canned chunks, or waits for the stop token
// and reports aborted.
struct mock_adapter : llm_adapter {
	std::vector<stream_chunk> canned;
	bool wait_for_stop = false;

	mutable bool streamed = false;
	mutable std::string provider_seen;

	araya::task<void> stream(generate_options const& options, chunk_sink const& sink) override {
		streamed = true;
		provider_seen = options.provider;
		if (wait_for_stop) {
			auto ex = co_await boost::asio::this_coro::executor;
			boost::asio::steady_timer poll(ex, 1ms);
			while (!options.stop_token.stop_requested()) {
				poll.expires_after(1ms);
				co_await poll.async_wait(boost::asio::use_awaitable);
			}
			co_await sink(finish_chunk{
				finish_chunk::reason::aborted, llm_failure{llm_error_code::aborted, "cancelled"}, std::nullopt});
			co_return;
		}
		for (auto const& chunk : canned)
			co_await sink(chunk);
	}
};

std::shared_ptr<mock_adapter> g_adapter;

// A consumer fiber that owns the route registration: unloading it must
// erase the routes.
struct adapter_plugin : araya::plugin {
	explicit adapter_plugin(std::shared_ptr<mock_adapter> adapter)
		: adapter(std::move(adapter)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto service = ctx.require<llm_service>(llm_key);
		registration = service->register_adapter({"mock"}, adapter, ctx);
		co_return;
	}

	std::shared_ptr<mock_adapter> adapter;
	araya::registration registration;
};

std::unique_ptr<araya::plugin> make_adapter(araya::plugin_config const&) {
	return std::make_unique<adapter_plugin>(g_adapter);
}

static const araya::dependency_spec g_llm_dep[]{{araya::service_id{"llm", 1}, true}};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::plugin_descriptor g_adapter_desc{"adapter", g_llm_dep, g_no_provs, &make_adapter};

struct harness {
	boost::asio::io_context io;
	std::shared_ptr<araya::runtime> rt = std::make_shared<araya::runtime>(io.get_executor());

	template <typename Fn>
	void run(Fn&& fn) {
		g_adapter.reset();
		struct driver {
			std::decay_t<Fn> fn;
			harness* self;
			araya::task<void> operator()() { co_await fn(*self->rt); }
		};
		boost::asio::co_spawn(io.get_executor(), driver{std::forward<Fn>(fn), this}, boost::asio::detached);
		io.run();
		io.restart();
	}

	araya::component_spec spec(araya::plugin_descriptor const* d, araya::plugin_config cfg = {}) {
		return araya::component_spec{
			std::shared_ptr<araya::plugin_descriptor>(const_cast<araya::plugin_descriptor*>(d), [](auto*) {}),
			std::move(cfg),
			nullptr,
			""};
	}

	araya::component_spec llm_spec() { return spec(&araya::llm::plugin_descriptor()); }

	std::shared_ptr<llm_service> service(araya::plugin_context& root_ctx) {
		return root_ctx.require<llm_service>(llm_key).shared();
	}
};

stream_chunk text_delta(std::size_t index, std::string text) {
	text_delta_chunk chunk;
	chunk.index = index;
	chunk.text = std::move(text);
	return chunk;
}

stream_chunk usage(uint64_t in, uint64_t out) {
	usage_chunk chunk;
	chunk.usage.input_tokens = in;
	chunk.usage.output_tokens = out;
	return chunk;
}

stream_chunk finish(finish_chunk::reason why) {
	finish_chunk chunk;
	chunk.why = why;
	return chunk;
}

} // namespace

TEST_CASE("routing delivers chunks in contract order through the sink") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		auto adapter = std::make_shared<mock_adapter>();
		g_adapter = adapter;
		adapter->canned = {
			block_start_chunk{0, content_block_type::text},
			text_delta(0, "hello "),
			text_delta(0, "world"),
			block_end_chunk{0, content_block{text_block{"hello world"}}},
			usage(7, 2),
			finish(finish_chunk::reason::stop),
		};
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.spec(&g_adapter_desc));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto service = h.service(root_ctx);
		CHECK(service->providers() == std::vector<std::string>{"mock"});

		generate_options options;
		options.provider = "mock";
		options.model = "test-model";
		options.session_id = "s1";

		std::vector<stream_chunk> got;
		chunk_sink sink = [&got](stream_chunk const& chunk) -> araya::task<void> {
			got.push_back(chunk);
			co_return;
		};
		co_await service->stream(options, sink);

		CHECK(adapter->streamed);
		CHECK(adapter->provider_seen == "mock");
		REQUIRE(got.size() == 6);
		CHECK(std::holds_alternative<block_start_chunk>(got[0]));
		CHECK(std::holds_alternative<text_delta_chunk>(got[1]));
		CHECK(std::holds_alternative<text_delta_chunk>(got[2]));
		CHECK(std::holds_alternative<block_end_chunk>(got[3]));
		CHECK(std::holds_alternative<usage_chunk>(got[4]));
		CHECK(std::holds_alternative<finish_chunk>(got[5]));
		CHECK(std::get<usage_chunk>(got[4]).usage.input_tokens == 7);
		CHECK(std::get<usage_chunk>(got[4]).usage.output_tokens == 2);
		CHECK(std::get<finish_chunk>(got[5]).why == finish_chunk::reason::stop);
		CHECK(std::get<finish_chunk>(got[5]).failure == std::nullopt);
	});
}

TEST_CASE("unknown providers throw no_adapter and bad options are invalid") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_adapter = std::make_shared<mock_adapter>();
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.spec(&g_adapter_desc));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto service = h.service(root_ctx);
		auto sink = [](stream_chunk const&) -> araya::task<void> { co_return; };

		generate_options missing_provider;
		missing_provider.provider = "nobody";
		missing_provider.model = "m";
		CHECK_THROWS_AS(co_await service->stream(missing_provider, sink), llm_error);
		try {
			co_await service->stream(missing_provider, sink);
		} catch (llm_error const& e) {
			CHECK(e.failure().code == llm_error_code::no_adapter);
		}

		generate_options empty_provider;
		empty_provider.model = "m";
		CHECK_THROWS_AS(co_await service->stream(empty_provider, sink), std::invalid_argument);
	});
}

TEST_CASE("duplicate registration is all-or-nothing") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_adapter = std::make_shared<mock_adapter>();
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.spec(&g_adapter_desc));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto service = h.service(root_ctx);
		auto other = std::make_shared<mock_adapter>();
		CHECK_THROWS_AS(service->register_adapter({"mock", "fresh"}, other, root_ctx), llm_error);
		CHECK(service->providers() == std::vector<std::string>{"mock"});
	});
}

TEST_CASE("a finish chunk carrying a failure completes without throwing") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		auto adapter = std::make_shared<mock_adapter>();
		g_adapter = adapter;
		llm_failure failure{llm_error_code::server, "provider exploded"};
		failure.status = 500;
		adapter->canned = {
			usage(0, 0),
			finish_chunk{finish_chunk::reason::error, std::move(failure), std::nullopt},
		};
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.spec(&g_adapter_desc));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto service = h.service(root_ctx);
		generate_options options;
		options.provider = "mock";
		options.model = "m";

		std::vector<stream_chunk> got;
		chunk_sink sink = [&got](stream_chunk const& chunk) -> araya::task<void> {
			got.push_back(chunk);
			co_return;
		};
		co_await service->stream(options, sink);

		REQUIRE(got.size() == 2);
		auto const& f = std::get<finish_chunk>(got[1]);
		CHECK(f.why == finish_chunk::reason::error);
		REQUIRE(f.failure.has_value());
		CHECK(f.failure->code == llm_error_code::server);
		CHECK(f.failure->status == 500);
	});
}

TEST_CASE("the stop token reaches the adapter and it reports aborted") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		auto adapter = std::make_shared<mock_adapter>();
		adapter->wait_for_stop = true;
		g_adapter = adapter;
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.spec(&g_adapter_desc));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto service = h.service(root_ctx);
		std::stop_source stop;
		generate_options options;
		options.provider = "mock";
		options.model = "m";
		options.stop_token = stop.get_token();

		std::vector<stream_chunk> got;
		chunk_sink sink = [&got](stream_chunk const& chunk) -> araya::task<void> {
			got.push_back(chunk);
			co_return;
		};
		bool done = false;
		boost::asio::co_spawn(
			rt.bus()->executor(),
			[&, service]() -> araya::task<void> {
				co_await service->stream(options, sink);
				done = true;
			},
			boost::asio::detached);

		co_await delay(rt.bus()->executor(), 10ms);
		stop.request_stop();
		for (int i = 0; i < 100 && !done; ++i)
			co_await delay(rt.bus()->executor(), 1ms);
		CHECK(done);
		REQUIRE(got.size() == 1);
		auto const& f = std::get<finish_chunk>(got[0]);
		CHECK(f.why == finish_chunk::reason::aborted);
		REQUIRE(f.failure.has_value());
		CHECK(f.failure->code == llm_error_code::aborted);
	});
}

TEST_CASE("adapter teardown with its registering fiber erases the routes") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_adapter = std::make_shared<mock_adapter>();
		co_await rt.mount(h.llm_spec());
		auto adapter_fiber = co_await rt.mount(h.spec(&g_adapter_desc));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto service = h.service(root_ctx);
		CHECK(service->providers() == std::vector<std::string>{"mock"});

		co_await rt.retire(adapter_fiber);
		co_await rt.wait_idle();
		CHECK(service->providers().empty());
		auto sink = [](stream_chunk const&) -> araya::task<void> { co_return; };
		generate_options options;
		options.provider = "mock";
		options.model = "m";
		CHECK_THROWS_AS(co_await service->stream(options, sink), llm_error);
	});
}

TEST_CASE("resolve_model delegates to the adapter and answers nullopt for strangers") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_adapter = std::make_shared<mock_adapter>();
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.spec(&g_adapter_desc));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto service = h.service(root_ctx);
		auto info = service->resolve_model("mock", "test-model");
		REQUIRE(info.has_value());
		CHECK(info->provider == "mock");
		CHECK(info->model == "test-model");
		CHECK(service->resolve_model("nobody", "test-model") == std::nullopt);
	});
}

TEST_CASE("sse parser frames events across arbitrary chunk boundaries") {
	sse_parser parser;
	std::vector<std::pair<std::string, std::string>> events;
	int activity = 0;
	parser.on_event = [&](std::string_view event, std::string_view data) {
		events.emplace_back(std::string(event), std::string(data));
	};
	parser.on_activity = [&] { ++activity; };

	// BOM, CRLF framing, comment heartbeat, multi-line data, event field.
	parser.feed("\xEF\xBB\xBF"
				"data: first\r\n");
	parser.feed(": heartbeat\r\n");
	parser.feed("data: second\r\n\r\n");
	parser.feed("event: typed\ndata: third\ndata: fourth\n\n");
	parser.feed("event: dangling\n\n");

	REQUIRE(events.size() == 2);
	CHECK(events[0] == std::pair<std::string, std::string>{"", "first\nsecond"});
	CHECK(events[1] == std::pair<std::string, std::string>{"typed", "third\nfourth"});
	CHECK(activity == 7);

	// The [DONE] sentinel is plain data; the wire layer interprets it.
	parser.feed("data: [DONE]\n\n");
	REQUIRE(events.size() == 3);
	CHECK(events[2] == std::pair<std::string, std::string>{"", "[DONE]"});
}

TEST_CASE("parse_url splits scheme, host, port, and path") {
	auto ep = araya::llm::http::parse_url("https://api.example.com:8443/v1/");
	CHECK(ep.scheme == "https");
	CHECK(ep.host == "api.example.com");
	CHECK(ep.port == "8443");
	CHECK(ep.path == "/v1");

	auto local = araya::llm::http::parse_url("http://127.0.0.1:11434");
	CHECK(local.scheme == "http");
	CHECK(local.host == "127.0.0.1");
	CHECK(local.port == "11434");
	CHECK(local.path.empty());

	CHECK_THROWS_AS(araya::llm::http::parse_url("api.example.com/v1"), std::invalid_argument);
	CHECK_THROWS_AS(araya::llm::http::parse_url("ftp://api.example.com"), std::invalid_argument);
	CHECK_THROWS_AS(araya::llm::http::parse_url("https://"), std::invalid_argument);
}
