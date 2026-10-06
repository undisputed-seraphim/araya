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
	// Globals first, filtered by the restrictions in scope, then matching
	// scoped entries shadow by name; the map keeps the result name-sorted.
	std::map<std::string, tool_entry const*> merged;
	for (auto const& entry : tools_)
		if (!entry.scope && admits(entry.definition.name, scope))
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

bool tools_service::admits(std::string_view name, std::optional<std::string> const& scope) const {
	for (auto const& entry : restrictions_) {
		if (!entry.scope || !scope || *entry.scope != *scope)
			continue;
		if (entry.filter.allow &&
			std::find(entry.filter.allow->begin(), entry.filter.allow->end(), name) == entry.filter.allow->end())
			return false;
		if (entry.filter.deny &&
			std::find(entry.filter.deny->begin(), entry.filter.deny->end(), name) != entry.filter.deny->end())
			return false;
	}
	return true;
}

araya::registration
	tools_service::restrict(araya::plugin_context& caller, tool_restriction filter, std::optional<std::string> scope) {
	if (!scope || scope->empty())
		throw std::invalid_argument("tools: restrict requires a scope");
	if (!filter.allow && !filter.deny)
		throw std::invalid_argument("tools: restrict requires allow and/or deny");
	auto const known = [this](std::string_view name) {
		for (auto const& entry : tools_)
			if (!entry.scope && entry.definition.name == name)
				return true;
		return false;
	};
	for (auto const* names : {&filter.allow, &filter.deny}) {
		if (!*names)
			continue;
		for (auto const& name : **names)
			if (!known(name))
				throw std::invalid_argument("tools: restrict names unknown tool '" + name + "'");
	}
	auto const id = next_restriction_id_++;
	return caller.effect([this, id, scope = std::move(scope), filter = std::move(filter)]() -> araya::cleanup_action {
		restrictions_.push_back(restriction_entry{id, std::move(scope), std::move(filter)});
		return [this, id] { std::erase_if(restrictions_, [id](restriction_entry const& e) { return e.id == id; }); };
	});
}

araya::registration
tools_service::guard(araya::plugin_context& caller, tool_guard check, std::optional<std::string> scope) {
	if (!check)
		throw std::invalid_argument("tools: guard must not be empty");
	auto const id = next_guard_id_++;
	return caller.effect([this, id, scope = std::move(scope), check = std::move(check)]() -> araya::cleanup_action {
		guards_.push_back(guard_entry{id, std::move(scope), std::move(check)});
		return [this, id] { std::erase_if(guards_, [id](guard_entry const& e) { return e.id == id; }); };
	});
}

araya::registration tools_service::on_result(
	araya::plugin_context& caller,
	tool_result_listener listener,
	std::optional<std::string> scope) {
	if (!listener)
		throw std::invalid_argument("tools: result listener must not be empty");
	auto const id = next_result_id_++;
	return caller.effect(
		[this, id, scope = std::move(scope), listener = std::move(listener)]() -> araya::cleanup_action {
			result_listeners_.push_back(result_entry{id, std::move(scope), std::move(listener)});
			return [this, id] { std::erase_if(result_listeners_, [id](result_entry const& e) { return e.id == id; }); };
		});
}

std::optional<std::string>
tools_service::guard_reason(tool_context const& context, std::optional<std::string> const& scope) const {
	for (auto const& entry : guards_) {
		if (entry.scope && (!scope || *entry.scope != *scope))
			continue;
		if (auto reason = entry.check(context))
			return reason;
	}
	return std::nullopt;
}

void tools_service::notify_result(
	tool_context const& context,
	tool_result const& result,
	std::optional<std::string> const& scope) const {
	for (auto const& entry : result_listeners_) {
		if (entry.scope && (!scope || *entry.scope != *scope))
			continue;
		entry.listener(context, result);
	}
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
	if (auto reason = guard_reason(context, scope)) {
		tool_result denied = error_result(*reason);
		notify_result(context, denied, scope);
		co_return denied;
	}
	tool_result result = co_await found->handler(context);
	notify_result(context, result, scope);
	co_return result;
}

std::vector<system_prompt::tool_schema> tools_service::schemas(std::optional<std::string> const& scope) const {
	std::vector<system_prompt::tool_schema> out;
	for (auto const* entry : visible(scope))
		out.push_back(system_prompt::tool_schema{
			entry->definition.name, entry->definition.description, entry->definition.parameters});
	return out;
}

} // namespace araya::tools
