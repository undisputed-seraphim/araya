#include "araya/subagents/subagents.hpp"

#include "araya/llm/bridge.hpp"

#include <boost/asio/co_spawn.hpp>

#include <algorithm>
#include <exception>
#include <functional>
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

} // namespace

std::shared_ptr<subagent_provider> make_spawn_provider(std::shared_ptr<araya::session::session_store> store) {
	return std::make_shared<spawn_provider>(std::move(store));
}

subagents_service::subagents_service(
	boost::asio::any_io_executor executor,
	std::shared_ptr<araya::session::session_store> store,
	std::shared_ptr<araya::agent::agent_service> agent,
	std::uint32_t max_depth)
	: executor_(std::move(executor))
	, store_(std::move(store))
	, agent_(std::move(agent))
	, max_depth_(max_depth) {}

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
	child_state state;
	std::shared_ptr<araya::session::session> child;
	try {
		child = create_child(provider_name, req, state);
	} catch (std::exception const& e) {
		co_return error_result(e.what());
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
	co_return r;
}

araya::task<result> subagents_service::start_continuable(std::string_view provider_name, start_request req) {
	auto provider = find_provider(provider_name);
	if (!provider)
		co_return error_result("subagents: unknown provider '" + std::string(provider_name) + "'");
	if (!provider->caps().continuable)
		co_return error_result("subagents: provider '" + std::string(provider_name) + "' is not continuable");

	child_state state;
	state.parent_stop = req.stop;
	std::shared_ptr<araya::session::session> child;
	try {
		child = create_child(provider_name, req, state);
	} catch (std::exception const& e) {
		co_return error_result(e.what());
	}

	state.label = req.label;
	state.pending.push_back(req.prompt);
	auto id = child->id().value;
	children_[id] = std::move(state);
	kick(id);

	result r;
	r.stop_reason = "running";
	r.child = std::move(id);
	co_return r;
}

void subagents_service::kick(std::string const& child_id) {
	auto it = children_.find(child_id);
	if (it == children_.end())
		return;
	auto& state = it->second;
	if (state.running || state.pending.empty())
		return;

	state.running = true;
	auto input = std::move(state.pending.front());
	state.pending.pop_front();

	auto stop = std::make_shared<std::stop_source>();
	state.current_stop = stop;
	state.parent_bridge.reset();
	if (state.parent_stop.stop_possible()) {
		std::stop_source* raw = stop.get();
		state.parent_bridge = std::make_shared<std::stop_callback<std::function<void()>>>(
			state.parent_stop, [raw] { raw->request_stop(); });
	}

	auto self = shared_from_this();
	boost::asio::co_spawn(
		executor_,
		[self, child_id, input = std::move(input), stop]() mutable -> araya::task<void> {
			co_await self->run_turn(child_id, std::move(input), stop);
		},
		[weak = std::weak_ptr<subagents_service>(self), child_id](std::exception_ptr ep) {
			if (auto svc = weak.lock())
				svc->on_turn_done(child_id, ep);
		});
}

araya::task<void>
subagents_service::run_turn(std::string child_id, std::string input, std::shared_ptr<std::stop_source> stop) {
	auto child = store_->get(araya::session::session_id{child_id});
	auto it = children_.find(child_id);
	if (!child || it == children_.end())
		co_return;

	// Copy the route out before awaiting: disposal (plugin unload) may
	// clear the child map while the turn is in flight, invalidating the
	// iterator. Re-look-up on resume and drop the result if gone.
	araya::agent::run_options options;
	options.provider = it->second.route_provider;
	options.model = it->second.model;
	options.reasoning_effort = it->second.reasoning_effort;
	options.session = child->id();
	options.input = std::move(input);
	options.stop = stop->get_token();

	std::string stop_reason;
	std::string error;
	std::string output;
	try {
		auto outcome = co_await agent_->run(options);
		stop_reason = araya::agent::run_status_name(outcome.status);
		if (outcome.status == araya::agent::run_status::error && outcome.failure)
			error = outcome.failure->message;
		output = last_assistant_text(*child);
	} catch (std::exception const& e) {
		stop_reason = "error";
		error = e.what();
	}

	auto after = children_.find(child_id);
	if (after == children_.end())
		co_return;
	after->second.stop_reason = std::move(stop_reason);
	after->second.error = std::move(error);
	after->second.last_output = std::move(output);
	co_return;
}

void subagents_service::on_turn_done(std::string const& child_id, std::exception_ptr ep) {
	auto it = children_.find(child_id);
	if (it == children_.end())
		return;
	auto& state = it->second;
	state.running = false;
	state.current_stop.reset();
	state.parent_bridge.reset();

	if (ep && state.error.empty()) {
		try {
			std::rethrow_exception(ep);
		} catch (std::exception const& e) {
			state.stop_reason = "error";
			state.error = e.what();
		} catch (...) {
			state.stop_reason = "error";
			state.error = "unknown subagent failure";
		}
	}

	// Deliver the settlement to the parent as a user message. The parent
	// sees it at its next step (the request is rebuilt from the surface
	// each step) or, if it already finished, at its next turn.
	if (auto parent = store_->get(araya::session::session_id{state.parent})) {
		std::string text = "[subagent " + (state.label.empty() ? state.id : state.label) + " " +
						   (state.stop_reason.empty() ? "completed" : state.stop_reason) + "]";
		if (!state.error.empty())
			text += "\n" + state.error;
		else if (!state.last_output.empty())
			text += "\n" + state.last_output;
		parent->append(
			"user/message", araya::llm_bridge::user_message_data("c" + std::to_string(parent->log().size()), text));
	}

	// Continue with the next queued message at the turn boundary.
	kick(child_id);
}

araya::task<result> subagents_service::send_message(std::string const& child, std::string text) {
	auto it = children_.find(child);
	if (it == children_.end())
		co_return error_result("subagents: unknown child '" + child + "'");
	bool was_running = it->second.running;
	it->second.pending.push_back(std::move(text));
	kick(child);
	result r;
	r.stop_reason = was_running ? "queued" : "running";
	r.child = child;
	co_return r;
}

bool subagents_service::interrupt(std::string const& child) {
	auto it = children_.find(child);
	if (it == children_.end() || !it->second.running || !it->second.current_stop)
		return false;
	it->second.current_stop->request_stop();
	return true;
}

std::vector<child_info> subagents_service::list_children(std::optional<std::string> const& parent) const {
	std::vector<child_info> out;
	for (auto const& [id, state] : children_) {
		if (parent && state.parent != *parent)
			continue;
		out.push_back(child_info{
			state.id,
			state.parent,
			state.label,
			state.backend,
			state.running,
			state.pending.size(),
			state.stop_reason});
	}
	return out;
}

void subagents_service::dispose_children() {
	for (auto const& [id, state] : children_) {
		if (state.current_stop)
			state.current_stop->request_stop();
		store_->dispose(araya::session::session_id{id});
	}
	children_.clear();
}

} // namespace araya::subagents
