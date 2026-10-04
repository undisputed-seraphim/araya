#include "araya/tool-workflow/tool_workflow.hpp"

#include "araya/config.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"
#include "araya/workflow/workflow.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace araya::tool_workflow {
namespace {

using araya::tools::text_result;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;
using araya::workflow::start_request;
using araya::workflow::workflow_service;

constexpr araya::config_key<std::string> tool_name_key{"tool_name"};
constexpr araya::config_key<bool> enabled_key{"enabled"};
constexpr araya::config_key<std::uint64_t> max_result_key{"max_result_chars"};

constexpr araya::config_field g_config[] = {
	field(tool_name_key, "Model-facing tool name.", "workflow"),
	field(enabled_key, "Register the model-facing tool (needs Node).", "false"),
	field(max_result_key, "Maximum characters of a workflow result returned to the model.", "50000"),
};

struct tool_workflow_config {
	std::string tool_name = "workflow";
	bool enabled = false;
	std::uint64_t max_result_chars = 50000;
};

tool_workflow_config parse_config(araya::plugin_config const& config) {
	araya::plugin_config_view view(config);
	tool_workflow_config out;
	if (auto value = view.try_get(tool_name_key))
		out.tool_name = *value;
	if (auto value = view.try_get(enabled_key))
		out.enabled = *value;
	if (auto value = view.try_get(max_result_key))
		out.max_result_chars = *value;
	return out;
}

// The script-authoring contract, embedded in the tool description (ported from
// the harness's tool-workflow description).
constexpr std::string_view description =
	"Run a JavaScript workflow script that orchestrates subagents at scale. Use this for work that fans out across "
	"many independent pieces - an audit over many files, a migration, multi-angle research, adversarial "
	"verification of findings - where you write the orchestration as a script instead of delegating turn by turn.\n\n"
	"The workflow's identity rides the `meta` parameter as JSON: required `name` (short kebab-case) and "
	"`description` strings, optional `whenToUse` string and `phases` array "
	"(`{title, detail?, provider?, model?}`). The `script` parameter is the plain JavaScript body ONLY (NOT "
	"TypeScript, and NO `export const meta` statement - meta is a parameter, not code), running with top-level "
	"await; end with `return <value>` - the value must be JSON-serializable and is this tool's result.\n\n"
	"Script-body hooks:\n"
	"- `agent(prompt, opts?): Promise<any>` - run one subagent to completion. Without `opts.schema` it resolves to "
	"the child's final text; with `opts.schema` (an object-rooted JSON Schema using ONLY "
	"type/properties/required/additionalProperties/items/enum/const/oneOf) it resolves to the validated object. "
	"Resolves `null` when the child fails (filter with `.filter(Boolean)`). Other opts: `label`, `phase`, and "
	"independent `provider`/`model` LLM target overrides. Anything else is rejected loudly.\n"
	"- `pipeline(items, ...stages): Promise<any[]>` - run each item through the stages independently with NO "
	"barrier between stages. Each stage receives `(prev, item, index)`. An ordinary stage throw drops that ITEM to "
	"`null` and skips its remaining stages.\n"
	"- `parallel(thunks): Promise<any[]>` - run zero-argument functions concurrently and await ALL of them. A "
	"throwing thunk resolves to `null`.\n"
	"- `phase(title)` - start a progress phase; `log(message)` - narrate progress; `args` - the tool call's `args` "
	"input, verbatim.\n\n"
	"Misused hooks (bad arguments, unknown options, unsupported schemas, tripped caps) throw errors that ALWAYS "
	"kill the script - they never dissolve into a per-item `null`.\n\n"
	"Constraints: concurrency and total-agent caps apply; no filesystem, network, timers, or Node.js APIs are "
	"provided - the agents do the work, the script only coordinates them. The run executes in the foreground: this "
	"call returns when the whole script finishes.";

boost::json::value workflow_schema() {
	boost::json::object name{{"type", "string"}, {"description", "Short kebab-case workflow name."}};
	boost::json::object meta_description{
		{"type", "string"}, {"description", "One-line description of what the workflow does."}};
	boost::json::object when_to_use{
		{"type", "string"}, {"description", "Optional guidance on when this workflow applies."}};
	boost::json::object phase_title{
		{"type", "string"}, {"description", "The phase title phase() calls match by exact string."}};
	boost::json::object phase_detail{
		{"type", "string"}, {"description", "Optional one-line description of the phase."}};
	boost::json::object phase_provider{
		{"type", "string"}, {"description", "Optional provider override this phase is expected to use."}};
	boost::json::object phase_model{
		{"type", "string"}, {"description", "Optional model override this phase is expected to use."}};
	boost::json::object phase{
		{"type", "object"},
		{"additionalProperties", true},
		{"properties",
		 boost::json::object{
			 {"title", std::move(phase_title)},
			 {"detail", std::move(phase_detail)},
			 {"provider", std::move(phase_provider)},
			 {"model", std::move(phase_model)}}},
		{"required", boost::json::array{"title"}}};
	boost::json::object meta{
		{"type", "object"},
		{"additionalProperties", true},
		{"properties",
		 boost::json::object{
			 {"name", std::move(name)},
			 {"description", std::move(meta_description)},
			 {"whenToUse", std::move(when_to_use)},
			 {"phases", boost::json::object{{"type", "array"}, {"items", std::move(phase)}}}}},
		{"required", boost::json::array{"name", "description"}}};
	boost::json::object script{
		{"type", "string"},
		{"description",
		 "The plain-JS workflow script body (top-level await allowed; NO `export const meta` statement; end with "
		 "`return <json-value>`)."}};
	boost::json::object args{
		{"type", "object"},
		{"additionalProperties", true},
		{"description",
		 "Optional JSON input exposed to the script as the `args` global (wrap a bare list as a field, e.g. "
		 "{\"files\": [...]})."}};
	return boost::json::object{
		{"type", "object"},
		{"properties",
		 boost::json::object{{"script", std::move(script)}, {"meta", std::move(meta)}, {"args", std::move(args)}}},
		{"required", boost::json::array{"script", "meta"}}};
}

std::string render_value(boost::json::value const& value, std::uint64_t max_chars) {
	auto const rendered = boost::json::serialize(value);
	if (rendered.size() <= max_chars)
		return rendered;
	return rendered.substr(0, static_cast<std::size_t>(max_chars)) +
		   "\n... [truncated: " + std::to_string(rendered.size() - max_chars) + " more characters]";
}

araya::task<tool_result> workflow_handler(
	std::shared_ptr<workflow_service> engine,
	std::shared_ptr<tool_workflow_config const> cfg,
	tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	if (!args)
		co_return text_result("Error: arguments must be a JSON object", true);
	auto const script = araya::util::json::get_string(*args, "script");
	if (script.empty())
		co_return text_result("Error: workflow requires a non-empty 'script'", true);
	auto const* meta = args->if_contains("meta") ? args->if_contains("meta")->if_object() : nullptr;
	if (!meta)
		co_return text_result("Error: workflow requires a 'meta' object", true);
	auto const name = araya::util::json::get_string(*meta, "name");
	auto const meta_text = araya::util::json::get_string(*meta, "description");
	if (name.empty() || meta_text.empty())
		co_return text_result("Error: meta requires non-empty 'name' and 'description'", true);
	if (ctx.session.empty())
		co_return text_result("Error: workflow requires a calling session", true);

	start_request request;
	request.script = script;
	request.meta = *args->if_contains("meta");
	if (auto const* call_args = args->if_contains("args"))
		request.args = *call_args;
	else
		request.args = boost::json::value(nullptr);
	request.parent = ctx.session;
	request.stop = ctx.stop;

	auto result = co_await engine->run(std::move(request));
	if (result.stop_reason == "cancelled") {
		co_return text_result("Error: workflow run was cancelled", true);
	}
	if (result.stop_reason != "completed") {
		co_return text_result("Error: workflow run failed: " + result.error.value_or("unknown error"), true);
	}
	std::string text = "workflow \"" + name + "\" completed (" + std::to_string(result.agents_started) + " agent" +
					   (result.agents_started == 1 ? "" : "s") + ").\nReturn value:\n" +
					   render_value(result.value.value_or(boost::json::value(nullptr)), cfg->max_result_chars);
	co_return text_result(std::move(text));
}

struct tool_workflow_plugin : araya::plugin {
	explicit tool_workflow_plugin(araya::plugin_config config)
		: config_(parse_config(config)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		// Off by default: the workflow engine requires a Node runtime, so the
		// model-facing tool is opt-in (config `enabled=true`).
		if (!config_.enabled) {
			co_return;
		}
		auto tools = ctx.require<araya::tools::tools_service>(araya::tools::tools_key).shared();
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
		auto engine = ctx.require<workflow_service>(araya::workflow::workflow_key).shared();

		araya::system_prompt::prompt_section section;
		section.name = "tool:" + config_.tool_name;
		section.order = araya::system_prompt::section_order("TOOL_WORKFLOW");
		section.text = "Use the " + config_.tool_name +
					   " tool ONLY when the user explicitly asks for a workflow or for large multi-agent "
					   "orchestration: you write a JavaScript script (the tool description documents the exact "
					   "format) that fans work out across many subagents with phases and structured results. For "
					   "one or two delegations, prefer plain subagent calls.";
		prompts->section(ctx, std::move(section));

		auto cfg = std::make_shared<tool_workflow_config const>(config_);
		tools->register_tool(
			ctx,
			tool_definition{config_.tool_name, std::string(description), workflow_schema()},
			[engine, cfg](tool_context const& call) { return workflow_handler(engine, cfg, call); });
		co_return;
	}

private:
	tool_workflow_config config_;
};

std::unique_ptr<araya::plugin> make_tool_workflow(araya::plugin_config const& config) {
	return std::make_unique<tool_workflow_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"workflow", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"tool-workflow", g_deps, g_provs, &make_tool_workflow, g_config};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tool_workflow
