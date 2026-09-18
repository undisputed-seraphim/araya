#include "araya/context.hpp"

#include "araya/detail/assert.hpp"

#include <utility>
#include <vector>

namespace araya {

context::binding_key context::key(service_id id, std::string const& realm) {
	return binding_key{std::string(id.name), id.version, realm};
}

context::context(std::shared_ptr<context> parent, bool pass_through_bind)
	: parent_(std::move(parent))
	, pass_through_bind_(pass_through_bind) {}

std::shared_ptr<context> context::root() { return std::shared_ptr<context>(new context(nullptr)); }

std::shared_ptr<context> context::make_child() { return std::shared_ptr<context>(new context(shared_from_this())); }

void context::isolate(service_id id, std::string realm) {
	realms_.insert_or_assign(owned_service_id(id), std::move(realm));
}

void context::unisolate(service_id id) { realms_.erase(id); }

std::string context::realm_for(service_id id) const {
	for (auto const* c = this; c; c = c->parent_.get()) {
		auto found = c->realms_.find(id);
		if (found != c->realms_.end())
			return found->second;
	}
	return "";
}

void context::bind_realm(service_id id, std::string realm, binding b) {
	ARAYA_ASSERT(b.value != nullptr);
	ARAYA_ASSERT(b.provider != 0);
	// KNOWN CONCURRENCY HAZARD (unfixed by design, noted deliberately):
	// bindings_ is a plain std::map mutated here from a plugin's fiber
	// strand while the control strand reads it via lookup(). This is benign
	// under the single-threaded io_context discipline the engine and the
	// test suite assume, but a real data race for multi-threaded hosts.
	// The planned remedy is routing all context mutation through the
	// control strand; do not paper over this without addressing it there.
	if (pass_through_bind_) {
		parent_->bind_realm(id, std::move(realm), std::move(b));
		return;
	}
	bindings_.insert_or_assign(context::key(id, realm), std::move(b));
}

void context::bind(service_id id, binding b) { bind_realm(id, realm_for(id), std::move(b)); }

void context::unbind_realm(service_id id, std::string const& realm) noexcept {
	if (pass_through_bind_) {
		if (parent_)
			parent_->unbind_realm(id, realm);
		return;
	}
	bindings_.erase(context::key(id, realm));
}

void context::unbind(service_id id) noexcept { unbind_realm(id, realm_for(id)); }

binding const* context::lookup_realm(service_id id, std::string const& realm) const noexcept {
	for (auto const* scope = this; scope; scope = scope->parent_.get()) {
		auto found = scope->bindings_.find(context::key(id, realm));
		if (found != scope->bindings_.end())
			return &found->second;
	}
	return nullptr;
}

binding const* context::lookup(service_id id) const noexcept { return lookup_realm(id, realm_for(id)); }

binding* context::lookup_mutable(service_id id) noexcept {
	auto key = context::key(id, realm_for(id));
	for (auto* scope = this; scope; scope = scope->parent_.get()) {
		auto found = scope->bindings_.find(key);
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

} // namespace araya
