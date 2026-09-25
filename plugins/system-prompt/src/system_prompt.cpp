#include "araya/system-prompt/system_prompt.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace araya::system_prompt {
namespace {

// The harness's centrally-owned placement tables (SECTION_ORDERS /
// CONTEXT_ORDERS in packages/core/system-prompt/src/index.ts), verbatim.
struct order_entry {
	std::string_view name;
	int order;
};

constexpr order_entry k_section_orders[]{
	{"HARNESS_IDENTITY", -1000},
	{"DEPLOYMENT_PERSONA_PREFIX", 0},
	{"PLAN_POLICY", 500},
	{"TEAM_POLICY", 600},
	{"PTC_ONLY", 800},
	{"FILE_REFERENCE", 900},
	{"TOOL_BASH", 1000},
	{"TOOL_PWSH", 1010},
	{"TOOL_READ", 1100},
	{"TOOL_WRITE", 1200},
	{"TOOL_EDIT", 1300},
	{"TOOL_GLOB", 1400},
	{"TOOL_GREP", 1500},
	{"TOOL_JOBS", 1600},
	{"TOOL_PTY", 1700},
	{"TOOL_WEB_SEARCH", 2000},
	{"TOOL_WEB_FETCH", 2100},
	{"TOOL_LSP", 2200},
	{"TOOL_SESSION_QUERY", 2300},
	{"TOOL_GOAL", 2400},
	{"TOOL_CORDIS", 2500},
	{"TOOL_WORKFLOW", 2600},
	{"TOOL_RALPH", 2700},
	{"TOOL_SUBAGENT", 2800},
	{"TOOL_REPORT", 2900},
	{"TOOL_COMPUTER_USE", 3000},
	{"MCP_SERVERS", 3100},
	{"TOOLS_SDK", 5000},
	{"DELIVERABLE_FILE_REFERENCES", 9000},
	{"STRUCTURED_OUTPUT", 9900},
	{"HARNESS_SOURCE", 10000},
	{"WEB_SURFACE", 10100},
	{"DEPLOYMENT_PERSONA_SUFFIX", 10200},
};

constexpr order_entry k_context_orders[]{
	{"SANDBOX_POLICY", 110},
	{"APPROVAL_POLICY", 115},
	{"SUBAGENT_DELEGATION", 120},
};

template <std::size_t N>
int lookup(std::span<order_entry const, N> table, std::string_view name, std::string_view kind) {
	for (auto const& entry : table) {
		if (entry.name == name)
			return entry.order;
	}
	throw std::invalid_argument(
		"system-prompt: unknown " + std::string(kind) + " order name '" + std::string(name) + "'");
}

bool is_variable_name(std::string_view name) {
	if (name.empty() || name.front() < 'a' || name.front() > 'z')
		return false;
	for (char c : name) {
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
			continue;
		return false;
	}
	return true;
}

constexpr std::string_view k_reserved_tool = "<unlisted-tools>";

std::vector<tool_schema> order_tools(
	std::vector<tool_schema> tools,
	std::optional<std::vector<std::string>> const& order,
	std::vector<std::string> const& known) {
	for (auto const& tool : tools) {
		if (tool.name == k_reserved_tool)
			throw std::invalid_argument(
				"system-prompt: tool provider returned the reserved name '" + std::string(k_reserved_tool) + "'");
	}
	auto by_name = [](tool_schema const& a, tool_schema const& b) { return a.name < b.name; };
	if (!order) {
		std::sort(tools.begin(), tools.end(), by_name);
		return tools;
	}
	std::vector<std::string> known_sorted = known;
	for (auto const& name : *order) {
		if (name == k_reserved_tool)
			continue;
		if (std::find(known.begin(), known.end(), name) == known.end())
			throw std::invalid_argument("system-prompt: tool order lists unregistered tool '" + name + "'");
	}
	std::vector<tool_schema> rest;
	for (auto& tool : tools) {
		if (std::find(order->begin(), order->end(), tool.name) == order->end())
			rest.push_back(std::move(tool));
	}
	std::sort(rest.begin(), rest.end(), by_name);
	std::vector<tool_schema> result;
	for (auto const& name : *order) {
		if (name == k_reserved_tool) {
			for (auto& tool : rest)
				result.push_back(std::move(tool));
			continue;
		}
		auto it = std::find_if(tools.begin(), tools.end(), [&](tool_schema const& t) { return t.name == name; });
		if (it != tools.end())
			result.push_back(*it);
	}
	return result;
}

} // namespace

int section_order(std::string_view name) { return lookup(std::span(k_section_orders), name, "section"); }

int context_order(std::string_view name) { return lookup(std::span(k_context_orders), name, "context"); }

std::string interpolate(
	std::string_view text,
	std::map<std::string, std::optional<std::string>> const& variables,
	std::string_view kind,
	std::string_view name) {
	std::string result;
	std::size_t last = 0;
	std::size_t open = text.find("{{");
	while (open != std::string_view::npos) {
		std::size_t const close = text.find("}}", open + 2);
		std::string inner;
		bool matched = false;
		if (close != std::string_view::npos) {
			inner = std::string(text.substr(open + 2, close - (open + 2)));
			matched = inner.find_first_of("{}") == std::string::npos;
		}
		if (!matched) {
			// A later closing brace makes this malformed; otherwise literal prose.
			if (close != std::string_view::npos)
				throw std::invalid_argument(
					"malformed prompt variable reference at \"" + std::string(text.substr(open, 16)) + "…\" in " +
					std::string(kind) + " \"" + std::string(name) + "\"");
			result.append(text.substr(last, open + 2 - last));
			last = open + 2;
			open = text.find("{{", last);
			continue;
		}
		if (!is_variable_name(inner))
			throw std::invalid_argument(
				"malformed prompt variable reference \"{{" + inner + "}}\" in " + std::string(kind) + " \"" +
				std::string(name) + "\"");
		auto const found = variables.find(inner);
		if (found == variables.end())
			throw std::invalid_argument(
				"unknown prompt variable \"{{" + inner + "}}\" in " + std::string(kind) + " \"" + std::string(name) +
				"\"");
		if (!found->second)
			throw std::invalid_argument(
				"prompt variable \"{{" + inner + "}}\" has no value for this assembly (" + std::string(kind) + " \"" +
				std::string(name) + "\")");
		result.append(text.substr(last, open - last));
		result.append(*found->second);
		last = close + 2;
		open = text.find("{{", last);
	}
	result.append(text.substr(last));
	return result;
}

std::string render_prompt(prompt_assembly const& assembly) {
	std::string out;
	for (auto const& section : assembly.sections) {
		std::string const text =
			section.interpolate ? interpolate(section.text, assembly.variables, "section", section.name) : section.text;
		if (text.empty())
			continue;
		if (!out.empty())
			out += "\n\n";
		out += text;
	}
	return out;
}

std::string render_context_snapshot(prompt_assembly const& assembly) {
	std::string body;
	for (auto const& context : assembly.contexts) {
		std::string const text = interpolate(context.text, assembly.variables, "context", context.name);
		if (text.empty())
			continue;
		if (!body.empty())
			body += "\n\n";
		body += text;
	}
	if (body.empty())
		return {};
	return "Current runtime context. This snapshot supersedes earlier runtime-context snapshots.\n\n" + body;
}

araya::registration
system_prompt_service::section(araya::plugin_context& caller, prompt_section value, std::optional<std::string> scope) {
	if (value.name.empty())
		throw std::invalid_argument("system-prompt: section name must not be empty");
	auto const id = next_id_++;
	return caller.effect([this, id, scope = std::move(scope), value = std::move(value)]() -> araya::cleanup_action {
		sections_.push_back(owned_section{id, scope, std::move(value)});
		return [this, id] { std::erase_if(sections_, [id](owned_section const& e) { return e.id == id; }); };
	});
}

araya::registration
system_prompt_service::context(araya::plugin_context& caller, prompt_context value, std::optional<std::string> scope) {
	if (value.name.empty())
		throw std::invalid_argument("system-prompt: context name must not be empty");
	auto const id = next_id_++;
	return caller.effect([this, id, scope = std::move(scope), value = std::move(value)]() -> araya::cleanup_action {
		contexts_.push_back(owned_context{id, scope, std::move(value)});
		return [this, id] { std::erase_if(contexts_, [id](owned_context const& e) { return e.id == id; }); };
	});
}

araya::registration system_prompt_service::variable(
	araya::plugin_context& caller,
	std::string name,
	variable_provider provider,
	std::optional<std::string> scope) {
	if (!is_variable_name(name))
		throw std::invalid_argument("system-prompt: invalid variable name '" + name + "'");
	auto const id = next_id_++;
	return caller.effect(
		[this, id, scope = std::move(scope), name = std::move(name), provider = std::move(provider)]()
			-> araya::cleanup_action {
			variables_.push_back(owned_variable{id, scope, std::move(name), std::move(provider)});
			return [this, id] { std::erase_if(variables_, [id](owned_variable const& e) { return e.id == id; }); };
		});
}

araya::registration
system_prompt_service::tools(araya::plugin_context& caller, tool_provider provider, std::optional<std::string> scope) {
	auto const id = next_id_++;
	return caller.effect(
		[this, id, scope = std::move(scope), provider = std::move(provider)]() -> araya::cleanup_action {
			tool_providers_.push_back(owned_tools{id, scope, std::move(provider)});
			return [this, id] { std::erase_if(tool_providers_, [id](owned_tools const& e) { return e.id == id; }); };
		});
}

araya::registration
system_prompt_service::suppress_runtime_context(araya::plugin_context& caller, std::optional<std::string> scope) {
	auto const id = next_id_++;
	return caller.effect([this, id, scope = std::move(scope)]() -> araya::cleanup_action {
		suppressors_.push_back(owned_suppressor{id, scope});
		return [this, id] { std::erase_if(suppressors_, [id](owned_suppressor const& e) { return e.id == id; }); };
	});
}

araya::registration system_prompt_service::intercept(
	araya::plugin_context& caller,
	int order,
	assemble_interceptor fn,
	std::optional<std::string> scope) {
	auto const id = next_id_++;
	return caller.effect([this, id, sc = std::move(scope), order, fn = std::move(fn)]() -> araya::cleanup_action {
		interceptors_.push_back(owned_interceptor{id, sc, order, std::move(fn)});
		return [this, id] { std::erase_if(interceptors_, [id](owned_interceptor const& e) { return e.id == id; }); };
	});
}

prompt_assembly system_prompt_service::assemble(assemble_context const& context) const {
	auto const scope_matches = [&](std::optional<std::string> const& scope) {
		if (!scope)
			return true;
		return context.scope && *context.scope == *scope;
	};

	prompt_assembly assembly;
	// Variables: globals first, then matching scoped values shadow them.
	for (auto const& variable : variables_)
		if (!variable.scope)
			assembly.variables[variable.name] = variable.provider(context);
	for (auto const& variable : variables_)
		if (variable.scope && scope_matches(variable.scope))
			assembly.variables[variable.name] = variable.provider(context);

	// Sections: merge by name (scoped shadows global), built-ins first.
	std::map<std::string, prompt_section const*> merged;
	static prompt_section const identity{
		std::string(harness_identity_section),
		section_order("HARNESS_IDENTITY"),
		"You are an AI agent powered by DeepSeek Harness.",
		{},
		true,
		false,
	};
	if (include_harness_identity_)
		merged[identity.name] = &identity;
	prompt_section const prefix{
		std::string(persona_prefix_section),
		section_order("DEPLOYMENT_PERSONA_PREFIX"),
		persona_prefix_,
		{},
		true,
		false};
	prompt_section const suffix{
		std::string(persona_suffix_section),
		section_order("DEPLOYMENT_PERSONA_SUFFIX"),
		persona_suffix_,
		{},
		true,
		false};
	merged[prefix.name] = &prefix;
	merged[suffix.name] = &suffix;
	for (auto const& entry : sections_)
		if (!entry.scope)
			merged[entry.value.name] = &entry.value;
	for (auto const& entry : sections_)
		if (entry.scope && scope_matches(entry.scope))
			merged[entry.value.name] = &entry.value;

	std::vector<prompt_section const*> ordered;
	ordered.reserve(merged.size());
	for (auto const& [name, section] : merged)
		ordered.push_back(section);
	std::sort(ordered.begin(), ordered.end(), [](prompt_section const* a, prompt_section const* b) {
		if (a->order != b->order)
			return a->order < b->order;
		return a->name < b->name;
	});

	std::optional<assembled_section> complete;
	int complete_count = 0;
	for (auto const* section : ordered) {
		assembled_section as;
		as.name = section->name;
		as.text = section->render ? section->render(context) : section->text;
		as.interpolate = section->interpolate;
		assembly.sections.push_back(as);
		if (section->complete) {
			++complete_count;
			complete = std::move(as);
		}
	}
	if (complete_count > 1)
		throw std::invalid_argument("system-prompt: multiple complete prompt sections are active");

	// Contexts (unless suppressed).
	bool suppressed = !include_runtime_context_;
	for (auto const& suppressor : suppressors_)
		if (scope_matches(suppressor.scope))
			suppressed = true;
	if (!suppressed) {
		std::map<std::string, prompt_context const*> cmerged;
		for (auto const& entry : contexts_)
			if (!entry.scope)
				cmerged[entry.value.name] = &entry.value;
		for (auto const& entry : contexts_)
			if (entry.scope && scope_matches(entry.scope))
				cmerged[entry.value.name] = &entry.value;
		std::vector<prompt_context const*> cordered;
		for (auto const& [name, ctx] : cmerged)
			cordered.push_back(ctx);
		std::sort(cordered.begin(), cordered.end(), [](prompt_context const* a, prompt_context const* b) {
			if (a->order != b->order)
				return a->order < b->order;
			return a->name < b->name;
		});
		for (auto const* ctx : cordered) {
			assembled_context ac;
			ac.name = ctx->name;
			ac.text = ctx->render ? ctx->render(context) : ctx->text;
			assembly.contexts.push_back(std::move(ac));
		}
	}

	// Tool schemas: every matching provider contributes.
	std::vector<tool_schema> collected;
	std::vector<std::string> known;
	for (auto const& provider : tool_providers_) {
		if (!scope_matches(provider.scope))
			continue;
		for (auto& schema : provider.provider(context)) {
			known.push_back(schema.name);
			collected.push_back(std::move(schema));
		}
	}
	assembly.tools = order_tools(std::move(collected), tool_order_, known);

	// Interceptors, then the complete-section restore.
	std::vector<owned_interceptor const*> interceptors;
	for (auto const& interceptor : interceptors_)
		if (scope_matches(interceptor.scope))
			interceptors.push_back(&interceptor);
	std::sort(interceptors.begin(), interceptors.end(), [](owned_interceptor const* a, owned_interceptor const* b) {
		if (a->order != b->order)
			return a->order < b->order;
		return a->id < b->id;
	});
	for (auto const* interceptor : interceptors)
		interceptor->fn(assembly, context);
	if (complete)
		assembly.sections = {*complete};
	return assembly;
}

} // namespace araya::system_prompt
