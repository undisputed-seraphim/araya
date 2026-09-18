#include "araya/runtime.hpp"

#include "araya/activation.hpp"

#include "araya/context.hpp"
#include "araya/detail/assert.hpp"
#include "araya/detail/fiber.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace araya {

struct runtime::fiber_record {
    fiber_id id;
    std::shared_ptr<detail::fiber_control> control;
    boost::asio::any_io_executor strand;
    component_spec spec;
    std::string path;
    fiber_id parent_fiber = 0;
    std::shared_ptr<araya::activation> parent_activation;
    fiber_state state = fiber_state::inactive;
    std::shared_ptr<araya::activation> activation;
    std::unique_ptr<plugin> instance;
    std::vector<owned_service_id> inject_keys;
    std::vector<std::pair<owned_service_id, bool>> inject;
    std::vector<owned_service_id> provide;
    std::map<owned_service_id, std::uint64_t, transparent_id_less> committed;
    std::set<fiber_id> consumers;
    std::size_t remaining = 0;
    int reactivate = 0;
    bool apply_failed = false;
    std::exception_ptr error;
    std::shared_ptr<detail::gate_impl> done = detail::make_gate(true);
};

struct runtime::resolution {
    bool satisfiable = false;
    std::map<owned_service_id, std::uint64_t, transparent_id_less> providers;
    std::map<owned_service_id, binding, transparent_id_less> bindings;
    std::exception_ptr error;
};

runtime::runtime(boost::asio::any_io_executor ex)
    : strand_(boost::asio::make_strand(std::move(ex))),
      bus_(std::make_shared<event_bus>(strand_)),
      root_context_(context::root()),
      root_activation_(std::make_shared<activation>(root_context_)) {
    root_activation_->bus = bus_;
    root_activation_->owner = this;
}

runtime::~runtime() {
    for (auto& [id, f] : fibers_)
        detail::retire_fiber(f->strand);
}

plugin_context runtime::root_context() {
    return plugin_context{root_activation_};
}

boost::asio::awaitable<fiber_handle> runtime::mount(component_spec spec) {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);
    co_return mount_locked(std::move(spec));
}

fiber_handle runtime::mount_locked(component_spec spec, fiber_id parent) {
    if (!spec.descriptor)
        throw std::invalid_argument("component_spec has no descriptor");
    if (!spec.parent)
        spec.parent = root_context_;

    auto rec = std::make_unique<fiber_record>();
    rec->spec = std::move(spec);
    rec->parent_fiber = parent;
    if (parent != 0) {
        if (auto* p = find(parent))
            rec->parent_activation = p->activation;
    }
    rec->strand = boost::asio::make_strand(strand_);
    rec->control = detail::make_fiber(rec->strand);
    rec->id = rec->control->id;
    for (auto const& dep : rec->spec.descriptor->inject) {
        owned_service_id key{dep.key};
        rec->inject_keys.push_back(key);
        rec->inject.emplace_back(key, dep.required);
    }
    for (auto const& prov : rec->spec.descriptor->provide)
        rec->provide.push_back(owned_service_id{prov.key});

    for (auto const& key : rec->inject_keys)
        consumers_of_[key].insert(rec->id);
    for (auto const& key : rec->provide)
        providers_of_[key].insert(rec->id);

    auto id = rec->id;
    auto* rec_ptr = rec.get();
    fibers_.emplace(id, std::move(rec));
    evaluate(*rec_ptr);

    // Cycle-scan gate: a new cycle signature needs a new node with an
    // out-edge, i.e. one of the new fiber's declared keys must have a
    // provider (itself included — the degenerate self-provision case
    // always scans). Skipping the Tarjan pass here is conservative:
    // removals cannot create cycles, and the reconcile batch runs a
    // full scan at the end regardless.
    bool cycles_possible = false;
    for (auto const& k : rec_ptr->inject_keys)
        if (providers_of_.contains(k)) {
            cycles_possible = true;
            break;
        }
    diagnose(cycles_possible);
    return rec_ptr->control->to_handle();
}

void runtime::retire_child(fiber_id id) {
    boost::asio::post(strand_, [this, id] {
        auto* f = find(id);
        if (!f)
            return;
        f->reactivate = -1;
        begin_unload(*f, false);
        if (f->state == fiber_state::inactive) {
            detail::retire_fiber(f->strand);
            fibers_.erase(id);
            unindex(id);
            return;
        }
        f->done->wait([this, id] {
            if (find(id)) {
                auto* g = find(id);
                detail::retire_fiber(g->strand);
                fibers_.erase(id);
                unindex(id);
            }
        });
    });
}

boost::asio::awaitable<void> runtime::retire(fiber_handle h) {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);
    auto* f = find(h.id());
    if (f && f->state != fiber_state::inactive) {
        f->reactivate = -1;
        auto done = f->done;
        begin_unload(*f, false);
        co_await done->wait(boost::asio::use_awaitable);
    }
    if (find(h.id())) {
        detail::retire_fiber(find(h.id())->strand);
        fibers_.erase(h.id());
        unindex(h.id());
    }
}

boost::asio::awaitable<void> runtime::wait_idle() {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);
    while (!settled())
        co_await idle_->wait(boost::asio::use_awaitable);
}

boost::asio::awaitable<void> runtime::run_on_strand(
    std::move_only_function<void()> fn) {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);
    fn();
}

boost::asio::awaitable<void> runtime::reconcile(
    std::vector<desired_component> desired) {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    std::map<std::string, desired_component const*> desired_by_path;
    for (auto const& d : desired)
        desired_by_path[d.path] = &d;

    std::vector<std::pair<fiber_id, std::shared_ptr<detail::gate_impl>>>
        to_await;

    for (auto it = reconciled_.begin(); it != reconciled_.end();) {
        auto* f = find(it->second);
        if (!f) {
            auto path = it->first;
            it = reconciled_.erase(it);
            entry_ctxs_.erase(path);
            continue;
        }
        auto dit = desired_by_path.find(it->first);
        if (dit == desired_by_path.end()) {
            f->reactivate = -1;
            to_await.emplace_back(f->id, f->done);
            begin_unload(*f, false);
            auto path = it->first;
            it = reconciled_.erase(it);
            entry_ctxs_.erase(path);
            continue;
        }

        auto const* node = dit->second;
        bool same_impl =
            f->spec.descriptor &&
            f->spec.descriptor->name == node->spec.descriptor->name;
        if (!same_impl) {
            f->reactivate = -1;
            to_await.emplace_back(f->id, f->done);
            begin_unload(*f, false);
            auto path = it->first;
            it = reconciled_.erase(it);
            entry_ctxs_.erase(path);
            continue;
        }

        if (f->spec.config != node->spec.config) {
            bool applied = f->instance &&
                           f->state == fiber_state::active &&
                           f->instance->reconfigure(node->spec.config);
            if (!applied) {
                f->reactivate = -1;
                to_await.emplace_back(f->id, f->done);
                begin_unload(*f, false);
                auto path = it->first;
                it = reconciled_.erase(it);
                entry_ctxs_.erase(path);
                continue;
            }
            f->spec.config = node->spec.config;
        }

        // Capture the realms the bindings live under before
        // entry_scope_for refreshes the tags in place.
        std::map<owned_service_id, std::string, transparent_id_less>
            old_realms;
        for (auto const& key : f->provide)
            old_realms[key] = f->spec.parent->realm_for(key);
        auto entry_scope = entry_scope_for(it->first, node->spec);
        bool scope_moved = f->spec.parent != entry_scope;
        bool realms_changed = f->spec.isolate != node->spec.isolate;
        if (scope_moved && f->state == fiber_state::loading) {
            // A scope move mid-transition would strand late provisions in
            // the old scope; rebuild instead.
            f->reactivate = -1;
            to_await.emplace_back(f->id, f->done);
            begin_unload(*f, false);
            auto path = it->first;
            it = reconciled_.erase(it);
            entry_ctxs_.erase(path);
            continue;
        }
        if (scope_moved || realms_changed) {
            if (f->state == fiber_state::active ||
                f->state == fiber_state::loading) {
                reassign(*f, node->spec, entry_scope, old_realms);
            } else {
                f->spec.parent = entry_scope;
                f->spec.isolate = node->spec.isolate;
            }
        }
        ++it;
    }

    for (auto& [id, gate] : to_await)
        co_await gate->wait(boost::asio::use_awaitable);
    for (auto& [id, gate] : to_await) {
        if (fibers_.contains(id)) {
            detail::retire_fiber(fibers_.at(id)->strand);
            fibers_.erase(id);
            unindex(id);
        }
    }

    for (auto const& node : desired) {
        if (reconciled_.contains(node.path))
            continue;
        auto spec = node.spec;
        spec.parent = entry_scope_for(node.path, node.spec);
        auto handle = co_await mount(std::move(spec));
        reconciled_[node.path] = handle.id();
    }
    diagnose();
}

std::shared_ptr<context> runtime::entry_scope_for(
    std::string const& path, component_spec const& spec) {
    auto base = spec.parent ? spec.parent : root_context_;
    if (spec.isolate.empty()) {
        entry_ctxs_.erase(path);
        entry_isolates_.erase(path);
        return base;
    }
    auto& ctx = entry_ctxs_[path];
    if (!ctx || ctx->parent() != base)
        ctx = std::shared_ptr<context>(new context(base, true));
    // Apply the desired tags in place: the fiber's activation shares this
    // context object, so its later provisions land at the updated realms.
    for (auto const& [name, realm] : spec.isolate)
        ctx->isolate(service_id{name, 1}, realm);
    // Drop tags no longer present.
    auto& applied = entry_isolates_[path];
    for (auto const& [name, realm] : applied)
        if (!spec.isolate.contains(name))
            ctx->unisolate(service_id{name, 1});
    applied = spec.isolate;
    return ctx;
}

std::size_t runtime::fiber_count() const noexcept {
    return fibers_.size();
}

fiber_state runtime::state_of(fiber_id id) const noexcept {
    auto it = fibers_.find(id);
    return it != fibers_.end() ? it->second->state : fiber_state::inactive;
}

std::exception_ptr runtime::error_of(fiber_id id) const noexcept {
    auto it = fibers_.find(id);
    return it != fibers_.end() ? it->second->error : nullptr;
}

std::vector<fiber_info> runtime::fibers() const {
    std::vector<fiber_info> out;
    out.reserve(fibers_.size());
    for (auto const& [id, f] : fibers_) {
        fiber_info info;
        info.id = id;
        info.name = f->spec.name;
        if (f->spec.descriptor)
            info.descriptor = std::string(f->spec.descriptor->name);
        info.state = f->state;
        info.error = f->error;
        info.parent = f->parent_fiber;
        info.scope = f->spec.parent;
        info.inject = f->inject_keys;
        info.provide = f->provide;
        for (auto const& [k, p] : f->committed)
            info.committed.emplace(k, p);
        out.push_back(std::move(info));
    }
    return out;
}

boost::asio::awaitable<std::vector<fiber_info>> runtime::fibers_async() const {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);
    co_return fibers();
}

void runtime::on_diagnostic(
    std::move_only_function<void(diagnostic const&)> sink) {
    diagnostic_sink_ = std::move(sink);
}

runtime::fiber_record* runtime::find(fiber_id id) noexcept {
    auto it = fibers_.find(id);
    return it != fibers_.end() ? it->second.get() : nullptr;
}

runtime::resolution runtime::resolve(fiber_record const& f) const {
    resolution r;
    r.satisfiable = true;
    for (auto const& [key, required] : f.inject) {
        auto const* b = f.spec.parent->lookup(key);
        // A fiber's own bindings never satisfy its declarations: the paper's
        // precedence relation is assumed acyclic, and a self-provided key
        // cannot be active at the fiber's own L-Begin anyway. Excluding self
        // also keeps an optional self-provision from oscillating. An
        // unavailable binding (the availability extension) reads as absent.
        if (b && b->value && b->state == provider_state::active &&
            b->available && b->provider != f.id) {
            r.providers[key] = b->provider;
            r.bindings[key] = *b;
        } else if (required) {
            r.satisfiable = false;
            r.error = std::make_exception_ptr(resolution_error{
                service_id{key.name, key.version}});
            return r;
        }
    }
    return r;
}

void runtime::evaluate(fiber_record& f) {
    if (f.state != fiber_state::inactive && f.state != fiber_state::active)
        return;
    if (f.reactivate < 0)
        return;
    auto r = resolve(f);
    if (f.state == fiber_state::inactive) {
        if (r.satisfiable && !f.apply_failed) {
            start_loading(f);
        } else if (!f.error) {
            f.error = r.error;
        }
        return;
    }
    bool same = r.satisfiable && r.providers.size() == f.committed.size();
    if (same) {
        for (auto const& [k, v] : r.providers) {
            auto it = f.committed.find(k);
            if (it == f.committed.end() || it->second != v) {
                same = false;
                break;
            }
        }
    }
    if (same)
        return;
    begin_unload(f, r.satisfiable);
}

void runtime::start_loading(fiber_record& f) {
    ARAYA_ASSERT(f.state == fiber_state::inactive);
    ARAYA_ASSERT(f.committed.empty());
    ARAYA_ASSERT(f.remaining == 0);
    f.error = nullptr;
    f.apply_failed = false;
    f.state = fiber_state::loading;
    f.control->cell->reset_error();
    f.control->cell->state->store(fiber_state::loading);

    auto fresh_stop = std::make_shared<std::stop_source>();
    f.control->cell->stop_source = fresh_stop;
    detail::update_fiber_stop(f.strand, fresh_stop);

    auto act = std::make_shared<activation>(f.spec.parent);
    act->id = f.id;
    act->bus = bus_;
    act->state = fiber_state::loading;
    act->spec_declared = true;
    act->inject_specs = f.inject_keys;
    act->provide_specs = f.provide;
    act->parent = f.parent_activation;
    act->owner = this;
    for (auto const& dep : f.spec.descriptor->inject) {
        owned_service_id key{dep.key};
        if (!dep.metadata.empty())
            act->inject_metadata[key] = dep.metadata;
    }

    auto r = resolve(f);
    if (!r.satisfiable) {
        f.error = r.error;
        f.state = fiber_state::inactive;
        f.control->cell->publish_error(f.error);
        f.control->cell->state->store(fiber_state::inactive);
        detail::retire_fiber(f.strand);
        return;
    }
    act->committed_view = std::move(r.bindings);
    act->stop_source = f.control->cell->stop_source;
    f.committed = std::move(r.providers);
    f.activation = act;

    f.instance = f.spec.descriptor->create(f.spec.config);
    if (!f.instance) {
        f.error = std::make_exception_ptr(std::logic_error(
            "plugin factory returned null for '" +
            std::string(f.spec.descriptor->name) + "'"));
        f.state = fiber_state::inactive;
        f.control->cell->publish_error(f.error);
        f.control->cell->state->store(fiber_state::inactive);
        f.activation.reset();
        f.committed.clear();
        detail::retire_fiber(f.strand);
        return;
    }

    transition_started();
    auto* plugin_ptr = f.instance.get();
    auto id = f.id;
    boost::asio::co_spawn(
        f.strand,
        [plugin_ptr, act]() mutable -> task<void> {
            // Keep the context heap-backed: the enclosing strand handler's
            // stack unwinds while the coroutine is suspended, so captures
            // must not live on it.
            auto ctx = std::make_shared<plugin_context>(act);
            co_await plugin_ptr->apply(*ctx);
        }(),
        boost::asio::bind_cancellation_slot(
            f.control->cell->signal->slot(),
            [this, id](std::exception_ptr ep) {
                on_apply_completed(id, ep);
            }));
}

void runtime::on_apply_completed(fiber_id id, std::exception_ptr ep) {
    auto* f = find(id);
    if (!f || (f->state != fiber_state::loading &&
               f->state != fiber_state::unloading))
        return;
    detail::retire_fiber(f->strand);

    bool stale = !providers_still_active(*f);
    bool cancelled =
        f->control->cell->stop_source->stop_requested();

    if (ep || cancelled || stale || f->state == fiber_state::unloading) {
        if (!f->error) {
            if (ep) {
                f->error = ep;
                // A raise writes the failure outcome on the fiber, and the
                // outcome withholds re-entry: only a revision (reinsertion)
                // retries a failed fiber (Section 4.4, Failure).
                f->apply_failed = true;
            } else if (cancelled) {
                f->error = std::make_exception_ptr(std::runtime_error(
                    "fiber cancelled before activation completed"));
            }
        }
        if (f->activation) {
            f->activation->teardown();
            f->activation->state = fiber_state::inactive;
        }
        f->state = fiber_state::inactive;
        f->control->cell->publish_error(f->error);
        f->control->cell->state->store(fiber_state::inactive);
        f->instance.reset();
        f->committed.clear();
        transition_finished();
        f->done->open();
        if (stale && f->reactivate >= 0)
            evaluate(*f);
        return;
    }

    publish(*f);
}

bool runtime::providers_still_active(fiber_record const& f) const {
    for (auto const& [key, p] : f.committed) {
        auto const* b = f.spec.parent->lookup(key);
        if (!b || !b->value || b->provider != p ||
            b->state != provider_state::active || !b->available)
            return false;
    }
    return true;
}

void runtime::signal_availability(service_id key, context* scope,
                                  std::uint64_t provider, bool available) {
    auto* b = scope->lookup_mutable(key);
    if (!b || !b->value)
        throw std::logic_error("signal_availability: no binding for '" +
                               std::string(key.name) + "'");
    if (b->provider != provider)
        throw std::logic_error(
            "signal_availability: fiber " + std::to_string(provider) +
            " does not provide '" + std::string(key.name) + "'");
    if (!available)
        throw std::logic_error(
            "signal_availability: deactivation is retirement - unload the "
            "provider of '" +
            std::string(key.name) + "' instead");
    if (b->available)
        return;  // idempotent promotion
    b->available = true;
    notify(key, scope, scope->realm_for(key));
}

void runtime::publish(fiber_record& f) {
    ARAYA_ASSERT(f.state == fiber_state::loading);
    ARAYA_ASSERT(f.activation);
    ARAYA_ASSERT(f.activation->state == fiber_state::loading);
    ARAYA_ASSERT(providers_still_active(f));
    for (auto const& key : f.provide) {
        if (auto* b = f.spec.parent->lookup_mutable(key);
            b && b->provider == f.id) {
            ARAYA_ASSERT(b->state == provider_state::loading);
            b->state = provider_state::active;
        }
    }
    f.state = fiber_state::active;
    f.control->cell->state->store(fiber_state::active);
    if (f.activation)
        f.activation->state = fiber_state::active;
    for (auto const& [k, p] : f.committed) {
        auto it = fibers_.find(p);
        if (it != fibers_.end())
            it->second->consumers.insert(f.id);
    }
    transition_finished();
    for (auto const& key : f.provide)
        notify(key, f.spec.parent.get(), f.spec.parent->realm_for(key));
}

void runtime::begin_unload(fiber_record& f, bool reactivate) {
    // reactivate is the retirement flag: -1 (retired) is sticky, mirroring
    // the paper's monotone tau — a host-requested removal must never be
    // undone by a cascade.
    if (f.reactivate >= 0)
        f.reactivate = std::max(f.reactivate, reactivate ? 1 : 0);
    if (f.state == fiber_state::unloading)
        return;
    if (f.state == fiber_state::inactive)
        return;
    if (f.state == fiber_state::loading) {
        f.state = fiber_state::unloading;
        f.control->cell->state->store(fiber_state::unloading);
        f.done->close();
        f.control->cell->stop_source->request_stop();
        return;
    }
    ARAYA_ASSERT(f.state == fiber_state::active);

    f.state = fiber_state::unloading;
    f.control->cell->state->store(fiber_state::unloading);
    if (f.activation)
        f.activation->state = fiber_state::unloading;
    f.done->close();
    transition_started();

    for (auto const& key : f.provide) {
        if (auto* b = f.spec.parent->lookup_mutable(key);
            b && b->provider == f.id)
            b->state = provider_state::retiring;
    }

    auto consumers = f.consumers;
    f.remaining = 0;
    for (auto cid : consumers) {
        auto it = fibers_.find(cid);
        if (it == fibers_.end())
            continue;
        if (it->second->state == fiber_state::inactive)
            continue;
        [[maybe_unused]] bool committed_here = false;
        for (auto const& [k, p] : it->second->committed)
            if (p == f.id) {
                committed_here = true;
                break;
            }
        ARAYA_ASSERT(committed_here);
        ++f.remaining;
    }
    for (auto cid : consumers) {
        auto it = fibers_.find(cid);
        if (it != fibers_.end())
            begin_unload(*it->second, true);
    }

    f.control->cell->stop_source->request_stop();
    maybe_finish_unload(f);
}

void runtime::maybe_finish_unload(fiber_record& f) {
    if (f.state != fiber_state::unloading)
        return;
    if (f.remaining > 0)
        return;
    finish_unload(f);
}

void runtime::finish_unload(fiber_record& f) {
    ARAYA_ASSERT(f.state == fiber_state::unloading);
    ARAYA_ASSERT(f.remaining == 0);
    if (f.activation) {
        ARAYA_ASSERT(f.activation->state == fiber_state::unloading);
        f.activation->teardown();
        f.activation->state = fiber_state::inactive;
    }
    // Detach the committed map before the state flip: maybe_finish_unload
    // below can re-entrantly reactivate this very fiber (notify -> evaluate
    // -> start_loading), which replaces f.committed, so the loop must not
    // iterate the member map.
    auto committed = std::move(f.committed);
    f.state = fiber_state::inactive;
    f.control->cell->publish_error(f.error);
    f.control->cell->state->store(fiber_state::inactive);
    f.instance.reset();

    for (auto const& [k, p] : committed) {
        auto it = fibers_.find(p);
        if (it != fibers_.end()) {
            [[maybe_unused]] auto erased =
                it->second->consumers.erase(f.id);
            ARAYA_ASSERT(erased == 1);
            if (it->second->remaining > 0) {
                ARAYA_ASSERT(it->second->state == fiber_state::unloading);
                --it->second->remaining;
            }
            maybe_finish_unload(*it->second);
        }
    }
    ARAYA_ASSERT(f.consumers.empty());
    transition_finished();
    f.done->open();
    for (auto const& key : f.provide)
        notify(key, f.spec.parent.get(), f.spec.parent->realm_for(key));
    if (f.reactivate > 0) {
        f.reactivate = 0;
        evaluate(f);
    }
}

namespace {
bool scope_on_chain(context const* probe, context const* from) {
    for (auto const* c = from; c; c = c->parent().get())
        if (c == probe)
            return true;
    return false;
}
}  // namespace
void runtime::notify(service_id key, context const* scope,
                     std::string const& realm) {
    auto it = consumers_of_.find(key);
    if (it == consumers_of_.end())
        return;
    std::vector<fiber_id> stale;
    for (auto id : it->second) {
        auto fit = fibers_.find(id);
        if (fit == fibers_.end()) {
            stale.push_back(id);
            continue;
        }
        auto& consumer = *fit->second;
        // Algorithm 3: only fibers whose declared key resolves to the same
        // realm, from a scope that can see the changed binding, are
        // affected.
        if (!scope_on_chain(scope, consumer.spec.parent.get()))
            continue;
        if (consumer.spec.parent->realm_for(key) != realm)
            continue;
        evaluate(consumer);
    }
    for (auto id : stale)
        it->second.erase(id);
}

void runtime::reassign(
    fiber_record& f, component_spec const& next,
    std::shared_ptr<context> const& new_scope,
    std::map<owned_service_id, std::string, transparent_id_less> const&
        old_realms) {
    auto old_scope = f.spec.parent;
    bool moved = false;
    for (auto const& key : f.provide) {
        auto old_it = old_realms.find(key);
        auto old_realm =
            old_it != old_realms.end() ? old_it->second : std::string{};
        auto const* b = old_scope->lookup_realm(key, old_realm);
        if (!b || b->provider != f.id)
            continue;
        auto new_realm = new_scope->realm_for(key);
        if (old_scope == new_scope && old_realm == new_realm)
            continue;
        if (new_scope->lookup_realm(key, new_realm))
            continue;
        new_scope->bind_realm(key, new_realm, *b);
        old_scope->unbind_realm(key, old_realm);
        moved = true;
    }
    if (moved || old_scope != new_scope) {
        for (auto const& key : f.provide) {
            auto old_it = old_realms.find(key);
            auto old_realm = old_it != old_realms.end() ? old_it->second
                                                        : std::string{};
            notify(key, old_scope.get(), old_realm);
            notify(key, new_scope.get(), new_scope->realm_for(key));
        }
    }
    f.spec.parent = new_scope;
    f.spec.isolate = next.isolate;
    f.spec.name = next.name;
    if (f.state == fiber_state::active)
        evaluate(f);
}

void runtime::unindex(fiber_id id) {
    for (auto it = consumers_of_.begin(); it != consumers_of_.end();) {
        it->second.erase(id);
        if (it->second.empty())
            it = consumers_of_.erase(it);
        else
            ++it;
    }
    for (auto it = providers_of_.begin(); it != providers_of_.end();) {
        it->second.erase(id);
        if (it->second.empty())
            it = providers_of_.erase(it);
        else
            ++it;
    }
}

void runtime::diagnose(bool scan_cycles) {
    auto emit = [this](diagnostic_kind kind, std::string key_name,
                       std::uint32_t key_version,
                       std::vector<fiber_id> ids) {
        std::sort(ids.begin(), ids.end());
        std::string sig = kind == diagnostic_kind::cycle ? "c:" : "f:";
        sig += key_name + ":" + std::to_string(key_version) + ":";
        for (auto id : ids)
            sig += std::to_string(id) + ",";
        if (!reported_diagnostics_.insert(std::move(sig)).second)
            return;
        if (!diagnostic_sink_)
            return;
        diagnostic d;
        d.kind = kind;
        d.key_name = std::move(key_name);
        d.key_version = key_version;
        d.fibers = std::move(ids);
        diagnostic_sink_(d);
    };

    // Single-source discipline (Section 4.2.1): two fibers providing one key
    // in one scope and one realm conflict; the bindings share a context map
    // slot, so the last provider shadows the first. Diagnosed, not enforced.
    //
    // The scan gathers (slot, provider) pairs into reused storage and
    // sorts in place: no per-entry map nodes or key strings are built
    // (realm_for's "" fast path stays in SSO). Emission only allocates
    // when a signature is actually reported, which is rare.
    diag_slots_.clear();
    for (auto const& [id, f] : fibers_)
        for (auto const& k : f->provide)
            diag_slots_.emplace_back(
                diag_slot_key{f->spec.parent.get(), k,
                              f->spec.parent->realm_for(k)},
                id);
    std::sort(diag_slots_.begin(), diag_slots_.end(),
              [](auto const& a, auto const& b) {
                  return a.first < b.first;
              });
    for (std::size_t i = 0; i < diag_slots_.size();) {
        std::size_t j = i + 1;
        while (j < diag_slots_.size() &&
               !(diag_slots_[i].first < diag_slots_[j].first) &&
               !(diag_slots_[j].first < diag_slots_[i].first))
            ++j;
        if (j - i >= 2) {
            std::vector<fiber_id> ids;
            ids.reserve(j - i);
            for (std::size_t q = i; q < j; ++q)
                ids.push_back(diag_slots_[q].second);
            // The reported key name carries the realm prefix, exactly as
            // the old string key did.
            auto const& slot = diag_slots_[i].first;
            std::string key_name =
                slot.realm.empty()
                    ? std::string(slot.key.name)
                    : slot.realm + "@" + slot.key.name;
            emit(diagnostic_kind::conflict, std::move(key_name),
                 slot.key.version, std::move(ids));
        }
        i = j;
    }

    // Dependency cycles (Section 6.5): edge m -> n when n provides a key m
    // declares and n's bindings are visible from m's scope. A fiber
    // declaring a key it provides itself is the degenerate n < n.
    // The edge scan joins each declared key against the providers_of_
    // index (O(edges x chain depth)) instead of scanning every fiber.
    // Tarjan runs over dense positions into reused tables; the recursion
    // is a self-referencing lambda (no std::function, no allocation).
    diag_order_.clear();
    for (auto const& [id, f] : fibers_)
        diag_order_.push_back(id);
    auto pos = [&](fiber_id id) {
        return static_cast<int>(
            std::lower_bound(diag_order_.begin(), diag_order_.end(), id) -
            diag_order_.begin());
    };

    if (scan_cycles) {
        if (diag_adj_.size() < diag_order_.size())
            diag_adj_.resize(diag_order_.size());
        for (auto& targets : diag_adj_)
            targets.clear();
        for (auto const& [mid, m] : fibers_) {
            diag_targets_.clear();
            for (auto const& k : m->inject_keys) {
                auto pit = providers_of_.find(k);
                if (pit == providers_of_.end())
                    continue;
                for (auto nid : pit->second)
                    if (scope_on_chain(
                            fibers_.find(nid)->second->spec.parent.get(),
                            m->spec.parent.get()))
                        diag_targets_.push_back(nid);
            }
            std::sort(diag_targets_.begin(), diag_targets_.end());
            diag_targets_.erase(
                std::unique(diag_targets_.begin(), diag_targets_.end()),
                diag_targets_.end());
            diag_adj_[pos(mid)] = diag_targets_;
        }
    }

    diag_index_.assign(diag_order_.size(), -1);
    diag_low_.assign(diag_order_.size(), 0);
    diag_on_stack_.assign(diag_order_.size(), 0);
    diag_stack_.clear();
    int next = 0;

    auto strongconnect = [&](auto& self, int p) -> void {
        diag_index_[p] = next;
        diag_low_[p] = next;
        ++next;
        diag_stack_.push_back(p);
        diag_on_stack_[p] = 1;
        for (auto w : diag_adj_[p]) {
            int wp = pos(w);
            if (diag_index_[wp] == -1) {
                self(self, wp);
                diag_low_[p] = std::min(diag_low_[p], diag_low_[wp]);
            } else if (diag_on_stack_[wp]) {
                diag_low_[p] = std::min(diag_low_[p], diag_index_[wp]);
            }
        }
        if (diag_low_[p] != diag_index_[p])
            return;
        // Pop the SCC into the reused buffer and emit inline: emission
        // order is SCC discovery order, exactly as the collected form.
        // emit() copies the buffer (and only when a real cycle is found),
        // so the rescan path allocates nothing.
        diag_scc_.clear();
        while (true) {
            int q = diag_stack_.back();
            diag_stack_.pop_back();
            diag_on_stack_[q] = 0;
            diag_scc_.push_back(diag_order_[q]);
            if (q == p)
                break;
        }
        if (diag_scc_.size() > 1)
            emit(diagnostic_kind::cycle, "", 0, diag_scc_);
    };

    if (scan_cycles)
        for (std::size_t i = 0; i < diag_order_.size(); ++i)
            if (diag_index_[i] == -1)
                strongconnect(strongconnect, static_cast<int>(i));

    // Self-provision: a component declaring a key it provides itself.
    for (auto const& [id, f] : fibers_) {
        for (auto const& k : f->provide)
            if (std::find(f->inject_keys.begin(), f->inject_keys.end(), k) !=
                f->inject_keys.end()) {
                emit(diagnostic_kind::cycle, k.name, k.version, {id});
                break;
            }
    }
}

void runtime::transition_started() {
    idle_->close();
    ++in_flight_;
}

void runtime::transition_finished() {
    ARAYA_ASSERT(in_flight_ > 0);
    if (in_flight_ > 0)
        --in_flight_;
    if (in_flight_ == 0)
        idle_->open();
}

void runtime::validate_invariants() const {
    for (auto const& [id, f] : fibers_) {
        ARAYA_ASSERT(f->id == id);
        ARAYA_ASSERT(f->control);

        // Declaration immutability (Lemma 59(5)): inject/provide keys are
        // written once at mount and never revised.
        if (f->spec.descriptor) {
            std::vector<owned_service_id> inject_keys;
            inject_keys.reserve(f->spec.descriptor->inject.size());
            for (auto const& dep : f->spec.descriptor->inject)
                inject_keys.push_back(owned_service_id{dep.key});
            ARAYA_ASSERT(f->inject_keys == inject_keys);

            std::vector<owned_service_id> provide_keys;
            provide_keys.reserve(f->spec.descriptor->provide.size());
            for (auto const& prov : f->spec.descriptor->provide)
                provide_keys.push_back(owned_service_id{prov.key});
            ARAYA_ASSERT(f->provide == provide_keys);
        }

        if (f->state == fiber_state::inactive) {
            ARAYA_ASSERT(f->committed.empty());
            ARAYA_ASSERT(f->remaining == 0);
            ARAYA_ASSERT(f->activation == nullptr ||
                           (f->activation->state == fiber_state::inactive &&
                            f->activation->effects->size() == 0));
            continue;
        }

        // Committed-view hygiene (Definition 63(3)/(4)): a committed view
        // names only declared keys and installed providers.
        for (auto const& [k, p] : f->committed) {
            ARAYA_ASSERT(std::find(f->inject_keys.begin(),
                                     f->inject_keys.end(),
                                     k) != f->inject_keys.end());
            [[maybe_unused]] auto pit = fibers_.find(p);
            ARAYA_ASSERT(pit != fibers_.end());
            ARAYA_ASSERT(pit->second->state != fiber_state::inactive);
        }
        if (f->activation) {
            ARAYA_ASSERT(f->activation->committed_view.size() ==
                           f->committed.size());
            for (auto const& [k, b] : f->activation->committed_view) {
                ARAYA_ASSERT(b.value != nullptr);
                ARAYA_ASSERT(b.provider != 0);
                [[maybe_unused]] auto cit = f->committed.find(k);
                ARAYA_ASSERT(cit != f->committed.end());
                ARAYA_ASSERT(cit->second == b.provider);
            }
        }

        // Guard accounting (Definition 54): every consumer edge is a real
        // commitment, and `remaining` counts exactly the installed ones.
        [[maybe_unused]] std::size_t expected = 0;
        for (auto cid : f->consumers) {
            auto cit = fibers_.find(cid);
            ARAYA_ASSERT(cit != fibers_.end());
            [[maybe_unused]] bool committed_here = false;
            for (auto const& [k, p] : cit->second->committed)
                if (p == id) {
                    committed_here = true;
                    break;
                }
            ARAYA_ASSERT(committed_here);
            if (cit->second->state != fiber_state::inactive)
                ++expected;
        }
        if (f->state == fiber_state::unloading)
            ARAYA_ASSERT(f->remaining == expected);
        else
            ARAYA_ASSERT(f->remaining == 0);
    }

    // consumers_of_ index consistency: no stale ids, every declared key
    // registered. providers_of_ mirrors the same discipline for provide
    // keys.
    for (auto const& [key, ids] : consumers_of_)
        for ([[maybe_unused]] auto cid : ids)
            ARAYA_ASSERT(fibers_.find(cid) != fibers_.end());
    for (auto const& [id, f] : fibers_) {
        for (auto const& k : f->inject_keys) {
            [[maybe_unused]] auto it = consumers_of_.find(k);
            ARAYA_ASSERT(it != consumers_of_.end());
            ARAYA_ASSERT(it->second.contains(id));
        }
    }
    for (auto const& [key, ids] : providers_of_)
        for ([[maybe_unused]] auto pid : ids)
            ARAYA_ASSERT(fibers_.find(pid) != fibers_.end());
    for (auto const& [id, f] : fibers_) {
        for (auto const& k : f->provide) {
            [[maybe_unused]] auto it = providers_of_.find(k);
            ARAYA_ASSERT(it != providers_of_.end());
            ARAYA_ASSERT(it->second.contains(id));
        }
    }
}

boost::asio::awaitable<void> runtime::validate_invariants_async() const {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);
    validate_invariants();
}

}  // namespace araya
