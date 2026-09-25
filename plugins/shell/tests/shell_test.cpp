#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/shell/shell.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

#include <unistd.h>

namespace {

using namespace araya::tools;
using namespace std::chrono_literals;

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec shell_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::shell::plugin_descriptor(), std::move(cfg));
	}
};

struct rig {
	std::shared_ptr<tools_service> tools;
	std::string session = "s1";

	araya::task<void> mount(araya::runtime& rt, harness& h, std::filesystem::path const& cwd) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.shell_spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		auto store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		tools = root.require<tools_service>(tools_key).shared();
		araya::session::create_session_options options;
		options.cwd = cwd.string();
		store->create(root, araya::session::session_id{session}, options);
	}

	araya::task<std::optional<tool_result>> call(std::string command, boost::json::object extra = {}) {
		extra["command"] = std::move(command);
		co_return co_await tools->invoke(
			"bash", tool_context{.call_id = "c", .name = "bash", .session = session, .arguments = std::move(extra)});
	}

	araya::task<std::optional<tool_result>> call_with_stop(std::string command, std::stop_token stop) {
		co_return co_await tools->invoke(
			"bash",
			tool_context{
				.call_id = "c",
				.name = "bash",
				.session = session,
				.arguments = boost::json::object{{"command", std::move(command)}},
				.stop = stop,
			});
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

TEST_CASE("bash returns stdout and runs in the session workdir") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, std::filesystem::current_path());
		auto out = co_await r.call("printf 'hello\\n'");
		REQUIRE(out.has_value());
		CHECK_FALSE(out->is_error);
		CHECK(text_of(*out).find("hello") != std::string::npos);

		auto pwd = co_await r.call("pwd");
		REQUIRE(pwd.has_value());
		CHECK(text_of(*pwd).find(std::filesystem::current_path().filename().string()) != std::string::npos);
	});
}

TEST_CASE("bash forwards exit codes, stderr, and signals") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, std::filesystem::current_path());

		auto exit = co_await r.call("exit 3");
		REQUIRE(exit.has_value());
		CHECK_FALSE(exit->is_error);
		CHECK(text_of(*exit).find("[exit code: 3]") != std::string::npos);

		auto err = co_await r.call("echo oops 1>&2");
		REQUIRE(err.has_value());
		CHECK_FALSE(err->is_error);
		CHECK(text_of(*err).find("[stderr]") != std::string::npos);
		CHECK(text_of(*err).find("oops") != std::string::npos);

		auto signal = co_await r.call("kill -TERM $$");
		REQUIRE(signal.has_value());
		CHECK_FALSE(signal->is_error);
		CHECK(text_of(*signal).find("[killed by signal: 15]") != std::string::npos);
	});
}

TEST_CASE("bash kills a timed-out command") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, std::filesystem::current_path());
		auto out = co_await r.call("sleep 5", {{"timeoutMs", 200}});
		REQUIRE(out.has_value());
		CHECK_FALSE(out->is_error);
		CHECK(text_of(*out).find("[timed out after 200ms]") != std::string::npos);
	});
}

TEST_CASE("bash aborts on caller cancellation") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, std::filesystem::current_path());

		std::stop_source source;
		auto ex = co_await boost::asio::this_coro::executor;
		boost::asio::steady_timer timer(ex);
		timer.expires_after(150ms);
		timer.async_wait([&](boost::system::error_code) { source.request_stop(); });

		auto out = co_await r.call_with_stop("sleep 5", source.get_token());
		REQUIRE(out.has_value());
		CHECK(out->is_error);
		CHECK(text_of(*out).find("aborted") != std::string::npos);
	});
}

TEST_CASE("bash reports an empty argument as an error") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, std::filesystem::current_path());
		auto out = co_await r.call("");
		REQUIRE(out.has_value());
		CHECK(out->is_error);
		CHECK(text_of(*out).find("non-empty") != std::string::npos);
	});
}
