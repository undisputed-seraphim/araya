#include "araya/tools/tools.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace araya::tools {

araya::registration tools_service::register_tool(
	araya::plugin_context& caller,
	tool_definition definition,
	tool_handler handler,
	std::optional<std::string> scope) {
	if (definition.name.empty())
		throw std::invalid_argument("tools: tool name must not be empty");
	for (auto const& entry : tools_) {
		if (entry.scope == scope && entry.definition.name == definition.name)
			throw std::invalid_argument("tools: duplicate tool '" + definition.name + "'");
	}
	auto const id = next_id_++;
	return caller.effect(
		[this, id, scope = std::move(scope), definition = std::move(definition), handler = std::move(handler)]()
			-> araya::cleanup_action {
			tools_.push_back(tool_entry{id, scope, std::move(definition), std::move(handler)});
			return [this, id] { std::erase_if(tools_, [id](tool_entry const& e) { return e.id == id; }); };
		});
}

std::vector<tools_service::tool_entry const*> tools_service::visible(std::optional<std::string> const& scope) const {
	// Globals first, then matching scoped entries shadow by name; the map
	// keeps the result name-sorted.
	std::map<std::string, tool_entry const*> merged;
	for (auto const& entry : tools_)
		if (!entry.scope)
			merged[entry.definition.name] = &entry;
	for (auto const& entry : tools_)
		if (entry.scope && scope && *scope == *entry.scope)
			merged[entry.definition.name] = &entry;
	std::vector<tool_entry const*> out;
	out.reserve(merged.size());
	for (auto const& [name, entry] : merged)
		out.push_back(entry);
	return out;
}

std::optional<tool_definition>
tools_service::find(std::string_view name, std::optional<std::string> const& scope) const {
	for (auto const* entry : visible(scope)) {
		if (entry->definition.name == name)
			return entry->definition;
	}
	return std::nullopt;
}

std::vector<tool_definition> tools_service::list(std::optional<std::string> const& scope) const {
	std::vector<tool_definition> out;
	for (auto const* entry : visible(scope))
		out.push_back(entry->definition);
	return out;
}

araya::task<std::optional<tool_result>>
tools_service::invoke(std::string_view name, tool_context context, std::optional<std::string> const& scope) const {
	tool_entry const* found = nullptr;
	for (auto const* entry : visible(scope)) {
		if (entry->definition.name == name) {
			found = entry;
			break;
		}
	}
	if (!found)
		co_return std::nullopt;
	co_return co_await found->handler(context);
}

std::vector<system_prompt::tool_schema> tools_service::schemas(std::optional<std::string> const& scope) const {
	std::vector<system_prompt::tool_schema> out;
	for (auto const* entry : visible(scope))
		out.push_back(system_prompt::tool_schema{
			entry->definition.name, entry->definition.description, entry->definition.parameters});
	return out;
}

} // namespace araya::tools
