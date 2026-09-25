#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/system-prompt/system_prompt.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/json/object.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The registry semantics ported from the harness: ordered/scoped
// sections, strict interpolation, complete sections, context suppression,
// tool ordering, and the interceptor seam.
namespace {

using namespace araya::system_prompt;

std::function<void(araya::plugin_context&, system_prompt_service&)> g_setup;

struct setup_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto service = ctx.require<system_prompt_service>(system_prompt_key).shared();
		if (g_setup)
			g_setup(ctx, *service);
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_setup(araya::plugin_config const&) { return std::make_unique<setup_plugin>(); }

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_setup_dep[]{{araya::service_id{"system-prompt", 1}, true}};
static const araya::plugin_descriptor g_setup_desc{"setup", g_setup_dep, g_no_provs, &make_setup};

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

struct rig {
	harness& h;
	araya::runtime& rt;
	std::shared_ptr<system_prompt_service> service;

	araya::task<void>
	mount(std::function<void(araya::plugin_context&, system_prompt_service&)> setup, araya::plugin_config config = {}) {
		co_await rt.mount(h.spec(&araya::system_prompt::plugin_descriptor(), std::move(config)));
		co_await rt.wait_idle();
		g_setup = std::move(setup);
		co_await rt.mount(h.spec(&g_setup_desc));
		co_await rt.wait_idle();
		service = rt.root_context().require<system_prompt_service>(system_prompt_key).shared();
	}
};

} // namespace

TEST_CASE("built-ins render identity then persona prefix and suffix") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_persona_prefix("You are a coding assistant.");
			svc.set_persona_suffix("cwd: {{cwd}}");
			svc.variable(ctx, "cwd", [](assemble_context const& c) { return std::optional<std::string>(c.cwd); });
		});
		auto assembly = r.service->assemble(assemble_context{.cwd = "/tmp"});
		CHECK(
			render_prompt(assembly) ==
			"You are an AI agent powered by DeepSeek Harness.\n\nYou are a coding assistant.\n\ncwd: /tmp");
	});
}

TEST_CASE("sections sort by order then name; empty sections disappear") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_include_harness_identity(false);
			svc.section(ctx, {"late", 100, "late"});
			svc.section(ctx, {"early", 10, "early"});
			svc.section(ctx, {"late-b", 100, "late-b"});
			svc.section(ctx, {"empty", 50, ""});
		});
		CHECK(render_prompt(r.service->assemble()) == "early\n\nlate\n\nlate-b");
	});
}

TEST_CASE("scoped sections shadow globals for that scope only") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_include_harness_identity(false);
			svc.section(ctx, {"x", 100, "global"});
			svc.section(ctx, {"x", 100, "scoped-a"}, std::string("scope-a"));
		});
		CHECK(render_prompt(r.service->assemble(assemble_context{.scope = "scope-a"})) == "scoped-a");
		CHECK(render_prompt(r.service->assemble(assemble_context{.scope = "scope-b"})) == "global");
		CHECK(render_prompt(r.service->assemble()) == "global");
	});
}

TEST_CASE("interpolation resolves registered variables") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_include_harness_identity(false);
			svc.variable(ctx, "model", [](assemble_context const& c) { return std::optional<std::string>(c.model); });
			svc.section(ctx, {"m", 100, "model={{model}}"});
		});
		CHECK(render_prompt(r.service->assemble(assemble_context{.model = "v4"})) == "model=v4");
	});
}

TEST_CASE("unknown and undefined variables throw at render") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_include_harness_identity(false);
			svc.variable(ctx, "unset", [](assemble_context const&) { return std::optional<std::string>{}; });
			svc.section(ctx, {"unknown", 100, "{{nope}}"});
			svc.section(ctx, {"unset", 200, "{{unset}}"});
		});
		CHECK_THROWS_AS(render_prompt(r.service->assemble()), std::invalid_argument);
	});
}

TEST_CASE("malformed references throw; a lone open brace is literal") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_include_harness_identity(false);
			svc.variable(ctx, "v", [](assemble_context const&) { return std::optional<std::string>("x"); });
			svc.section(ctx, {"malformed", 100, "{{bad name}}"});
			svc.section(ctx, {"literal", 200, "a {{ lone brace"});
		});
		CHECK_THROWS_AS(render_prompt(r.service->assemble()), std::invalid_argument);
	});
}

TEST_CASE("interpolate:false preserves literal braces") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_include_harness_identity(false);
			svc.section(ctx, {"literal", 100, "{{not_a_var}}", {}, /*interpolate=*/false});
		});
		CHECK(render_prompt(r.service->assemble()) == "{{not_a_var}}");
	});
}

TEST_CASE("a single complete section becomes the whole prompt") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_include_harness_identity(false);
			svc.section(ctx, {"other", 100, "other"});
			svc.section(ctx, {"solo", 200, "solo", {}, /*interpolate=*/true, /*complete=*/true});
		});
		CHECK(render_prompt(r.service->assemble()) == "solo");
	});
}

TEST_CASE("two complete sections throw") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.section(ctx, {"a", 100, "a", {}, true, true});
			svc.section(ctx, {"b", 200, "b", {}, true, true});
		});
		CHECK_THROWS_AS(r.service->assemble(), std::invalid_argument);
	});
}

TEST_CASE("contexts render into the superseding snapshot and can be suppressed") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.context(ctx, {"sandbox", 110, "sandbox on"});
		});
		CHECK(
			render_context_snapshot(r.service->assemble()) ==
			"Current runtime context. This snapshot supersedes earlier runtime-context snapshots.\n\nsandbox on");
	});
}

TEST_CASE("suppression and include_runtime_context=false empty the snapshot") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.context(ctx, {"sandbox", 110, "sandbox on"});
			svc.suppress_runtime_context(ctx);
		});
		CHECK(render_context_snapshot(r.service->assemble()).empty());
	});
}

TEST_CASE("tool schemas order lexicographically by default") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.tools(ctx, [](assemble_context const&) {
				return std::vector<tool_schema>{{"b", "", boost::json::object{}}, {"a", "", boost::json::object{}}};
			});
		});
		auto tools = r.service->assemble().tools;
		REQUIRE(tools.size() == 2);
		CHECK(tools[0].name == "a");
		CHECK(tools[1].name == "b");
	});
}

TEST_CASE("explicit tool order inserts unlisted tools at the rest marker") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_tool_order({"b", "<unlisted-tools>"});
			svc.tools(ctx, [](assemble_context const&) {
				return std::vector<tool_schema>{{"b", "", boost::json::object{}}, {"a", "", boost::json::object{}}};
			});
		});
		auto tools = r.service->assemble().tools;
		REQUIRE(tools.size() == 2);
		CHECK(tools[0].name == "b");
		CHECK(tools[1].name == "a");
	});
}

TEST_CASE("unknown tool-order names and reserved tool names throw") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_tool_order({"zzz", "<unlisted-tools>"});
			svc.tools(ctx, [](assemble_context const&) {
				return std::vector<tool_schema>{{"a", "", boost::json::object{}}};
			});
		});
		CHECK_THROWS_AS(r.service->assemble(), std::invalid_argument);
	});
}

TEST_CASE("interceptors mutate the assembly before any complete restore") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_include_harness_identity(false);
			svc.section(ctx, {"base", 100, "base"});
			svc.intercept(ctx, 0, [](prompt_assembly& a, assemble_context const&) {
				a.sections.push_back({"added", "added", true});
			});
		});
		CHECK(render_prompt(r.service->assemble()) == "base\n\nadded");
	});
}

TEST_CASE("a complete section is restored after interceptors") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h, rt};
		co_await r.mount([](araya::plugin_context& ctx, system_prompt_service& svc) {
			svc.set_include_harness_identity(false);
			svc.section(ctx, {"solo", 100, "solo", {}, true, true});
			svc.intercept(ctx, 0, [](prompt_assembly& a, assemble_context const&) {
				a.sections.push_back({"added", "added", true});
			});
		});
		CHECK(render_prompt(r.service->assemble()) == "solo");
	});
}
