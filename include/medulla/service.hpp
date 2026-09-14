#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace medulla {

struct service_id {
    std::string_view name;
    std::uint32_t version = 1;

    friend constexpr bool operator==(service_id const&,
                                     service_id const&) noexcept = default;
};

struct owned_service_id {
    std::string name;
    std::uint32_t version = 1;

    owned_service_id() = default;
    owned_service_id(service_id id) : name(id.name), version(id.version) {}

    constexpr operator service_id() const noexcept {
        return service_id{name, version};
    }

    friend constexpr bool operator==(owned_service_id const&,
                                     owned_service_id const&) noexcept =
        default;
};

struct transparent_id_less {
    using is_transparent = void;

    static bool less(std::string_view a, std::uint32_t av, std::string_view b,
                     std::uint32_t bv) noexcept {
        if (auto c = a.compare(b); c != 0)
            return c < 0;
        return av < bv;
    }

    bool operator()(owned_service_id const& a,
                    owned_service_id const& b) const noexcept {
        return less(a.name, a.version, b.name, b.version);
    }

    bool operator()(owned_service_id const& a, service_id const& b) const
        noexcept {
        return less(a.name, a.version, b.name, b.version);
    }

    bool operator()(service_id const& a, owned_service_id const& b) const
        noexcept {
        return less(a.name, a.version, b.name, b.version);
    }
};

using service_metadata = std::map<std::string, std::string>;

template <class T>
struct service_key {
    service_id id;

    constexpr service_key(service_id i) noexcept : id(i) {}
    constexpr service_key(std::string_view name,
                          std::uint32_t version = 1) noexcept
        : id{name, version} {}
};

enum class provider_state : std::uint8_t {
    loading,
    active,
    retiring,
};

struct binding {
    std::shared_ptr<void> value;
    std::uint64_t provider = 0;
    provider_state state = provider_state::active;
};

template <class T>
class service_lease {
public:
    service_lease() = default;

    service_lease(std::shared_ptr<T> value, std::uint64_t provider)
        : value_(std::move(value)), provider_(provider) {}

    T* get() const noexcept { return value_.get(); }
    T* operator->() const noexcept { return value_.get(); }
    T& operator*() const noexcept { return *value_; }

    explicit operator bool() const noexcept { return !!value_; }

    std::shared_ptr<T> const& shared() const noexcept { return value_; }

    std::uint64_t provider() const noexcept { return provider_; }

private:
    std::shared_ptr<T> value_;
    std::uint64_t provider_ = 0;
};

}  // namespace medulla
