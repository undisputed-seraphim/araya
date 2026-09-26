#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/user-questions/user_questions.hpp"

#include "support/plugin_harness.hpp"

#include <memory>
#include <stdexcept>
#include <string>

namespace {

using namespace araya::user_questions;

struct harness : araya_test::plugin_harness {
	araya::component_spec spec() {
		return araya_test::plugin_harness::spec(&araya::user_questions::plugin_descriptor());
	}
};

} // namespace

TEST_CASE("ask requires an answerer") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.spec());
		co_await rt.wait_idle();
		auto questions = rt.root_context().require<user_questions_service>(user_questions_key).shared();

		ask_request request;
		request.questions.push_back(question{.id = "q", .question = "Which?"});
		CHECK_THROWS_AS(co_await questions->ask(request), std::runtime_error);
	});
}

TEST_CASE("a registered answerer answers the request") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		auto questions = root.require<user_questions_service>(user_questions_key).shared();

		questions->register_answerer(root, [](ask_request const& request) {
			answer response;
			for (auto const& q : request.questions)
				response.answers.push_back(answer_item{.id = q.id, .selected = {"blue"}});
			return response;
		});

		ask_request request;
		request.questions.push_back(
			question{.id = "color", .question = "Which color?", .options = {{.label = "red"}, {.label = "blue"}}});
		auto response = co_await questions->ask(request);
		REQUIRE(response.answers.size() == 1);
		CHECK(response.answers[0].id == "color");
		REQUIRE(response.answers[0].selected.size() == 1);
		CHECK(response.answers[0].selected[0] == "blue");
	});
}

TEST_CASE("an aborted request is rejected without asking") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.spec());
		co_await rt.wait_idle();
		auto questions = rt.root_context().require<user_questions_service>(user_questions_key).shared();

		std::stop_source source;
		source.request_stop();
		ask_request request;
		request.questions.push_back(question{.id = "q", .question = "Which?"});
		request.stop = source.get_token();
		CHECK_THROWS_AS(co_await questions->ask(request), std::runtime_error);
	});
}
