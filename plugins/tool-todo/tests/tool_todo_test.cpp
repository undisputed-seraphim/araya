#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tool-todo/tool_todo.hpp"
#include "araya/tools/tools.hpp"

#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using namespace araya::tools;

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec todo_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::tool_todo::plugin_descriptor(), std::move(cfg));
	}
};

struct rig {
	std::shared_ptr<araya::session::session_store> store;
	std::shared_ptr<tools_service> tools;
	std::shared_ptr<araya::tool_todo::todos_service> todos;
	std::string session = "s1";

	araya::task<void> mount(araya::runtime& rt, harness& h, bool allow_parallel = true) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.todo_spec({{"allow_parallel_in_progress", allow_parallel ? "true" : "false"}}));
		co_await rt.wait_idle();
		auto root = rt.root_context();
		store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		tools = root.require<tools_service>(tools_key).shared();
		todos = root.require<araya::tool_todo::todos_service>(araya::tool_todo::todos_key).shared();
		store->create(root, araya::session::session_id{session}, {});
	}

	araya::task<std::optional<tool_result>> call(boost::json::value arguments) {
		std::string const name = "todo_write";
		co_return co_await tools->invoke(
			name, tool_context{.call_id = "c", .name = name, .session = session, .arguments = std::move(arguments)});
	}
};

boost::json::value list(std::initializer_list<std::pair<char const*, char const*>> items) {
	boost::json::array array;
	for (auto const& [content, status] : items)
		array.push_back(boost::json::object{{"content", content}, {"status", status}});
	return boost::json::value{{"todos", std::move(array)}};
}

std::string text_of(tool_result const& result) {
	auto const* arr = result.content.if_array();
	if (!arr || arr->empty())
		return {};
	auto const* object = arr->front().if_object();
	if (!object)
		return {};
	auto it = object->find("text");
	return it != object->end() && it->value().is_string() ? std::string(it->value().as_string()) : std::string{};
}

} // namespace

TEST_CASE("todo_write records the list and replaces it whole") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		auto first = co_await r.call(list({{"draft the plan", "completed"}, {"write the code", "in_progress"}}));
		REQUIRE(first.has_value());
		CHECK_FALSE(first->is_error);
		CHECK(text_of(*first).find("0 pending") != std::string::npos);
		CHECK(text_of(*first).find("1 in progress") != std::string::npos);
		CHECK(text_of(*first).find("1 completed") != std::string::npos);

		auto state = r.todos->state_of(araya::session::session_id{"s1"});
		REQUIRE(state.has_value());
		REQUIRE(state->size() == 2);
		CHECK((*state)[0].content == "draft the plan");
		CHECK((*state)[0].status == "completed");

		// A second call replaces, never appends.
		auto second = co_await r.call(list({{"only one", "pending"}}));
		REQUIRE(second.has_value());
		CHECK_FALSE(second->is_error);
		state = r.todos->state_of(araya::session::session_id{"s1"});
		REQUIRE(state.has_value());
		CHECK(state->size() == 1);
		CHECK((*state)[0].content == "only one");
	});
}

TEST_CASE("a new turn clears the standing plan") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		co_await r.call(list({{"one", "pending"}}));
		REQUIRE(r.todos->state_of(araya::session::session_id{"s1"}).has_value());

		auto session = r.store->get(araya::session::session_id{"s1"});
		REQUIRE(session != nullptr);
		session->append("turn/start", boost::json::object{{"turn", 1}});
		CHECK_FALSE(r.todos->state_of(araya::session::session_id{"s1"}).has_value());
	});
}

TEST_CASE("todo_write rejects malformed lists") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, /*allow_parallel=*/false);

		auto empty = co_await r.call(list({{"   ", "pending"}}));
		REQUIRE(empty.has_value());
		CHECK(empty->is_error);

		auto duplicate = co_await r.call(list({{"same", "pending"}, {"same", "completed"}}));
		REQUIRE(duplicate.has_value());
		CHECK(duplicate->is_error);
		CHECK(text_of(*duplicate).find("duplicate content") != std::string::npos);

		auto parallel = co_await r.call(list({{"a", "in_progress"}, {"b", "in_progress"}}));
		REQUIRE(parallel.has_value());
		CHECK(parallel->is_error);
		CHECK(text_of(*parallel).find("at most one") != std::string::npos);
	});
}

TEST_CASE("parallel in_progress is allowed by default") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, /*allow_parallel=*/true);
		auto result = co_await r.call(list({{"a", "in_progress"}, {"b", "in_progress"}}));
		REQUIRE(result.has_value());
		CHECK_FALSE(result->is_error);
	});
}
