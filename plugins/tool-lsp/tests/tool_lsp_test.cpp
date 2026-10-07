#include <catch2/catch_test_macros.hpp>

#include "araya/lsp/lsp.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/session_types.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tool-lsp/tool_lsp.hpp"
#include "araya/tools/tools.hpp"

#include "support/plugin_harness.hpp"

#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace {

using namespace araya::lsp;
using araya::tools::tool_context;
using araya::tools::tool_result;
using araya::tools::tools_key;
using araya::tools::tools_service;

struct fake_provider : lsp_provider {
	std::string_view id() const override { return "fake"; }
	std::map<std::string, std::string> const& extension_to_language() const override {
		static std::map<std::string, std::string> const mapping{{".ts", "typescript"}};
		return mapping;
	}
	araya::task<lsp_query_result> query(lsp_provider_query const& request, std::stop_token) override {
		lsp_query_result result;
		if (request.request.operation == lsp_operation::hover) {
			result.type = lsp_query_result::kind::hover;
			result.hover = lsp_hover{"Hover docs", std::nullopt};
		} else {
			result.type = lsp_query_result::kind::locations;
			result.resolved_workspace_uri = "file:///ws";
			result.locations.push_back(lsp_location{"file:///ws/a.ts", lsp_range{{0, 0}, {0, 1}}});
		}
		co_return result;
	}
};

std::string* g_session_id = nullptr;

// Registers a fake provider on `lsp` and creates one session whose cwd is the
// configured `cwd`.
struct setup_plugin : araya::plugin {
	araya::plugin_config config;
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto lsp = ctx.require<lsp_service>(lsp_key).shared();
		auto sessions = ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		lsp->register_provider(ctx, std::make_shared<fake_provider>());
		araya::session::create_session_options options;
		if (auto it = config.find("cwd"); it != config.end())
			options.cwd = it->second;
		auto session = sessions->create(ctx, araya::session::session_id{"test-session"}, std::move(options));
		if (g_session_id)
			*g_session_id = session->id().value;
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_setup(araya::plugin_config const& config) {
	auto plugin = std::make_unique<setup_plugin>();
	plugin->config = config;
	return plugin;
}

static const araya::dependency_spec setup_deps[]{
	{araya::service_id{"lsp", 1}, true, {}},
	{araya::service_id{"sessions", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> setup_provs{};
static const araya::plugin_descriptor setup_descriptor{"test-lsp-setup", setup_deps, setup_provs, &make_setup, {}};

struct harness : araya_test::plugin_harness {
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec lsp_spec() { return spec(&araya::lsp::plugin_descriptor()); }
	araya::component_spec setup_spec(std::string cwd) { return spec(&setup_descriptor, {{"cwd", std::move(cwd)}}); }
	araya::component_spec tool_spec() { return spec(&araya::tool_lsp::plugin_descriptor()); }
};

std::string text_of(tool_result const& result) {
	auto const* array = result.content.if_array();
	if (!array || array->empty())
		return {};
	auto const* object = array->front().if_object();
	if (!object)
		return {};
	auto const it = object->find("text");
	return it != object->end() && it->value().is_string() ? std::string(it->value().as_string()) : std::string{};
}

} // namespace

TEST_CASE("tool-lsp renders locations and hover from the session workspace") {
	harness h;
	std::string session_id;
	g_session_id = &session_id;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.lsp_spec());
		co_await rt.mount(h.setup_spec("/workspace"));
		co_await rt.mount(h.tool_spec());
		co_await rt.wait_idle();
		auto const tools = rt.root_context().require<tools_service>(tools_key).shared();

		tool_context definition;
		definition.name = "lsp";
		definition.session = session_id;
		definition.arguments =
			boost::json::object{{"operation", "goToDefinition"}, {"file_path", "a.ts"}, {"line", 1}, {"character", 1}};
		auto const located = co_await tools->invoke("lsp", definition);
		REQUIRE(located.has_value());
		CHECK_FALSE(located->is_error);
		CHECK(text_of(*located).find("a.ts:1:1") != std::string::npos);

		tool_context hover;
		hover.name = "lsp";
		hover.session = session_id;
		hover.arguments =
			boost::json::object{{"operation", "hover"}, {"file_path", "a.ts"}, {"line", 1}, {"character", 1}};
		auto const hovered = co_await tools->invoke("lsp", hover);
		REQUIRE(hovered.has_value());
		CHECK_FALSE(hovered->is_error);
		CHECK(text_of(*hovered) == "Hover docs");
	});
	g_session_id = nullptr;
}

TEST_CASE("tool-lsp reports invalid arguments and a missing workspace") {
	harness h;
	std::string session_id;
	g_session_id = &session_id;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.lsp_spec());
		co_await rt.mount(h.setup_spec("/workspace"));
		co_await rt.mount(h.tool_spec());
		co_await rt.wait_idle();
		auto const tools = rt.root_context().require<tools_service>(tools_key).shared();

		tool_context invalid;
		invalid.name = "lsp";
		invalid.session = session_id;
		invalid.arguments =
			boost::json::object{{"operation", "nonsense"}, {"file_path", "a.ts"}, {"line", 1}, {"character", 1}};
		auto const bad = co_await tools->invoke("lsp", invalid);
		REQUIRE(bad.has_value());
		CHECK(bad->is_error);

		tool_context no_session;
		no_session.name = "lsp";
		no_session.arguments =
			boost::json::object{{"operation", "hover"}, {"file_path", "a.ts"}, {"line", 1}, {"character", 1}};
		auto const missing = co_await tools->invoke("lsp", no_session);
		REQUIRE(missing.has_value());
		CHECK(missing->is_error);
		CHECK(text_of(*missing).find("workspace") != std::string::npos);
	});
	g_session_id = nullptr;
}
