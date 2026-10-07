#include <catch2/catch_test_macros.hpp>

#include "araya/mcp-client/mcp.hpp"
#include "araya/mcp-resources/mcp_resources.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"

#include "support/plugin_harness.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace araya::tools;

// A canned MCP provider: no process, no transport; returns fixed protocol
// results keyed by method.
struct fake_service : araya::mcp::mcp_service {
	std::vector<std::string> names;
	std::map<std::string, boost::json::value> canned;

	std::vector<std::string> servers() const override { return names; }

	araya::task<boost::json::value>
	request(std::string const& server, std::string_view method, boost::json::value, std::stop_token) override {
		if (std::find(names.begin(), names.end(), server) == names.end())
			throw std::runtime_error("unknown MCP server '" + server + "'");
		auto const it = canned.find(std::string(method));
		if (it == canned.end())
			throw std::runtime_error("no canned response for " + std::string(method));
		co_return it->second;
	}
};

struct fake_plugin : araya::plugin {
	std::shared_ptr<fake_service> service;

	araya::task<void> apply(araya::plugin_context& ctx) override {
		ctx.provide(araya::mcp::mcp_key, std::shared_ptr<araya::mcp::mcp_service>{service});
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_fake(araya::plugin_config const& config) {
	auto plugin = std::make_unique<fake_plugin>();
	plugin->service = std::make_shared<fake_service>();
	if (config.find("empty") == config.end()) {
		plugin->service->names = {"alpha"};
		plugin->service->canned["resources/list"] = boost::json::object{
			{"resources",
			 boost::json::array{
				 boost::json::object{{"uri", "file:///a"}, {"name", "A"}, {"mimeType", "text/plain"}},
				 boost::json::object{{"uri", "file:///b"}, {"blob", std::string(8, 'Q')}}}}};
		plugin->service->canned["resources/templates/list"] = boost::json::object{
			{"resourceTemplates",
			 boost::json::array{boost::json::object{{"uriTemplate", "file:///{p}"}, {"name", "T"}}}}};
		plugin->service->canned["resources/read"] = boost::json::object{
			{"contents",
			 boost::json::array{
				 boost::json::object{{"uri", "file:///a"}, {"mimeType", "text/plain"}, {"text", "hello resource"}}}}};
	}
	return plugin;
}

static const araya::provision_spec fake_provs[]{{araya::service_id{"mcp", 1}}};
static const araya::plugin_descriptor fake_descriptor{"fake-mcp", {}, fake_provs, &make_fake, {}};

struct harness : araya_test::plugin_harness {
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec fake_spec(araya::plugin_config cfg = {}) { return spec(&fake_descriptor, std::move(cfg)); }
	araya::component_spec resources_spec() { return spec(&araya::mcp_resources::plugin_descriptor()); }
};

struct rig {
	std::shared_ptr<tools_service> tools;
	std::shared_ptr<araya::system_prompt::system_prompt_service> prompts;

	araya::task<void> mount(araya::runtime& rt, harness& h, araya::plugin_config fake_config = {}) {
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.fake_spec(std::move(fake_config)));
		co_await rt.mount(h.resources_spec());
		co_await rt.wait_idle();
		tools = rt.root_context().require<tools_service>(tools_key).shared();
		prompts = rt.root_context()
					  .require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key)
					  .shared();
	}

	araya::task<std::optional<tool_result>> call(std::string name, boost::json::object args = {}) {
		std::string const tool = name;
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

TEST_CASE("mcp resources lists resources and projects blobs") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto out = co_await r.call("list_mcp_resources", {{"server", "alpha"}});
		REQUIRE(out.has_value());
		CHECK_FALSE(out->is_error);
		std::string const text = text_of(*out);
		CHECK(text.find("MCP server: alpha") != std::string::npos);
		CHECK(text.find("file:///a") != std::string::npos);
		CHECK(text.find("file:///b") != std::string::npos);
		CHECK(
			text.find("[binary resource: 8 base64 characters; available to programmatic callers]") !=
			std::string::npos);
	});
}

TEST_CASE("mcp resources reads a resource and requires the uri") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto out = co_await r.call("read_mcp_resource", {{"server", "alpha"}, {"uri", "file:///a"}});
		REQUIRE(out.has_value());
		CHECK_FALSE(out->is_error);
		CHECK(text_of(*out).find("hello resource") != std::string::npos);

		auto missing = co_await r.call("read_mcp_resource", {{"server", "alpha"}});
		REQUIRE(missing.has_value());
		CHECK(missing->is_error);
		CHECK(text_of(*missing).find("uri") != std::string::npos);
	});
}

TEST_CASE("mcp resources reports an unconfigured server") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto out = co_await r.call("list_mcp_resources", {{"server", "beta"}});
		REQUIRE(out.has_value());
		CHECK(out->is_error);
		CHECK(text_of(*out).find("unknown MCP server") != std::string::npos);
	});
}

TEST_CASE("mcp resources adds the server-name prompt section") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto const assembly = r.prompts->assemble({});
		bool found = false;
		for (auto const& section : assembly.sections) {
			if (section.text.find("## MCP resource servers") != std::string::npos) {
				found = true;
				CHECK(section.text.find("\"alpha\"") != std::string::npos);
				CHECK(section.text.find("list_mcp_resources") != std::string::npos);
			}
		}
		CHECK(found);
	});
}

TEST_CASE("mcp resources registers nothing when no server is configured") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, {{"empty", "true"}});
		CHECK_FALSE(r.tools->find("list_mcp_resources").has_value());
		CHECK_FALSE(r.tools->find("read_mcp_resource").has_value());
		auto const assembly = r.prompts->assemble({});
		for (auto const& section : assembly.sections)
			CHECK(section.text.find("## MCP resource servers") == std::string::npos);
	});
}
