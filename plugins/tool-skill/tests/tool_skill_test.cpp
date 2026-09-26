#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/skill/skill.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tool-skill/tool_skill.hpp"
#include "araya/tools/tools.hpp"

#include "support/plugin_harness.hpp"

#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using namespace araya::tools;

struct workspace {
	fs::path root;

	workspace() {
		std::error_code ec;
		auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		root = fs::temp_directory_path(ec) / ("araya-tool-skill-" + std::to_string(stamp));
		fs::remove_all(root, ec);
		fs::create_directories(root / ".git", ec);
	}

	~workspace() {
		std::error_code ec;
		fs::remove_all(root, ec);
	}

	void write(fs::path const& relative, std::string const& content) {
		std::error_code ec;
		fs::create_directories((root / relative).parent_path(), ec);
		std::ofstream stream(root / relative, std::ios::binary);
		stream << content;
	}
};

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec skill_spec() { return spec(&araya::skill::plugin_descriptor()); }
	araya::component_spec tool_skill_spec() { return spec(&araya::tool_skill::plugin_descriptor()); }
};

struct rig {
	std::shared_ptr<tools_service> tools;
	std::shared_ptr<araya::system_prompt::system_prompt_service> prompts;
	std::string session = "s1";

	araya::task<void> mount(araya::runtime& rt, harness& h, fs::path const& cwd) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.skill_spec());
		co_await rt.mount(h.tool_skill_spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		auto store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		tools = root.require<tools_service>(tools_key).shared();
		prompts =
			root.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
		araya::session::create_session_options options;
		options.cwd = cwd.string();
		store->create(root, araya::session::session_id{session}, options);
	}

	araya::task<std::optional<tool_result>> call(std::string name) {
		std::string const tool = "skill";
		co_return co_await tools->invoke(
			tool,
			tool_context{
				.call_id = "c",
				.name = tool,
				.session = session,
				.arguments = boost::json::object{{"name", std::move(name)}},
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

std::string
section_text(std::shared_ptr<araya::system_prompt::system_prompt_service> const& prompts, std::string const& cwd) {
	araya::system_prompt::assemble_context context;
	context.cwd = cwd;
	for (auto const& section : prompts->assemble(context).sections) {
		if (section.name == "skill:catalog")
			return section.text;
	}
	return {};
}

} // namespace

TEST_CASE("the skill tool loads a discovered skill and the catalog lists it") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		workspace ws;
		ws.write(
			".agents/skills/deep-dive/SKILL.md",
			"---\nname: deep-dive\ndescription: Investigate a topic thoroughly.\nwhenToUse: for research\n---\n"
			"Always cite sources.\n");
		ws.write(
			".agents/skills/secret/SKILL.md",
			"---\nname: secret\ndescription: Model must not see this.\ndisable-model-invocation: true\n---\n"
			"hidden\n");

		rig r;
		co_await r.mount(rt, h, ws.root);

		auto catalog = section_text(r.prompts, ws.root.string());
		CHECK(catalog.find("<available_skills>") != std::string::npos);
		CHECK(catalog.find("`deep-dive`: Investigate a topic thoroughly.") != std::string::npos);
		CHECK(catalog.find("for research") != std::string::npos);
		CHECK(catalog.find("secret") == std::string::npos);

		auto loaded = co_await r.call("deep-dive");
		REQUIRE(loaded.has_value());
		CHECK_FALSE(loaded->is_error);
		CHECK(text_of(*loaded).find("<skill_content name=\"deep-dive\">") != std::string::npos);
		CHECK(text_of(*loaded).find("Base directory for this skill:") != std::string::npos);
		CHECK(text_of(*loaded).find("Always cite sources.") != std::string::npos);
	});
}

TEST_CASE("the skill tool rejects unknown and model-disabled skills") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		workspace ws;
		ws.write(
			".agents/skills/secret/SKILL.md",
			"---\nname: secret\ndescription: hidden\ndisable-model-invocation: true\n---\nhidden\n");

		rig r;
		co_await r.mount(rt, h, ws.root);

		auto unknown = co_await r.call("missing");
		REQUIRE(unknown.has_value());
		CHECK(unknown->is_error);

		auto secret = co_await r.call("secret");
		REQUIRE(secret.has_value());
		CHECK(secret->is_error);
		CHECK(text_of(*secret).find("not available for model invocation") != std::string::npos);
	});
}

TEST_CASE("an empty skill set drops the catalog section") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		workspace ws;
		rig r;
		co_await r.mount(rt, h, ws.root);
		CHECK(section_text(r.prompts, ws.root.string()).empty());
	});
}
