#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace araya::tools;

std::function<void(araya::plugin_context&, tools_service&)> g_setup;

struct setup_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto service = ctx.require<tools_service>(tools_key).shared();
		if (g_setup)
			g_setup(ctx, *service);
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_setup(araya::plugin_config const&) { return std::make_unique<setup_plugin>(); }

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_setup_dep[]{{araya::service_id{"tools", 1}, true}};
static const araya::plugin_descriptor g_setup_desc{"setup", g_setup_dep, g_no_provs, &make_setup};

araya::task<tool_result> echo_handler(tool_context const& ctx) {
	co_return tool_result{
		boost::json::array{{{"type", "text"}, {"text", "echo " + ctx.name}}},
		false,
	};
}

tool_definition echo_def(std::string description = "echoes") {
	return tool_definition{"echo", std::move(description), boost::json::object{}};
}

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

	araya::component_spec spec(araya::plugin_descriptor const* d) {
		return araya::component_spec{
			std::shared_ptr<araya::plugin_descriptor>(const_cast<araya::plugin_descriptor*>(d), [](auto*) {}),
			{},
			nullptr,
			""};
	}
};

struct rig {
	harness& h;
	araya::runtime& rt;
	std::shared_ptr<tools_service> tools;
	std::shared_ptr<araya::system_prompt::system_prompt_service> prompts;

	araya::task<void> mount(std::function<void(araya::plugin_context&, tools_service&)> setup) {
		co_await rt.mount(h.spec(&araya::system_prompt::plugin_descriptor()));
		co_await rt.wait_idle();
		co_await rt.mount(h.spec(&araya::tools::plugin_descriptor()));
		co_await rt.wait_idle();
		g_setup = std::move(setup);
		co_await rt.mount(h.spec(&g_setup_desc));
		co_await rt.wait_idle();
		auto root = rt.root_context();
		tools = root.require<tools_service>(tools_key).shared();
		prompts =
			root.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
	}
};

} // namespace

TEST_CASE("registers, lists sorted, and finds tools") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, tools_service& svc) {
			svc.register_tool(ctx, {"zeta", "z", boost::json::object{}}, &echo_handler);
			svc.register_tool(ctx, {"alpha", "a", boost::json::object{}}, &echo_handler);
		});
		auto list = r.tools->list();
		REQUIRE(list.size() == 2);
		CHECK(list[0].name == "alpha");
		CHECK(list[1].name == "zeta");
		CHECK(r.tools->find("zeta").has_value());
		CHECK_FALSE(r.tools->find("ghost").has_value());
	});
}

TEST_CASE("duplicate names in a scope throw; scoped entries shadow globals") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, tools_service& svc) {
			svc.register_tool(ctx, echo_def("global"), &echo_handler);
			CHECK_THROWS_AS(svc.register_tool(ctx, echo_def("dup"), &echo_handler), std::invalid_argument);
			// A different scope is a distinct namespace.
			svc.register_tool(ctx, {"echo", "scoped", boost::json::object{}}, &echo_handler, std::string("scope-a"));
		});
		CHECK(r.tools->find("echo", std::string("scope-a"))->description == "scoped");
		CHECK(r.tools->find("echo", std::string("scope-b"))->description == "global");
		CHECK(r.tools->find("echo")->description == "global");
	});
}

TEST_CASE("schemas feed the system-prompt assembly") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount(
			[](araya::plugin_context& ctx, tools_service& svc) { svc.register_tool(ctx, echo_def(), &echo_handler); });
		auto tools = r.prompts->assemble().tools;
		REQUIRE(tools.size() == 1);
		CHECK(tools[0].name == "echo");
		CHECK(tools[0].description == "echoes");
	});
}

TEST_CASE("invoke runs the visible handler; unknown tools return nullopt") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount(
			[](araya::plugin_context& ctx, tools_service& svc) { svc.register_tool(ctx, echo_def(), &echo_handler); });
		auto result = co_await r.tools->invoke("echo", tool_context{.name = "echo"});
		REQUIRE(result.has_value());
		CHECK(result->content == boost::json::array{{{"type", "text"}, {"text", "echo echo"}}});
		CHECK_FALSE(result->is_error);

		auto missing = co_await r.tools->invoke("ghost", tool_context{.name = "ghost"});
		CHECK_FALSE(missing.has_value());
	});
}
