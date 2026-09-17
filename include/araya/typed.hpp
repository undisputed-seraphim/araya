#pragma once

// The compile-time encodings of the paper's static fragment (Section 6.4 /
// 6.7): capability-typed access, acquisition-tagged leases, witness
// concepts, and typestate host handles. All are facades over the erased
// runtime; the engine remains the single source of truth and still
// verifies every transition at runtime.

#include "araya/effects.hpp"
#include "araya/fiber_handle.hpp"
#include "araya/plugin_context.hpp"
#include "araya/runtime.hpp"

#include <concepts>
#include <type_traits>
#include <utility>

namespace araya {

// ---------------------------------------------------------------------------
// Capability sets
//
// A capability set is a compile-time list of pointers to static service
// keys. Declared access is checked against it at compile time: requiring or
// providing a key outside the set does not compile.

template <auto... Ks>
struct capabilities {
    template <auto K>
    static constexpr bool contains = ((K->id == Ks->id) || ...);
};

// ---------------------------------------------------------------------------
// Witness concepts
//
// The paper's witnesses (an inverse reverts its effect; a key's operations
// commute) are supplied where the definition is written, not checked where
// they are used. C++ can check their presence at compile time, not their
// truth; the provider remains the source of the claim (p. 59).

template <class F>
concept reversible_effect = std::invocable<F> && requires(F&& f) {
    { std::forward<F>(f)() } -> std::convertible_to<cleanup_action>;
};

// A provider marks a service type commutative by specializing this trait.
template <class T>
inline constexpr bool is_commutative_key_v = false;

template <class T>
concept commutative_key = is_commutative_key_v<T>;

// ---------------------------------------------------------------------------
// Acquisition-tagged leases
//
// Phantom tags make "this lease came from the committed view" a type-level
// fact rather than a convention.

struct committed_tag {};
struct live_tag {};

template <class T, class Tag>
class tagged_lease {
public:
    tagged_lease() = default;

    tagged_lease(service_lease<T> lease)
        : lease_(std::move(lease)) {}

    T* get() const noexcept { return lease_.get(); }
    T* operator->() const noexcept { return lease_.get(); }
    T& operator*() const noexcept { return *lease_; }

    explicit operator bool() const noexcept { return !!lease_; }

    std::shared_ptr<T> const& shared() const noexcept {
        return lease_.shared();
    }

    std::uint64_t provider() const noexcept { return lease_.provider(); }

    service_metadata const& metadata() const noexcept {
        return lease_.metadata();
    }

    service_lease<T> const& untagged() const noexcept { return lease_; }

private:
    service_lease<T> lease_;
};

// ---------------------------------------------------------------------------
// The typed plugin context
//
// The compile-time facade over plugin_context. require/find/provide are
// constrained to the capability set; provide additionally requires the
// commutative-key witness.

template <typename Caps>
class typed_context {
public:
    explicit typed_context(plugin_context& ctx) : ctx_(&ctx) {}

    plugin_context& erased() noexcept { return *ctx_; }

    template <auto K>
        requires Caps::template contains<K>
    auto require() {
        auto const& key = *K;
        auto b = ctx_->find_binding(key.id);
        if (!b)
            throw resolution_error(key.id);
        using value_type = typename std::remove_pointer_t<decltype(K)>::
            value_type;
        return tagged_lease<value_type, committed_tag>(
            service_lease<value_type>(
                std::static_pointer_cast<value_type>(b->value),
                b->provider, ctx_->merged_metadata(key.id)));
    }

    template <auto K>
        requires Caps::template contains<K>
    auto find() {
        auto const& key = *K;
        auto b = ctx_->find_binding(key.id);
        if (!b)
            return std::optional<tagged_lease<
                typename std::remove_pointer_t<decltype(K)>::value_type,
                committed_tag>>{std::nullopt};
        using value_type = typename std::remove_pointer_t<decltype(K)>::
            value_type;
        return std::optional<tagged_lease<value_type, committed_tag>>(
            service_lease<value_type>(
                std::static_pointer_cast<value_type>(b->value),
                b->provider, ctx_->merged_metadata(key.id)));
    }

    template <auto K>
        requires Caps::template contains<K> &&
                 commutative_key<
                     typename std::remove_pointer_t<decltype(K)>::value_type>
    registration provide(
        std::shared_ptr<
            typename std::remove_pointer_t<decltype(K)>::value_type>
            service) {
        return ctx_->provide(*K, std::move(service));
    }

    template <reversible_effect Setup>
    registration effect(Setup&& setup) {
        return ctx_->effect(std::forward<Setup>(setup));
    }

    template <class Message, dispatch_mode Mode, class Fn>
    registration on(event_key<Message, Mode> const& key, Fn&& fn) {
        return ctx_->on(key, std::forward<Fn>(fn));
    }

    // Child instantiation needs no declaration: the paper's instantiating
    // iteration is the one registry effect every context may take.
    boost::asio::awaitable<fiber_handle> mount(component_spec spec) {
        return ctx_->mount(std::move(spec));
    }

    std::stop_token stop_token() const noexcept {
        return ctx_->stop_token();
    }

private:
    plugin_context* ctx_;
};

// ---------------------------------------------------------------------------
// Typestate host handles
//
// The phantom mirrors the runtime state; every transition is still verified
// by the engine, so the typestate is ergonomics and honest failure, not a
// proof.

template <fiber_state S>
class typed_handle {
public:
    static typed_handle<fiber_state::active> claim(fiber_handle h) {
        return typed_handle<fiber_state::active>(std::move(h));
    }

    fiber_id id() const noexcept { return h_.id(); }

    fiber_state state() const noexcept { return S; }

    void cancel() requires(S == fiber_state::active) { h_.cancel(); }

    // Consuming transition: retiring an active handle moves it to the
    // inactive state.
    boost::asio::awaitable<typed_handle<fiber_state::inactive>>
    retire(runtime& rt) && requires(S == fiber_state::active) {
        co_await rt.retire(h_);
        co_return typed_handle<fiber_state::inactive>(std::move(h_));
    }

    fiber_handle const& erased() const noexcept { return h_; }

private:
    template <fiber_state>
    friend class typed_handle;

    explicit typed_handle(fiber_handle h) : h_(std::move(h)) {}

    fiber_handle h_;
};

}  // namespace araya
