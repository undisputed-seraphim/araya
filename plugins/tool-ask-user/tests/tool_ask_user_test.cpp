#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/tool-ask-user/tool_ask_user.hpp"
#include "araya/tools/tools.hpp"
#include "araya/user-questions/user_questions.hpp"

#include "support/plugin_harness.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <memory>
#include <optional>
#include <string>

namespace {

using namespace araya::tools;

struct harness : araya_test::plugin_harness {
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec questions_spec() { return spec(&araya::user_questions::plugin_descriptor()); }
	araya::component_spec ask_spec() { return spec(&araya::tool_ask_user::plugin_descriptor()); }
};

struct rig {
	std::shared_ptr<tools_service> tools;

	araya::task<void> mount(araya::runtime& rt, harness& h) {
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.questions_spec());
		co_await rt.mount(h.ask_spec());
		co_await rt.wait_idle();
		tools = rt.root_context().require<tools_service>(tools_key).shared();
	}

	araya::task<std::optional<tool_result>> call(boost::json::object args = {}) {
		std::string const tool = "ask_user_question";
		co_return co_await tools->invoke(
			tool, tool_context{.call_id = "c", .name = tool, .arguments = std::move(args)});
	}
};

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

TEST_CASE("ask_user_question returns the answerer's answer as JSON") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto root = rt.root_context();
		auto questions =
			root.require<araya::user_questions::user_questions_service>(araya::user_questions::user_questions_key)
				.shared();
		questions->register_answerer(root, [](araya::user_questions::ask_request const& request) {
			araya::user_questions::answer response;
			for (auto const& q : request.questions) {
				response.answers.push_back(
					araya::user_questions::answer_item{.id = q.id, .selected = {"blue"}, .custom = std::nullopt});
			}
			return response;
		});

		boost::json::object question{
			{"id", "color"},
			{"question", "Which color?"},
			{"options",
			 boost::json::array{boost::json::object{{"label", "red"}}, boost::json::object{{"label", "blue"}}}},
		};
		auto out = co_await r.call({{"questions", boost::json::array{question}}});
		REQUIRE(out.has_value());
		CHECK_FALSE(out->is_error);
		CHECK(text_of(*out).find("\"id\":\"color\"") != std::string::npos);
		CHECK(text_of(*out).find("\"selected\":[\"blue\"]") != std::string::npos);
	});
}

TEST_CASE("ask_user_question reports missing questions and missing answerer") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		auto empty = co_await r.call({{"questions", boost::json::array{}}});
		REQUIRE(empty.has_value());
		CHECK(empty->is_error);

		boost::json::object question{{"id", "q"}, {"question", "Which?"}};
		auto no_answerer = co_await r.call({{"questions", boost::json::array{question}}});
		REQUIRE(no_answerer.has_value());
		CHECK(no_answerer->is_error);
		CHECK(text_of(*no_answerer).find("no user-questions answerer") != std::string::npos);
	});
}
