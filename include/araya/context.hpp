#pragma once

#include "araya/service.hpp"

#include <map>
#include <memory>
#include <string>

namespace araya {

// A scoped binding tree: the spatial composition structure. Each fiber
// gets a scope (child of its parent's), and bindings, realm tags, and
// metadata are stored per scope with parent-chain fallback.
//
// LIFETIME: contexts are shared_ptr-managed and owned by the runtime and
// the activations' scopes; plugin code reads them through
// plugin_context::scope()/root() and never binds into them directly
// (provide does it on the fiber's behalf). Advanced hosts use
// context::isolate to carve realm-tagged scopes (Section 5.2.1) before
// mounting components into them.
//
// THREADING: engine-mutated on the control strand; treat as read-only
// outside it (lookup/lookup_realm/metadata_for are safe to call from
// listeners and cleanups, which run on the strand anyway).
class context : public std::enable_shared_from_this<context> {
public:
	explicit context(std::shared_ptr<context> parent = nullptr, bool pass_through_bind = false);

	static std::shared_ptr<context> root();

	std::shared_ptr<context> parent() const noexcept { return parent_; }

	std::shared_ptr<context> make_child();

	void bind(service_id id, binding b);

	void unbind(service_id id) noexcept;

	binding const* lookup(service_id id) const noexcept;

	binding* lookup_mutable(service_id id) noexcept;

	// Realm machinery (Section 3.2.3 / Section 5.2.1): a per-key realm tag
	// redirects resolution of the key, inherited by descendant contexts.
	// Bindings are stored under the realm resolved at bind time; the default
	// (untagged) realm is the empty string, preserving the plain per-key
	// binding model. Keys sharing a realm share one binding.
	void isolate(service_id id, std::string realm);

	void unisolate(service_id id);

	std::string realm_for(service_id id) const;

	void bind_realm(service_id id, std::string realm, binding b);

	void unbind_realm(service_id id, std::string const& realm) noexcept;

	binding const* lookup_realm(service_id id, std::string const& realm) const noexcept;

	void set_metadata(service_id id, service_metadata metadata);

	service_metadata metadata_for(service_id id) const;

	// Invokes fn(key, realm, binding) for every binding held by this
	// context alone (not the parent chain). Diagnostics helper.
	template <class Fn>
	void visit_bindings(Fn&& fn) const {
		for (auto const& [k, b] : bindings_)
			fn(service_id{k.name, k.version}, k.realm, b);
	}

private:
	struct binding_key {
		std::string name;
		std::uint32_t version = 1;
		std::string realm;

		bool operator<(binding_key const& other) const noexcept {
			if (auto c = name.compare(other.name); c != 0)
				return c < 0;
			if (version != other.version)
				return version < other.version;
			return realm < other.realm;
		}
	};

	static binding_key key(service_id id, std::string const& realm);

	std::shared_ptr<context> parent_;
	bool pass_through_bind_ = false;
	std::map<binding_key, binding> bindings_;
	std::map<owned_service_id, std::string, transparent_id_less> realms_;
	std::map<owned_service_id, service_metadata, transparent_id_less> metadata_;
};

} // namespace araya
