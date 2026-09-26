#include <catch2/catch_test_macros.hpp>

#include "araya/jobs/jobs.hpp"
#include "araya/llm/bridge.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tool-jobs/tool_jobs.hpp"
#include "araya/tools/tools.hpp"

#include "support/plugin_harness.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using namespace araya::jobs;
using namespace araya::tools;

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec jobs_spec() { return spec(&araya::jobs::plugin_descriptor()); }
	araya::component_spec tool_jobs_spec() { return spec(&araya::tool_jobs::plugin_descriptor()); }
};

struct rig {
	std::shared_ptr<jobs_service> jobs;
	std::shared_ptr<araya::session::session_store> store;
	std::shared_ptr<tools_service> tools;
	std::string session = "s1";

	araya::task<void> mount(araya::runtime& rt, harness& h) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.jobs_spec());
		co_await rt.mount(h.tool_jobs_spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		tools = root.require<tools_service>(tools_key).shared();
		jobs = root.require<jobs_service>(jobs_key).shared();
		store->create(root, araya::session::session_id{session}, {});
	}

	araya::task<std::optional<tool_result>> call(std::string name, boost::json::object arguments = {}) {
		std::string const tool = name;
		co_return co_await tools->invoke(
			tool,
			tool_context{
				.call_id = "c",
				.name = tool,
				.session = session,
				.arguments = std::move(arguments),
			});
	}
};

// A controllable stream producer: its output buffer is appended by the test and
// its settle callback captured for later invocation.
struct producer {
	std::shared_ptr<std::string> buffer = std::make_shared<std::string>();
	std::shared_ptr<std::size_t> cursor = std::make_shared<std::size_t>(0);
	std::shared_ptr<std::function<void(job_outcome)>> settle = std::make_shared<std::function<void(job_outcome)>>();

	void finish(job_outcome outcome) {
		if (*settle)
			(*settle)(std::move(outcome));
	}
};

job_start make_start(
	producer p,
	std::string kind,
	std::string label,
	std::optional<std::string> owner = std::optional<std::string>{"s1"}) {
	auto buffer = p.buffer;
	auto cursor = p.cursor;
	auto settle = p.settle;
	return job_start{
		.kind = std::move(kind),
		.label = std::move(label),
		.owner_session = std::move(owner),
		.run =
			[buffer, cursor, settle](std::function<void(job_outcome)> callback) {
				*settle = std::move(callback);
				return job_handle{
					.cancel = [](std::string const&) {},
					.read_output =
						[buffer, cursor] {
							auto text = buffer->substr(*cursor);
							*cursor = buffer->size();
							return text;
						},
				};
			},
	};
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

std::size_t notice_count(std::shared_ptr<araya::session::session_store> const& store, std::string const& id) {
	auto session = store->get(araya::session::session_id{id});
	if (!session)
		return 0;
	std::size_t count = 0;
	for (auto const& message : session->surface().messages()) {
		if (araya::llm_bridge::message_text(message).find("background job") != std::string::npos)
			++count;
	}
	return count;
}

} // namespace

TEST_CASE("job_list and job_output render status without waiting") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		auto empty = co_await r.call("job_list");
		REQUIRE(empty.has_value());
		CHECK(text_of(*empty) == "(no background jobs)");

		producer p;
		auto id = r.jobs->start(make_start(p, "bash", "echo hi"));
		CHECK(id == "bash-1");

		auto list = co_await r.call("job_list");
		REQUIRE(list.has_value());
		CHECK(text_of(*list).find("bash-1 [bash] running") != std::string::npos);

		auto idle = co_await r.call("job_output", {{"job_id", "bash-1"}});
		REQUIRE(idle.has_value());
		CHECK(text_of(*idle).find("(no new output)") != std::string::npos);
		CHECK(text_of(*idle).find("[status: running]") != std::string::npos);

		*p.buffer += "hello";
		auto read = co_await r.call("job_output", {{"job_id", "bash-1"}});
		REQUIRE(read.has_value());
		CHECK(text_of(*read).find("hello") != std::string::npos);

		p.finish(job_outcome{job_status::completed, "exit code: 0", {}});
		auto settled = co_await r.call("job_output", {{"job_id", "bash-1"}});
		REQUIRE(settled.has_value());
		CHECK(text_of(*settled).find("[status: completed, exit code: 0]") != std::string::npos);
	});
}

TEST_CASE("job_output wait blocks until settlement") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		producer p;
		auto id = r.jobs->start(make_start(p, "bash", "slow"));
		auto settle = p.settle;
		auto worker = [&h, settle]() -> araya::task<void> {
			boost::asio::steady_timer timer(h.io.get_executor());
			timer.expires_after(std::chrono::milliseconds(2));
			co_await timer.async_wait(boost::asio::use_awaitable);
			(*settle)(job_outcome{job_status::completed, "exit code: 0", {}});
		};
		boost::asio::co_spawn(h.io.get_executor(), worker(), boost::asio::detached);

		auto result = co_await r.call("job_output", {{"job_id", id}, {"wait", true}, {"timeout_ms", 5000}});
		REQUIRE(result.has_value());
		CHECK(text_of(*result).find("[status: completed, exit code: 0]") != std::string::npos);
	});
}

TEST_CASE("job_kill requests cancellation and reports already-finished") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		producer p;
		auto id = r.jobs->start(make_start(p, "bash", "kill me"));
		auto killed = co_await r.call("job_kill", {{"job_id", id}, {"reason", "done"}});
		REQUIRE(killed.has_value());
		CHECK(text_of(*killed) == "requested cancellation of job bash-1");
		CHECK(r.jobs->get(id, std::string{"s1"}).status == job_status::stopping);

		p.finish(job_outcome{job_status::killed, "signal: 9", {}});
		auto again = co_await r.call("job_kill", {{"job_id", id}});
		REQUIRE(again.has_value());
		CHECK(text_of(*again).find("had already finished") != std::string::npos);
	});
}

TEST_CASE("a settled job appends one completion notice to its owner's session") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		producer p;
		auto id = r.jobs->start(make_start(p, "bash", "notify me"));
		CHECK(notice_count(r.store, r.session) == 0);

		p.finish(job_outcome{job_status::completed, "exit code: 0", {}});
		CHECK(notice_count(r.store, r.session) == 1);

		auto session = r.store->get(araya::session::session_id{r.session});
		REQUIRE(session != nullptr);
		bool found = false;
		for (auto const& message : session->surface().messages()) {
			auto text = araya::llm_bridge::message_text(message);
			if (text.find("background job " + id) != std::string::npos &&
				text.find("Read its output with job_output") != std::string::npos)
				found = true;
		}
		CHECK(found);
	});
}

TEST_CASE("a reported job does not append a notice") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		producer p;
		auto id = r.jobs->start(make_start(p, "bash", "cancel me"));
		co_await r.call("job_kill", {{"job_id", id}});
		p.finish(job_outcome{job_status::killed, "signal: 9", {}});
		CHECK(notice_count(r.store, r.session) == 0);
	});
}

TEST_CASE("job controls reject foreign and unknown jobs") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		producer p;
		r.jobs->start(make_start(p, "bash", "private", std::optional<std::string>{"other"}));

		auto list = co_await r.call("job_list");
		REQUIRE(list.has_value());
		CHECK(text_of(*list) == "(no background jobs)");

		auto output = co_await r.call("job_output", {{"job_id", "bash-1"}});
		REQUIRE(output.has_value());
		CHECK(output->is_error);
		CHECK(text_of(*output).find("another session") != std::string::npos);

		auto unknown = co_await r.call("job_output", {{"job_id", "bash-99"}});
		REQUIRE(unknown.has_value());
		CHECK(unknown->is_error);
		CHECK(text_of(*unknown).find("unknown job") != std::string::npos);
	});
}
