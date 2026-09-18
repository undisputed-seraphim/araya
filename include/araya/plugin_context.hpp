#pragma once

#include "araya/activation.hpp"
#include "araya/detail/assert.hpp"
#include "araya/effects.hpp"
#include "araya/events.hpp"
#include "araya/plugin.hpp"
#include "araya/service.hpp"

#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace araya {

class resolution_error : public std::runtime_error {
public:
    explicit resolution_error(service_id id)
        : std::runtime_error("unresolved service '" + std::string(id.name) +
                             "' version " + std::to_string(id.version)),
          service_name(id.name),
          version(id.version) {}

    std::string service_name;
    std::uint32_t version;
};

class plugin_context {
public:
    explicit plugin_context(std::shared_ptr<activation> act)
        : act_(std::move(act)) {}

    std::shared_ptr<activation> const& activation_ptr() const noexcept {
        return act_;
    }

    std::shared_ptr<context> const& scope() const noexcept {
        return act_->scope;
    }

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

    std::stop_token stop_token() const noexcept {
        return act_ ? act_->stop_token() : std::stop_token{};
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
            throw std::logic_error(
                "undeclared capability access: '" +
                std::string(id.name) + "'");
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
        return service_lease<T>(std::static_pointer_cast<T>(b->value),
                                b->provider, merged_metadata(key.id));
    }

    template <class T>
    std::optional<service_lease<T>> find(service_key<T> const& key) {
        auto b = find_binding(key.id);
        if (!b)
            return std::nullopt;
        return service_lease<T>(std::static_pointer_cast<T>(b->value),
                                b->provider, merged_metadata(key.id));
    }

    // Instantiates a child component (Definition 52): the child runs in a
    // context derived from this fiber's, and the instantiation is recorded
    // as an ordinary tracked effect whose inverse retires the child, so
    // unloading this fiber cascades to its children (Theorem 73).
    boost::asio::awaitable<fiber_handle> mount(component_spec spec);

    registration provide_raw(service_id id, std::shared_ptr<void> value) {
        if (act_->spec_declared && !declared(act_->provide_specs, id))
            throw std::logic_error("undeclared provision: '" +
                                   std::string(id.name) + "'");
        auto scope = act_->scope;
        auto state = act_->state == fiber_state::active
                         ? provider_state::active
                         : provider_state::loading;
        scope->bind(id, binding{std::move(value), act_->id, state});
        auto index = act_->effects->add(
            [scope, key = owned_service_id(id), owner = act_->id] {
                auto* b = scope->lookup_mutable(key);
                if (b && b->provider == owner)
                    scope->unbind(service_id{key.name, key.version});
            });
        return registration{act_->effects, index};
    }

    template <class T>
    registration provide(service_key<T> const& key,
                         std::shared_ptr<T> service) {
        return provide_raw(key.id,
                           std::shared_ptr<void>(std::move(service)));
    }

    registration effect(
        std::move_only_function<cleanup_action()> setup) {
        auto cleanup = setup();
        if (!cleanup)
            return {};
        auto index = act_->effects->add(std::move(cleanup));
        return registration{act_->effects, index};
    }

    template <class Message, dispatch_mode Mode, class Fn>
    registration on(event_key<Message, Mode> const& key, Fn&& fn,
                    listener_options opts = {}) {
        if (!act_->bus)
            throw std::logic_error(
                "no event bus bound to this activation");
        auto token = act_->bus->add_listener(key, std::forward<Fn>(fn),
                                             act_->id, opts);
        auto index = act_->effects->add(
            [bus = act_->bus, id = owned_service_id(key.id), token] {
                bus->remove_listener(
                    service_id{id.name, id.version}, token);
            });
        return registration{act_->effects, index};
    }

private:
    static bool declared(std::vector<owned_service_id> const& specs,
                         service_id id) noexcept {
        for (auto const& s : specs) {
            if (s == owned_service_id(id))
                return true;
        }
        return false;
    }

    std::shared_ptr<activation> act_;
};

}  // namespace araya
