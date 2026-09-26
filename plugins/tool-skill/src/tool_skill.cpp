#include "araya/tool-skill/tool_skill.hpp"

#include "araya/plugin_context.hpp"
#include "araya/session/store.hpp"
#include "araya/skill/skill.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::tool_skill {
namespace {

using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::skill::skill_definition;
using araya::skill::skill_summary;
using araya::skill::skills_key;
using araya::skill::skills_service;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;
using araya::tools::tools_key;
using araya::tools::tools_service;

std::string escape_text(std::string_view value) {
	std::string out;
	out.reserve(value.size());
	for (char const c : value) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		default:
			out.push_back(c);
		}
	}
	return out;
}

std::string escape_attr(std::string_view value) {
	std::string out;
	out.reserve(value.size());
	for (char const c : value) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '"':
			out += "&quot;";
			break;
		case '<':
			out += "&lt;";
			break;
		default:
			out.push_back(c);
		}
	}
	return out;
}

tool_result text_result(std::string text, bool is_error = false) {
	return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, is_error};
}

tool_result error_result(std::string text) { return text_result(std::move(text), true); }

// The canonical model-facing skill render, shared by the loader result.
std::string render_skill_content(skill_definition const& skill) {
	std::string base = std::filesystem::path(skill.summary.path).parent_path().string();
	std::string text = "<skill_content name=\"" + escape_attr(skill.summary.name) + "\">\n<skill_resources>\n";
	text += "Base directory for this skill: " + escape_text(base) + "\n";
	text += "Resolve relative paths mentioned by this skill against the base directory before using them. Load "
			"referenced resources only as needed.\n";
	text += "</skill_resources>\n\n<skill_instructions>\n";
	text += skill.content;
	text += "\n</skill_instructions>\n</skill_content>";
	return text;
}

// The session skill catalog rendered as one system-prompt section (the
// harness's `<available_skills>` frame). Empty when no model-invocable skill is
// available, which drops the section.
std::string render_catalog(std::vector<skill_summary> const& skills) {
	std::vector<skill_summary const*> visible;
	for (auto const& skill : skills) {
		if (skill.model_invocable)
			visible.push_back(&skill);
	}
	if (visible.empty())
		return {};
	std::string text;
	text += "<system-reminder>\n";
	text += "A skill is a reusable set of task-specific instructions. The following skills are available in this "
			"session:\n\n";
	text += "<available_skills>\n";
	for (auto const* skill : visible) {
		text += "- `" + skill->name + "`: " + escape_text(skill->description);
		if (skill->when_to_use && !skill->when_to_use->empty())
			text += " Use when: " + escape_text(*skill->when_to_use);
		text += "\n";
	}
	text += "</available_skills>\n\n";
	text += "If the user names a skill, or the task clearly matches a skill's description, call the `skill` tool "
			"with the exact skill name before taking task actions. Load all applicable skills, then follow their full "
			"instructions. This catalog contains summaries only; do not infer or follow a skill's instructions until "
			"it has been loaded.\n";
	text += "A user may also invoke a skill directly; its <skill_content> block then appears in this conversation. "
			"Follow it, and do not call the `skill` tool again for that skill.\n";
	text += "</system-reminder>";
	return text;
}

std::string cwd_for(std::shared_ptr<session_store> const& store, tool_context const& ctx) {
	if (!ctx.session.empty()) {
		if (auto session = store->get(session_id{ctx.session}); session && session->header().cwd)
			return *session->header().cwd;
	}
	return std::filesystem::current_path().string();
}

araya::task<tool_result>
handle_skill(std::shared_ptr<skills_service> skills, std::shared_ptr<session_store> store, tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto name = args ? araya::util::json::get_string(*args, "name") : std::string{};
	if (name.empty())
		co_return error_result("Error: skill requires a non-empty 'name'");
	auto const cwd = cwd_for(store, ctx);

	auto catalog = skills->list(cwd);
	auto found =
		std::find_if(catalog.begin(), catalog.end(), [&](skill_summary const& skill) { return skill.name == name; });
	if (found == catalog.end())
		co_return error_result("Error: skill \"" + name + "\" is unknown or no longer available");
	if (!found->model_invocable)
		co_return error_result("Error: skill \"" + name + "\" is not available for model invocation");
	auto loaded = skills->get(name, cwd);
	if (!loaded)
		co_return error_result("Error: skill \"" + name + "\" is unknown or no longer available");
	co_return text_result(render_skill_content(*loaded));
}

boost::json::value skill_schema() {
	boost::json::object name_property;
	name_property["type"] = "string";
	name_property["description"] = "The exact skill name from the available skills list.";
	boost::json::object properties;
	properties["name"] = std::move(name_property);
	boost::json::object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = boost::json::array{"name"};
	return schema;
}

struct tool_skill_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto skills = ctx.require<skills_service>(skills_key).shared();
		auto store = ctx.require<session_store>(sessions_key).shared();
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
		auto tools = ctx.require<tools_service>(tools_key).shared();

		{
			araya::system_prompt::prompt_section section;
			section.name = "skill:catalog";
			section.order = araya::system_prompt::section_order("SKILL_CATALOG");
			section.render = [skills](araya::system_prompt::assemble_context const& context) {
				return render_catalog(skills->list(context.cwd));
			};
			prompts->section(ctx, std::move(section));
		}

		tools->register_tool(
			ctx,
			tool_definition{
				"skill",
				"Load the full instructions for an available skill. Call this with the exact skill name from the "
				"session skill catalog before acting on a task that names or clearly matches that skill.",
				skill_schema()},
			[skills, store](tool_context const& call) { return handle_skill(skills, store, call); });
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_tool_skill(araya::plugin_config const& config) {
	(void)config;
	return std::make_unique<tool_skill_plugin>();
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"skills", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"tool-skill", g_deps, g_provs, &make_tool_skill};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tool_skill
