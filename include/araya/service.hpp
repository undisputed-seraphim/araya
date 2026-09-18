#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

// Services and leases: the vocabulary of bindings.
//
// Threading: service_key/service_id/owned_service_id are plain values.
// service_lease<T> is shared_ptr-backed - see the class comment for the
// value-lifetime vs binding-lifetime distinction. bindings live in
// contexts and are mutated by the engine only, on the control strand.
namespace araya {

// The erased identity of a service: name + version. A name_view into
// caller-owned storage (string literals in practice); owned_service_id is
// its owning counterpart for maps and long-lived records.
struct service_id {
	std::string_view name;
	std::uint32_t version = 1;

	friend constexpr bool operator==(service_id const&, service_id const&) noexcept = default;
};

struct owned_service_id {
	std::string name;
	std::uint32_t version = 1;

	owned_service_id() = default;
	owned_service_id(service_id id)
		: name(id.name)
		, version(id.version) {}

	constexpr operator service_id() const noexcept { return service_id{name, version}; }

	friend constexpr bool operator==(owned_service_id const&, owned_service_id const&) noexcept = default;
};

struct transparent_id_less {
	using is_transparent = void;

	static bool less(std::string_view a, std::uint32_t av, std::string_view b, std::uint32_t bv) noexcept {
		if (auto c = a.compare(b); c != 0)
			return c < 0;
		return av < bv;
	}

	bool operator()(owned_service_id const& a, owned_service_id const& b) const noexcept {
		return less(a.name, a.version, b.name, b.version);
	}

	bool operator()(owned_service_id const& a, service_id const& b) const noexcept {
		return less(a.name, a.version, b.name, b.version);
	}

	bool operator()(service_id const& a, owned_service_id const& b) const noexcept {
		return less(a.name, a.version, b.name, b.version);
	}
};

using service_metadata = std::map<std::string, std::string>;

template <class T>
struct service_key {
	using value_type = T;

	service_id id;

	constexpr service_key(service_id i) noexcept
		: id(i) {}
	constexpr service_key(std::string_view name, std::uint32_t version = 1) noexcept
		: id{name, version} {}
};

// How far a published binding is through the provider's lifecycle. The
// engine writes it; plugin code reads it only through the resolved lease
// (which never hands out non-active bindings).
enum class provider_state : std::uint8_t {
	loading,
	active,
	retiring,
};

struct binding {
	std::shared_ptr<void> value;
	std::uint64_t provider = 0;
	provider_state state = provider_state::active;
	// The availability gate (the tier-2 extension, ArayaMachine.tla's
	// AvailInit/AvailabilityFlip): evaluated once at provide-time from
	// the optional check predicate. Promotion-only - runtime::
	// signal_availability flips it true; deactivation is retirement
	// (the paper's lifecycle DAG has no active -> loading edge).
	bool available = true;
};

// A resolved service: the value, the providing fiber, and the merged
// interception metadata.
//
// Lifetime: the lease holds a shared_ptr to the value, so the object
// outlives any teardown of the provider's activation. The binding itself
// is not retained: after the provider unloads, the same key may be
// re-provided by another fiber. Hold a lease for the object; require()
// again for the current resolution.
template <class T>
class service_lease {
public:
	service_lease() = default;

	service_lease(std::shared_ptr<T> value, std::uint64_t provider, service_metadata metadata = {})
		: value_(std::move(value))
		, provider_(provider)
		, metadata_(std::move(metadata)) {}

	T* get() const noexcept { return value_.get(); }
	T* operator->() const noexcept { return value_.get(); }
	T& operator*() const noexcept { return *value_; }

	explicit operator bool() const noexcept { return !!value_; }

	std::shared_ptr<T> const& shared() const noexcept { return value_; }

	std::uint64_t provider() const noexcept { return provider_; }

	// The interception metadata merged at access (Definition 27): the
	// component-declared metadata overlaid with the context-carried
	// metadata, which takes priority.
	service_metadata const& metadata() const noexcept { return metadata_; }

private:
	std::shared_ptr<T> value_;
	std::uint64_t provider_ = 0;
	service_metadata metadata_;
};

} // namespace araya
