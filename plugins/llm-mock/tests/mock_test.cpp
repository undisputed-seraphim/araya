#include <catch2/catch_test_macros.hpp>

#include "araya/llm-mock/mock.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace araya::llm;
using namespace araya::llm_mock;

struct harness {
	boost::asio::io_context io;
	std::shared_ptr<araya::runtime> rt = std::make_shared<araya::runtime>(io.get_executor());

	template <typename Fn>
	void run(Fn&& fn) {
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
};

} // namespace

TEST_CASE("load_config parses scripts and rejects malformed ones") {
	CHECK_THROWS_AS(load_config({{"script", "not json"}}), std::invalid_argument);
	CHECK_THROWS_AS(load_config({{"script", "[42]"}}), std::invalid_argument);
	CHECK_THROWS_AS(load_config({{"script", "[{\"nope\": 1}]"}}), std::invalid_argument);
	CHECK_THROWS_AS(load_config({{"script", "[{\"tool_call\":{\"name\":\"t\"}}]"}}), std::invalid_argument);
	auto ok = load_config({{"script", "[{\"text\":\"a\"},{\"tool_call\":{\"name\":\"t\",\"arguments\":\"{}\"}}]"}});
	REQUIRE(ok.script.size() == 2);
	CHECK(ok.script[0].is_tool_call == false);
	CHECK(ok.script[0].text == "a");
	CHECK(ok.script[1].is_tool_call == true);
	CHECK(ok.script[1].text == "t");
	CHECK(ok.script[1].arguments == "{}");
}

TEST_CASE("the script replays its steps then falls back to the canned response") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.spec(&araya::llm::plugin_descriptor()));
		co_await rt.mount(h.spec(
			&araya::llm_mock::plugin_descriptor(),
			{{"script",
			  "[{\"text\":\"step "
			  "one\"},{\"tool_call\":{\"name\":\"weather\",\"arguments\":\"{\\\"city\\\":\\\"paris\\\"}\"}}]"}}));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto service = root_ctx.require<llm_service>(llm_key).shared();

		generate_options options;
		options.provider = "mock";
		options.model = "mock-model";

		auto collect = [&](std::vector<stream_chunk>& got) -> araya::task<void> {
			chunk_sink sink = [&](stream_chunk const& chunk) -> araya::task<void> {
				got.push_back(chunk);
				co_return;
			};
			co_await service->stream(options, sink);
		};

		std::vector<stream_chunk> first;
		co_await collect(first);
		REQUIRE(first.size() == 5);
		CHECK(std::holds_alternative<text_delta_chunk>(first[1]));
		CHECK(std::get<text_delta_chunk>(first[1]).text == "step one");
		CHECK(std::get<finish_chunk>(first[4]).why == finish_chunk::reason::stop);

		std::vector<stream_chunk> second;
		co_await collect(second);
		REQUIRE(second.size() == 5);
		CHECK(std::holds_alternative<block_start_chunk>(second[0]));
		CHECK(std::get<tool_call_delta_chunk>(second[1]).name == "weather");
		CHECK(std::get<tool_call_delta_chunk>(second[1]).arguments_delta == "{\"city\":\"paris\"}");
		auto const* end = std::get_if<block_end_chunk>(&second[2]);
		REQUIRE(end != nullptr);
		auto const* call = std::get_if<tool_call_block>(&end->block);
		REQUIRE(call != nullptr);
		CHECK(call->id == "call-1");
		CHECK(call->name == "weather");
		CHECK(std::get<finish_chunk>(second[4]).why == finish_chunk::reason::tool_calls);

		// The script is exhausted: the canned response takes over.
		std::vector<stream_chunk> third;
		co_await collect(third);
		CHECK(std::get<text_delta_chunk>(third[1]).text == "echo: ");
		CHECK(std::get<finish_chunk>(third[4]).why == finish_chunk::reason::stop);
	});
}
