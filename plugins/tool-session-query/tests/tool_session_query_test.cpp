#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/agent.hpp"
#include "araya/llm-mock/mock.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session-query/query.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tool-session-query/tool_session_query.hpp"
#include "araya/tools/tools.hpp"

#include "support/plugin_harness.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec query_spec() { return spec(&araya::session_query::plugin_descriptor()); }
	araya::component_spec llm_spec() { return spec(&araya::llm::plugin_descriptor()); }
	araya::component_spec mock_spec() {
		return spec(&araya::llm_mock::plugin_descriptor(), {{"provider", "mock"}, {"response", "echo"}});
	}
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec agent_spec() { return spec(&araya::agent::plugin_descriptor()); }
	araya::component_spec tool_spec() { return spec(&araya::tool_session_query::plugin_descriptor()); }
};

struct rig {
	std::shared_ptr<araya::session::session_store> store;
	std::shared_ptr<araya::tools::tools_service> tools;
	std::shared_ptr<araya::session::session> session;
	std::optional<araya::plugin_context> owner;

	araya::task<void> mount(araya::runtime& rt, harness& h) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.query_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.tool_spec());
		co_await rt.wait_idle();
		owner.emplace(rt.root_context());
		store = owner->require<araya::session::session_store>(araya::session::sessions_key).shared();
		tools = owner->require<araya::tools::tools_service>(araya::tools::tools_key).shared();
	}

	araya::session::session& create(
		std::string const& id,
		std::optional<std::string> cwd = std::nullopt,
		std::optional<araya::session::session_id> parent = std::nullopt) {
		araya::session::create_session_options options;
		options.cwd = std::move(cwd);
		options.parent_session = std::move(parent);
		return *store->create(*owner, araya::session::session_id{id}, std::move(options));
	}

	araya::task<std::optional<araya::tools::tool_result>>
	call(std::string name, boost::json::object arguments = {}, std::string session = "s1") {
		co_return co_await tools->invoke(
			name,
			araya::tools::tool_context{
				.call_id = "c", .name = name, .session = std::move(session), .arguments = std::move(arguments)});
	}
};

void append_user(araya::session::session& session, std::string id, std::string text) {
	session.append(
		"user/message",
		boost::json::value{
			{"id", std::move(id)},
			{"role", "user"},
			{"content", boost::json::array{boost::json::object{{"type", "text"}, {"text", std::move(text)}}}}});
}

std::string text_of(araya::tools::tool_result const& result) {
	auto const* array = result.content.if_array();
	if (!array || array->empty())
		return {};
	auto const* object = array->front().if_object();
	if (!object)
		return {};
	auto it = object->find("text");
	return it != object->end() && it->value().is_string() ? std::string(it->value().as_string()) : std::string{};
}

TEST_CASE("tool-session-query registers the five tools and a section") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto const names = [&] {
			std::vector<std::string> out;
			for (auto const& definition : r.tools->list())
				out.push_back(definition.name);
			return out;
		}();
		CHECK(std::find(names.begin(), names.end(), "session_search") != names.end());
		CHECK(std::find(names.begin(), names.end(), "session_event_search") != names.end());
		CHECK(std::find(names.begin(), names.end(), "session_trace") != names.end());
		CHECK(std::find(names.begin(), names.end(), "session_event_trace") != names.end());
		CHECK(std::find(names.begin(), names.end(), "session_event_read") != names.end());
	});
}

TEST_CASE("session_search finds same-workspace prior sessions and excludes the caller") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		append_user(r.create("s1", std::string("/work")), "u", "caller topic");
		append_user(r.create("s2", std::string("/work")), "u", "prior session topic");
		append_user(r.create("s3", std::string("/elsewhere")), "u", "other topic");

		auto result = co_await r.call("session_search", boost::json::object{{"query", "topic"}});
		REQUIRE(result.has_value());
		CHECK_FALSE(result->is_error);
		auto const text = text_of(*result);
		CHECK(text.find("s2") != std::string::npos);
		CHECK(text.find("s1") == std::string::npos);
		CHECK(text.find("s3") == std::string::npos);
	});
}

TEST_CASE("session_event_search targets one session and rejects foreign workspaces") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		append_user(r.create("s1", std::string("/work")), "u", "caller");
		auto& other = r.create("s2", std::string("/work"));
		append_user(other, "u", "needle in a haystack");
		append_user(r.create("s3", std::string("/elsewhere")), "u", "needle");

		auto result =
			co_await r.call("session_event_search", boost::json::object{{"session_id", "s2"}, {"query", "needle"}});
		REQUIRE(result.has_value());
		CHECK_FALSE(result->is_error);
		CHECK(text_of(*result).find("seq 0") != std::string::npos);

		auto denied =
			co_await r.call("session_event_search", boost::json::object{{"session_id", "s3"}, {"query", "needle"}});
		REQUIRE(denied.has_value());
		CHECK(denied->is_error);
	});
}

TEST_CASE("session_event_search on the current session excludes the in-flight turn") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto& s1 = r.create("s1", std::string("/work"));
		append_user(s1, "u0", "before keyword");
		s1.append("turn/start", boost::json::value{{"turn", 1}});
		s1.append("step/start", boost::json::value{{"turn", 1}, {"step", 1}});
		append_user(s1, "u1", "inflight keyword");

		auto result = co_await r.call("session_event_search", boost::json::object{{"query", "keyword"}});
		REQUIRE(result.has_value());
		CHECK_FALSE(result->is_error);
		auto const text = text_of(*result);
		CHECK(text.find("seq 0") != std::string::npos);
		CHECK(text.find("seq 3") == std::string::npos);
	});
}

TEST_CASE("session_trace reports same-workspace descendants") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		append_user(r.create("s1", std::string("/work")), "u", "caller");
		r.create("parent", std::string("/work"));
		r.create("child", std::string("/work"), araya::session::session_id{"parent"});

		auto result = co_await r.call("session_trace", boost::json::object{{"session_id", "parent"}});
		REQUIRE(result.has_value());
		CHECK_FALSE(result->is_error);
		CHECK(text_of(*result).find("child") != std::string::npos);
	});
}

TEST_CASE("session_event_read returns the target and neighbours") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		append_user(r.create("s1", std::string("/work")), "u", "caller");
		auto& other = r.create("s2", std::string("/work"));
		append_user(other, "u0", "first");
		append_user(other, "u1", "second");
		append_user(other, "u2", "third");

		auto result = co_await r.call(
			"session_event_read", boost::json::object{{"session_id", "s2"}, {"seq", 1}, {"before", 1}, {"after", 1}});
		REQUIRE(result.has_value());
		CHECK_FALSE(result->is_error);
		auto const text = text_of(*result);
		CHECK(text.find("Target event seq 1") != std::string::npos);
		CHECK(text.find("Before") != std::string::npos);
		CHECK(text.find("After") != std::string::npos);
	});
}

} // namespace
