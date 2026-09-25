#pragma once

#include "araya/activation.hpp"
#include "araya/detail/assert.hpp"
#include "araya/effects.hpp"
#include "araya/events.hpp"
#include "araya/plugin.hpp"
#include "araya/service.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace araya {

// Thrown by require() when a declared key has no binding (or its binding
// is unavailable - an unavailable provision reads as absent). find()
// returns nullopt instead of throwing; undeclared access is a
// std::logic_error, not a resolution_error.
class resolution_error : public std::runtime_error {
public:
	explicit resolution_error(service_id id)
		: std::runtime_error("unresolved service '" + std::string(id.name) + "' version " + std::to_string(id.version))
		, service_name(id.name)
		, version(id.version) {}

	std::string service_name;
	std::uint32_t version;
};

// What provide() returns: the early-release registration plus a live
// copy of the published value. Deriving from registration keeps every
// release() call site compiling, and carrying the value makes the
// publish-then-capture idiom safe by construction: the author captures
// from the handle, never from a local that provide() consumed (the
// classic move-then-capture null-shared_ptr trap). Capture p.value, not
// the temporary passed to provide.
template <class T>
class provision_handle : public registration {
public:
	provision_handle() = default;

	provision_handle(registration release, std::shared_ptr<T> value)
		: registration(std::move(release))
		, value(std::move(value)) {}

	std::shared_ptr<T> value;
};

// The plugin-facing half of one activation.
//
// LIFETIME: this is a *view*, not a handle. It is valid only while the
// activation runs - inside plugin::apply, and inside the listeners and
// cleanups that apply registered. Never store it: by the time a stored
// copy could be used, the activation it describes may be unloading.
//
// THREADING: everything here runs on the control strand already; apply
// bodies and listeners execute on it. set_available is synchronous and
// must be called from that strand - offload long work to your own
// executors and return to the strand before touching the context.
class plugin_context {
public:
	explicit plugin_context(std::shared_ptr<activation> act)
		: act_(std::move(act)) {}

	std::shared_ptr<activation> const& activation_ptr() const noexcept { return act_; }

	std::shared_ptr<context> const& scope() const noexcept { return act_->scope; }

	// The runtime's root context: the top of this activation's scope
	// chain. Every scope in a runtime descends from the root, so the walk
	// is the definition, not a lookup. Returns null for a default
	// plugin_context.
	std::shared_ptr<context> root() const noexcept {
		auto s = act_ ? act_->scope : nullptr;
		if (!s)
			return nullptr;
		while (s->parent())
			s = s->parent();
		return s;
	}

	std::stop_token stop_token() const noexcept { return act_ ? act_->stop_token() : std::stop_token{}; }

	// The runtime's control-strand executor. Background work that touches
	// any service must run here: co_spawn onto this executor to keep a
	// task on the control strand. An empty executor for a default context.
	boost::asio::any_io_executor executor() const noexcept {
		return act_ && act_->bus ? act_->bus->executor() : boost::asio::any_io_executor{};
	}

	// The interception metadata merged at access (Definition 27): the
	// component-declared metadata overlaid with the context-carried
	// metadata, which takes priority.
	service_metadata merged_metadata(service_id id) const {
		service_metadata merged;
		auto declared = act_->inject_metadata.find(id);
		if (declared != act_->inject_metadata.end())
			merged = declared->second;
		for (auto const& [k, v] : act_->scope->metadata_for(id))
			merged[k] = v;
		return merged;
	}

	std::optional<binding> find_binding(service_id id) const {
		if (act_->spec_declared) {
			if (declared(act_->inject_specs, id)) {
				auto found = act_->committed_view.find(id);
				if (found == act_->committed_view.end())
					return std::nullopt;
				ARAYA_ASSERT(found->second.value != nullptr);
				ARAYA_ASSERT(found->second.provider != 0);
				return found->second;
			}
			// Algorithm 6: walk the parent-fiber chain, resolving undeclared
			// keys against the first parent's committed view that binds them.
			for (auto* p = act_->parent.get(); p; p = p->parent.get()) {
				auto found = p->committed_view.find(id);
				if (found != p->committed_view.end()) {
					ARAYA_ASSERT(found->second.value != nullptr);
					ARAYA_ASSERT(found->second.provider != 0);
					return found->second;
				}
				if (declared(p->inject_specs, id))
					return std::nullopt;
			}
			throw std::logic_error("undeclared capability access: '" + std::string(id.name) + "'");
		}
		auto const* b = act_->scope->lookup(id);
		if (!b || !b->value || b->state != provider_state::active)
			return std::nullopt;
		return *b;
	}

	template <class T>
	service_lease<T> require(service_key<T> const& key) {
		auto b = find_binding(key.id);
		if (!b)
			throw resolution_error(key.id);
		return service_lease<T>(std::static_pointer_cast<T>(b->value), b->provider, merged_metadata(key.id));
	}

	// Like require, but absence (or an unavailable binding) is nullopt
	// rather than an exception. A present-but-unavailable binding is
	// exactly absence: optional consumers see the empty view and are
	// re-applied when the provision is promoted.
	template <class T>
	std::optional<service_lease<T>> find(service_key<T> const& key) {
		auto b = find_binding(key.id);
		if (!b)
			return std::nullopt;
		return service_lease<T>(std::static_pointer_cast<T>(b->value), b->provider, merged_metadata(key.id));
	}

	// Instantiates a child component (Definition 52): the child runs in a
	// context derived from this fiber's, and the instantiation is recorded
	// as an ordinary tracked effect whose inverse retires the child, so
	// unloading this fiber cascades to its children (Theorem 73).
	boost::asio::awaitable<fiber_handle> mount(component_spec spec);

	// Publishes a service under a declared provision key. Throws
	// std::logic_error for undeclared keys. The binding is removed
	// automatically at teardown (reverse order); the returned
	// registration releases it early.
	//
	// Availability: check (if given) runs exactly once, here, before the
	// binding is published. A false or throwing check publishes the
	// binding as unavailable: required consumers park, optional consumers
	// proceed with the empty view, and the provider promotes later via
	// set_available. There is no demotion - see set_available.
	registration provide_raw(service_id id, std::shared_ptr<void> value, std::function<bool()> check = {}) {
		if (act_->spec_declared && !declared(act_->provide_specs, id))
			throw std::logic_error("undeclared provision: '" + std::string(id.name) + "'");
		auto scope = act_->scope;
		auto state = act_->state == fiber_state::active ? provider_state::active : provider_state::loading;
		// The availability check is evaluated once at provide-time; the
		// outcome is promotion-only (see set_available below). A
		// throwing check reads as unavailable.
		bool available = true;
		if (check) {
			try {
				available = check();
			} catch (...) {
				available = false;
			}
		}
		scope->bind(id, binding{std::move(value), act_->id, state, available});
		auto index = act_->effects->add([scope, key = owned_service_id(id), owner = act_->id] {
			auto* b = scope->lookup_mutable(key);
			if (b && b->provider == owner)
				scope->unbind(service_id{key.name, key.version});
		});
		return registration{act_->effects, index};
	}

	// The typed provision. Returns a provision_handle carrying a live
	// copy of the published value: capture that (handle.value) in the
	// listeners you register afterwards -
	//   auto p = ctx.provide(key, make_shared<...>());
	//   ctx.on(event, [svc = p.value](...) { ... });
	// The handle's release() still unbinds early, and dropping it keeps
	// the ordinary teardown path.
	template <class T>
	provision_handle<T>
	provide(service_key<T> const& key, std::shared_ptr<T> service, std::function<bool()> check = {}) {
		auto value = service;
		auto release = provide_raw(key.id, std::shared_ptr<void>(std::move(service)), std::move(check));
		return provision_handle<T>{std::move(release), std::move(value)};
	}

	// Promotes this fiber's provision of key to available and re-evaluates
	// the dependents (ArayaMachine.tla's AvailabilityFlip, read as
	// L-Finish through the lagged alpha). Promotion is idempotent.
	// Deactivation is NOT expressible in the paper's lifecycle DAG:
	// marking a provision unavailable throws - unload the provider
	// instead. Must be called on the control strand (from apply or a
	// listener).
	template <class T>
	void set_available(service_key<T> const& key, bool available) const {
		set_available_raw(key.id, available);
	}

	// The runtime-touching half of set_available (defined in
	// plugin_context.cpp, where the runtime is complete). Const on the
	// context: the mutation goes through the activation's runtime.
	void set_available_raw(service_id key, bool available) const;

	// Runs setup immediately (on the strand) and records its returned
	// cleanup for reverse-order teardown, unless setup returns an empty
	// cleanup (in which case the effect is a no-op and the returned
	// registration is inert). Throw from setup to fail the activation -
	// nothing is recorded.
	registration effect(std::move_only_function<cleanup_action()> setup) {
		auto cleanup = setup();
		if (!cleanup)
			return {};
		auto index = act_->effects->add(std::move(cleanup));
		return registration{act_->effects, index};
	}

	// Registers a listener owned by this activation: removed at teardown
	// (or earlier via the returned registration). Throws if no event bus
	// is bound to this activation. The listener's signature is fixed by
	// the key's dispatch mode; see events.hpp for the per-mode contract
	// and listener_options (prepend/once/global/scope).
	template <class Message, dispatch_mode Mode, class Fn>
	registration on(event_key<Message, Mode> const& key, Fn&& fn, listener_options opts = {}) {
		if (!act_->bus)
			throw std::logic_error("no event bus bound to this activation");
		auto token = act_->bus->add_listener(key, std::forward<Fn>(fn), act_->id, opts);
		auto index = act_->effects->add([bus = act_->bus, id = owned_service_id(key.id), token] {
			bus->remove_listener(service_id{id.name, id.version}, token);
		});
		return registration{act_->effects, index};
	}

private:
	static bool declared(std::vector<owned_service_id> const& specs, service_id id) noexcept {
		for (auto const& s : specs) {
			if (s == owned_service_id(id))
				return true;
		}
		return false;
	}

	std::shared_ptr<activation> act_;
};

} // namespace araya
