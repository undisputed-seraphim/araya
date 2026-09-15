#include "medulla/context.hpp"

#include "medulla/detail/assert.hpp"

#include <vector>

namespace medulla {

context::context(std::shared_ptr<context> parent) : parent_(std::move(parent)) {
}

std::shared_ptr<context> context::root() {
    return std::shared_ptr<context>(new context(nullptr));
}

std::shared_ptr<context> context::make_child() {
    return std::shared_ptr<context>(new context(shared_from_this()));
}

void context::bind(service_id id, binding b) {
    MEDULLA_ASSERT(b.value != nullptr);
    MEDULLA_ASSERT(b.provider != 0);
    // KNOWN CONCURRENCY HAZARD (unfixed by design, noted deliberately):
    // bindings_ is a plain std::map mutated here from a plugin's fiber
    // strand while the control strand reads it via lookup(). This is benign
    // under the single-threaded io_context discipline the engine and the
    // test suite assume, but a real data race for multi-threaded hosts.
    // The planned remedy is routing all context mutation through the
    // control strand; do not paper over this without addressing it there.
    bindings_.insert_or_assign(owned_service_id(id), std::move(b));
}

void context::unbind(service_id id) noexcept {
    bindings_.erase(id);
}

binding const* context::lookup(service_id id) const noexcept {
    for (auto const* scope = this; scope; scope = scope->parent_.get()) {
        auto found = scope->bindings_.find(id);
        if (found != scope->bindings_.end())
            return &found->second;
    }
    return nullptr;
}

binding* context::lookup_mutable(service_id id) noexcept {
    for (auto* scope = this; scope; scope = scope->parent_.get()) {
        auto found = scope->bindings_.find(id);
        if (found != scope->bindings_.end())
            return &found->second;
    }
    return nullptr;
}

void context::set_metadata(service_id id, service_metadata metadata) {
    metadata_.insert_or_assign(owned_service_id(id), std::move(metadata));
}

service_metadata context::metadata_for(service_id id) const {
    std::vector<context const*> chain;
    for (auto const* scope = this; scope; scope = scope->parent_.get())
        chain.push_back(scope);

    service_metadata merged;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        auto found = (*it)->metadata_.find(id);
        if (found != (*it)->metadata_.end()) {
            for (auto const& [k, v] : found->second)
                merged[k] = v;
        }
    }
    return merged;
}

}  // namespace medulla
