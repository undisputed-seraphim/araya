#include "medulla/plugin_context.hpp"

#include "medulla/runtime.hpp"

#include <stdexcept>

namespace medulla {

boost::asio::awaitable<fiber_handle> plugin_context::mount(component_spec spec) {
    // Copy everything this coroutine needs into its own frame before the
    // first suspension: the calling plugin_context may live on the
    // initiating stack, which unwinds while this coroutine is suspended.
    auto act = act_;
    auto* owner = act ? act->owner : nullptr;
    if (!owner)
        throw std::logic_error("plugin_context is not attached to a runtime");
    if (act->spec_declared) {
        // Definition 52 / Algorithm 4: the child's context derives from this
        // fiber's context (ctx[fiber ↦ fiber]).
        spec.parent = act->scope->make_child();
    }
    fiber_handle h;
    co_await owner->run_on_strand(
        [&, act, spec = std::move(spec)]() mutable {
            h = owner->mount_locked(std::move(spec), act->id);
        });
    if (act->spec_declared) {
        // The instantiation is an ordinary tracked effect; its inverse is
        // the O-Retire of the child, so unloading the parent cascades.
        act->effects->add([owner, id = h.id()] {
            owner->retire_child(id);
        });
    }
    co_return h;
}

}  // namespace medulla
