#include "araya/subagents/subagents.hpp"

#include "araya/llm/bridge.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace araya::subagents {
namespace {

// The last assistant message's text in a session, or empty. The one-shot
// result and a continuable settlement both carry this.
std::string last_assistant_text(araya::session::session const& s) {
	auto const& messages = s.surface().messages();
	for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
		if (it->role != araya::session::message_role::assistant)
			continue;
		auto text = araya::llm_bridge::message_text(*it);
		if (!text.empty())
			return text;
	}
	return {};
}

// The built-in in-process spawn backend: a fresh child session with no
// seed, inheriting the parent's cwd and preset, at parent depth + 1.
class spawn_provider : public subagent_provider {
public:
	explicit spawn_provider(std::shared_ptr<araya::session::session_store> store)
		: store_(std::move(store)) {}

	capabilities caps() const override { return capabilities{}; }

	std::shared_ptr<araya::session::session>
	create_child(start_request const& req, std::uint32_t child_depth) override {
		using namespace araya::session;
		create_session_options options;
		options.origin = session_origin::subagent;
		options.parent_session = session_id{req.parent};
		options.delegation_depth = child_depth;
		if (auto parent = store_->get(session_id{req.parent})) {
			options.cwd = parent->header().cwd;
			options.agent_preset = parent->header().agent_preset;
		}
		auto child = store_->prepare(store_->mint_id(), std::move(options));
		store_->enter(child);
		store_->announce(*child);
		return child;
	}

private:
	std::shared_ptr<araya::session::session_store> store_;
};

result error_result(std::string message) {
	result r;
	r.is_error = true;
	r.stop_reason = "error";
	r.error = std::move(message);
	return r;
}

// -- structured output -----------------------------------------------------
//
// The JSON-Schema subset the harness documents for script/agent schemas:
// type (object/array/string/number/integer/boolean/null), properties,
// required, additionalProperties, items, enum, const, oneOf. Unknown
// keywords are ignored (permissive), matching the seam's "assert the subset"
// posture; native constrained decoding is a later milestone.

constexpr std::string_view k_structured_tool = "structured_output";

constexpr std::string_view k_structured_instruction =
	"When you have your final answer, you MUST report it by calling the `structured_output` tool with arguments "
	"matching its parameter schema exactly. Do not finish with a plain text answer: only the tool call counts as "
	"your result.";

bool type_matches(boost::json::value const& value, std::string_view type) {
	if (type == "object")
		return value.is_object();
	if (type == "array")
		return value.is_array();
	if (type == "string")
		return value.is_string();
	if (type == "boolean")
		return value.is_bool();
	if (type == "null")
		return value.is_null();
	if (type == "number")
		return value.is_number();
	if (type == "integer")
		return value.is_int64() || value.is_uint64();
	return true;
}

void validate_into(
	boost::json::value const& value,
	boost::json::value const& schema,
	std::string const& path,
	std::vector<std::string>& out) {
	auto const* rules = schema.if_object();
	if (!rules)
		return; // permissive: a non-object schema constrains nothing
	if (auto const* constant = rules->if_contains("const")) {
		if (value != *constant)
			out.push_back(path + ": does not equal the required constant");
	}
	if (auto const* allowed = rules->if_contains("enum")) {
		if (auto const* array = allowed->if_array()) {
			bool found = false;
			for (auto const& item : *array) {
				if (item == value) {
					found = true;
					break;
				}
			}
			if (!found)
				out.push_back(path + ": is not one of the allowed values");
		}
	}
	if (auto const* variants = rules->if_contains("oneOf")) {
		if (auto const* array = variants->if_array()) {
			bool any = false;
			for (auto const& variant : *array) {
				std::vector<std::string> scratch;
				validate_into(value, variant, path, scratch);
				if (scratch.empty()) {
					any = true;
					break;
				}
			}
			if (!any)
				out.push_back(path + ": matches none of the oneOf variants");
		}
	}
	if (auto const* type = rules->if_contains("type")) {
		bool ok = true;
		if (type->is_string()) {
			ok = type_matches(value, type->as_string());
		} else if (auto const* array = type->if_array()) {
			ok = false;
			for (auto const& item : *array) {
				if (item.is_string() && type_matches(value, item.as_string())) {
					ok = true;
					break;
				}
			}
		}
		if (!ok)
			out.push_back(path + ": has the wrong type");
	}
	if (value.is_object()) {
		auto const& object = value.as_object();
		if (auto const* required = rules->if_contains("required")) {
			if (auto const* array = required->if_array()) {
				for (auto const& name : *array) {
					if (name.is_string() && !object.contains(name.as_string()))
						out.push_back(path + ": is missing required '" + std::string(name.as_string()) + "'");
				}
			}
		}
		auto const* properties = rules->if_contains("properties");
		auto const* property_rules = properties ? properties->if_object() : nullptr;
		if (property_rules) {
			for (auto const& [key, sub_schema] : *property_rules) {
				auto it = object.find(key);
				if (it != object.end())
					validate_into(it->value(), sub_schema, path + "." + std::string(key), out);
			}
		}
		if (auto const* additional = rules->if_contains("additionalProperties")) {
			if (additional->is_bool() && !additional->as_bool() && property_rules) {
				for (auto const& [key, ignored] : object) {
					(void)ignored;
					if (!property_rules->contains(key))
						out.push_back(path + ": has unexpected property '" + std::string(key) + "'");
				}
			}
		}
	}
	if (auto const* array = value.if_array()) {
		if (auto const* items = rules->if_contains("items")) {
			for (std::size_t i = 0; i < array->size(); ++i)
				validate_into((*array)[i], *items, path + "[" + std::to_string(i) + "]", out);
		}
	}
}

// The mutable capture shared between the child's scoped tool handler and the
// run that awaits the child.
struct structured_capture {
	boost::json::value schema;
	std::optional<boost::json::value> value;
	bool recorded = false;
};

// Releases a scoped registration when the run's frame unwinds.
struct registration_guard {
	araya::registration reg;
	~registration_guard() { reg.release(); }
};

araya::tools::tool_definition structured_tool_def(boost::json::value const& schema) {
	return araya::tools::tool_definition{
		std::string(k_structured_tool),
		"Report your final structured result. Call this exactly once, when your answer is complete; the arguments "
		"must match this tool's parameter schema exactly.",
		schema};
}

araya::system_prompt::prompt_section structured_section() {
	araya::system_prompt::prompt_section section;
	section.name = "tool:structured_output";
	section.order = araya::system_prompt::section_order("STRUCTURED_OUTPUT");
	section.text = std::string(k_structured_instruction);
	return section;
}

araya::tools::tool_handler make_structured_handler(std::shared_ptr<structured_capture> capture) {
	return [capture](araya::tools::tool_context const& ctx) -> araya::task<araya::tools::tool_result> {
		std::vector<std::string> violations;
		validate_into(ctx.arguments, capture->schema, "$", violations);
		if (!violations.empty()) {
			std::string text = "Error: arguments do not match the structured output schema:";
			for (auto const& violation : violations)
				text += "\n- " + violation;
			co_return araya::tools::tool_result{
				boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, true};
		}
		capture->value = ctx.arguments;
		capture->recorded = true;
		co_return araya::tools::tool_result{
			boost::json::array{{{"type", "text"}, {"text", "Structured output recorded."}}}, false};
	};
}

} // namespace

std::shared_ptr<subagent_provider> make_spawn_provider(std::shared_ptr<araya::session::session_store> store) {
	return std::make_shared<spawn_provider>(std::move(store));
}

subagents_service::subagents_service(
	boost::asio::any_io_executor executor,
	std::shared_ptr<araya::session::session_store> store,
	std::shared_ptr<araya::agent::agent_service> agent,
	std::shared_ptr<araya::tools::tools_service> tools,
	std::shared_ptr<araya::system_prompt::system_prompt_service> prompts,
	std::uint32_t max_depth)
	: executor_(std::move(executor))
	, store_(std::move(store))
	, agent_(std::move(agent))
	, tools_(std::move(tools))
	, prompts_(std::move(prompts))
	, max_depth_(max_depth) {}

void subagents_service::set_context_factory(std::function<araya::plugin_context()> factory) {
	context_factory_ = std::move(factory);
}

araya::registration subagents_service::register_provider(
	araya::plugin_context& caller,
	std::string name,
	std::shared_ptr<subagent_provider> provider) {
	if (name.empty())
		throw std::invalid_argument("subagents: provider name must not be empty");
	if (!provider)
		throw std::invalid_argument("subagents: provider '" + name + "' is null");
	if (providers_.contains(name))
		throw std::invalid_argument("subagents: provider '" + name + "' is already registered");
	return caller.effect([this, name, provider = std::move(provider)]() -> araya::cleanup_action {
		providers_[name] = provider;
		return [this, name] { providers_.erase(name); };
	});
}

std::vector<std::string> subagents_service::provider_names() const {
	std::vector<std::string> names;
	names.reserve(providers_.size());
	for (auto const& [name, provider] : providers_)
		names.push_back(name);
	return names;
}

std::shared_ptr<subagent_provider> subagents_service::find_provider(std::string_view name) const {
	auto it = providers_.find(std::string(name));
	return it == providers_.end() ? nullptr : it->second;
}

std::shared_ptr<araya::session::session>
subagents_service::create_child(std::string_view provider_name, start_request const& req, child_state& state) {
	auto provider = find_provider(provider_name);
	if (!provider)
		throw std::invalid_argument("subagents: unknown provider '" + std::string(provider_name) + "'");
	auto caps = provider->caps();

	auto parent = store_->get(araya::session::session_id{req.parent});
	if (!parent)
		throw std::invalid_argument("subagents: unknown parent session '" + req.parent + "'");

	if (!caps.agent_options && (req.provider || req.model || req.reasoning_effort))
		throw std::invalid_argument(
			"subagents: provider '" + std::string(provider_name) + "' does not support route overrides");
	if (!caps.output_schema && req.output_schema)
		throw std::invalid_argument(
			"subagents: provider '" + std::string(provider_name) + "' does not support structured output");

	std::uint32_t parent_depth = parent->header().delegation_depth;
	std::uint32_t child_depth = parent_depth + 1;
	std::uint32_t cap = req.max_depth ? std::min(*req.max_depth, max_depth_) : max_depth_;
	if (child_depth > cap)
		throw std::runtime_error(
			"subagents: delegation depth limit reached (depth " + std::to_string(child_depth) + " > " +
			std::to_string(cap) + ")");

	state.parent = req.parent;
	state.backend = std::string(provider_name);
	state.route_provider = req.provider ? *req.provider : inherit_route(req.parent, "provider").value_or("");
	state.model = req.model ? *req.model : inherit_route(req.parent, "model").value_or("");
	state.reasoning_effort = req.reasoning_effort.value_or("");
	if (state.route_provider.empty() || state.model.empty())
		throw std::runtime_error(
			"subagents: child route requires a provider and model (none given and none inherited)");

	auto child = provider->create_child(req, child_depth);
	state.id = child->id().value;
	return child;
}

std::optional<std::string> subagents_service::inherit_route(std::string const& parent, std::string_view key) const {
	auto s = store_->get(araya::session::session_id{parent});
	if (!s)
		return std::nullopt;
	for (auto it = s->log().rbegin(); it != s->log().rend(); ++it) {
		if (it->type != "request/header")
			continue;
		auto const* obj = it->data.if_object();
		if (!obj)
			continue;
		auto hit = obj->find("header");
		if (hit == obj->end() || !hit->value().is_object())
			continue;
		auto const* found = hit->value().as_object().if_contains(key);
		if (found && found->is_string())
			return std::string(found->as_string());
		return std::nullopt;
	}
	return std::nullopt;
}

araya::task<result> subagents_service::run(std::string_view provider_name, start_request req) {
	co_await boost::asio::dispatch(executor_, boost::asio::use_awaitable);
	child_state state;
	std::shared_ptr<araya::session::session> child;
	try {
		child = create_child(provider_name, req, state);
	} catch (std::exception const& e) {
		co_return error_result(e.what());
	}

	// A schema-bearing one-shot registers a child-scoped capture tool and
	// instruction before the child's first step assembles its prompt.
	std::shared_ptr<structured_capture> capture;
	std::optional<registration_guard> tool_guard;
	std::optional<registration_guard> section_guard;
	if (req.output_schema && tools_ && prompts_ && context_factory_) {
		capture = std::make_shared<structured_capture>();
		capture->schema = *req.output_schema;
		auto view = context_factory_();
		tool_guard.emplace();
		tool_guard->reg = tools_->register_tool(
			view, structured_tool_def(*req.output_schema), make_structured_handler(capture), child->id().value);
		section_guard.emplace();
		section_guard->reg = prompts_->section(view, structured_section(), child->id().value);
	}

	araya::agent::run_options options;
	options.provider = state.route_provider;
	options.model = state.model;
	options.reasoning_effort = state.reasoning_effort;
	options.session = child->id();
	options.input = req.prompt;
	options.stop = req.stop;

	result r;
	r.child = child->id().value;
	try {
		auto outcome = co_await agent_->run(options);
		r.stop_reason = araya::agent::run_status_name(outcome.status);
		r.is_error = outcome.status == araya::agent::run_status::error;
		if (r.is_error && outcome.failure)
			r.error = outcome.failure->message;
		r.output = last_assistant_text(*child);
	} catch (std::exception const& e) {
		r.is_error = true;
		r.stop_reason = "error";
		r.error = e.what();
	}
	if (capture) {
		if (capture->recorded) {
			r.structure = *capture->value;
		} else if (!r.is_error && r.stop_reason == "completed") {
			r.is_error = true;
			r.stop_reason = "error";
			r.error = "structured output was not recorded";
		}
	}
	co_return r;
}

araya::task<result> subagents_service::start_continuable(std::string_view provider_name, start_request req) {
	auto provider = find_provider(provider_name);
	if (!provider)
		co_return error_result("subagents: unknown provider '" + std::string(provider_name) + "'");
	if (!provider->caps().continuable)
		co_return error_result("subagents: provider '" + std::string(provider_name) + "' is not continuable");
	if (req.output_schema)
		co_return error_result("subagents: structured output is only supported for one-shot runs");

	co_await boost::asio::dispatch(executor_, boost::asio::use_awaitable);
	child_state state;
	state.parent_stop = req.stop;
	std::shared_ptr<araya::session::session> child;
	try {
		child = create_child(provider_name, req, state);
	} catch (std::exception const& e) {
		co_return error_result(e.what());
	}

	state.label = req.label;
	auto const id = child->id().value;
	children_[id] = std::move(state);
	auto const& created = children_[id];

	// The child is an ordinary driver session: its first turn is a durable
	// followup that also establishes the route in the request header.
	auto message = araya::llm_bridge::user_message_data("u" + std::to_string(child->log().size()), req.prompt);
	araya::agent::drive_options exec;
	exec.provider = created.route_provider;
	exec.model = created.model;
	exec.reasoning_effort = created.reasoning_effort;
	exec.stop = created.parent_stop;
	try {
		co_await agent_->followup(child->id(), std::move(message), exec);
	} catch (std::exception const& e) {
		children_.erase(id);
		co_return error_result(e.what());
	}

	result r;
	r.stop_reason = "running";
	r.child = id;
	co_return r;
}

araya::task<result> subagents_service::send_message(std::string const& child, std::string text) {
	co_await boost::asio::dispatch(executor_, boost::asio::use_awaitable);
	auto it = children_.find(child);
	if (it == children_.end())
		co_return error_result("subagents: unknown child '" + child + "'");
	auto session = store_->get(araya::session::session_id{child});
	if (!session)
		co_return error_result("subagents: unknown child '" + child + "'");

	auto message = araya::llm_bridge::user_message_data("u" + std::to_string(session->log().size()), std::move(text));
	araya::agent::drive_options exec;
	exec.provider = it->second.route_provider;
	exec.model = it->second.model;
	exec.reasoning_effort = it->second.reasoning_effort;
	exec.stop = it->second.parent_stop;

	bool const running = agent_->status(araya::session::session_id{child}) == araya::agent::agent_status::running;
	try {
		if (running)
			co_await agent_->steer(araya::session::session_id{child}, std::move(message), exec);
		else
			co_await agent_->followup(araya::session::session_id{child}, std::move(message), exec);
	} catch (std::exception const& e) {
		co_return error_result(e.what());
	}
	result r;
	r.stop_reason = running ? "queued" : "running";
	r.child = child;
	co_return r;
}

bool subagents_service::interrupt(std::string const& child) {
	auto it = children_.find(child);
	if (it == children_.end())
		return false;
	if (agent_->status(araya::session::session_id{child}) != araya::agent::agent_status::running)
		return false;
	// Stop the current turn but keep queued input for the next one.
	agent_->cancel(araya::session::session_id{child}, araya::agent::cancel_cause::parent, /*keep_inbox=*/true);
	return true;
}

std::vector<child_info> subagents_service::list_children(std::optional<std::string> const& parent) const {
	std::vector<child_info> out;
	for (auto const& [id, state] : children_) {
		if (parent && state.parent != *parent)
			continue;
		auto const sid = araya::session::session_id{id};
		std::size_t pending = 0;
		if (auto const* inbox = agent_->inbox(sid))
			pending = inbox->next_turn.size() + inbox->next_step.size();
		auto const outcome = agent_->last_outcome(sid);
		std::string stop_reason;
		if (outcome.turn > 0)
			stop_reason = araya::agent::run_status_name(outcome.status);
		out.push_back(child_info{
			state.id,
			state.parent,
			state.label,
			state.backend,
			agent_->status(sid) == araya::agent::agent_status::running,
			pending,
			std::move(stop_reason)});
	}
	return out;
}

void subagents_service::on_agent_idle(araya::session::session_id const& child) {
	if (!children_.contains(child.value))
		return;
	auto self = shared_from_this();
	boost::asio::co_spawn(
		executor_,
		[self, id = child.value]() -> araya::task<void> { co_await self->deliver_settlement(id); },
		boost::asio::detached);
}

araya::task<void> subagents_service::deliver_settlement(std::string child_id) {
	auto it = children_.find(child_id);
	if (it == children_.end())
		co_return;
	auto child = store_->get(araya::session::session_id{child_id});
	if (!child)
		co_return;
	auto const parent_id = araya::session::session_id{it->second.parent};
	auto parent = store_->get(parent_id);
	if (!parent)
		co_return;

	auto const outcome = agent_->last_outcome(araya::session::session_id{child_id});
	std::string const reason = araya::agent::run_status_name(outcome.status);
	std::string const label = it->second.label.empty() ? child_id : it->second.label;
	std::string text = "[subagent " + label + " " + reason + "]";
	if (outcome.status == araya::agent::run_status::error && outcome.failure)
		text += "\n" + outcome.failure->message;
	else if (auto output = last_assistant_text(*child); !output.empty())
		text += "\n" + output;

	auto message = araya::llm_bridge::user_message_data(
		"c" + std::to_string(parent->log().size()),
		text,
		araya::llm_bridge::message_source("plugin", {{"plugin", "subagents"}}));
	if (agent_->status(parent_id) == araya::agent::agent_status::running)
		co_await agent_->inject(parent_id, std::move(message));
	else
		co_await agent_->followup(parent_id, std::move(message));
}

void subagents_service::dispose_children() {
	for (auto const& [id, state] : children_) {
		(void)state;
		store_->dispose(araya::session::session_id{id});
	}
	children_.clear();
}

} // namespace araya::subagents
