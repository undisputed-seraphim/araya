#include "araya/plugin_context.hpp"

#include "araya/runtime.hpp"

#include <stdexcept>

namespace araya {

void plugin_context::set_available_raw(service_id key, bool available) const {
	auto owner = act_ ? act_->owner.lock() : nullptr;
	if (!owner)
		throw std::logic_error("plugin_context is not attached to a runtime");
	owner->signal_availability(key, *act_->scope, act_->id, available);
}

boost::asio::awaitable<fiber_handle>
plugin_context::mount(component_spec spec) { // Copy everything this coroutine needs into its own frame before the
	// first suspension: the calling plugin_context may live on the
	// initiating stack, which unwinds while this coroutine is suspended.
	auto act = act_;
	auto owner = act ? act->owner.lock() : nullptr;
	if (!owner)
		throw std::logic_error("plugin_context is not attached to a runtime");
	if (act->spec_declared) {
		// Definition 52 / Algorithm 4: the child's context derives from this
		// fiber's context (ctx[fiber ↦ fiber]).
		spec.parent = act->scope->make_child();
	}
	fiber_handle h;
	co_await owner->run_on_strand(
		[&, act, spec = std::move(spec)]() mutable { h = owner->mount_locked(std::move(spec), act->id); });
	if (act->spec_declared) {
		// The instantiation is an ordinary tracked effect; its inverse is
		// the O-Retire of the child, so unloading the parent cascades.
		// The weak owner keeps the effect graph free of runtime <->
		// activation cycles: if the runtime is already gone, its
		// destructor has retired the child anyway.
		act->effects->add([weak = act->owner, id = h.id()] {
			if (auto owner = weak.lock())
				owner->retire_child(id);
		});
	}
	co_return h;
}

} // namespace araya
