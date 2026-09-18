#pragma once

#include "araya/context.hpp"
#include "araya/service.hpp"
#include "araya/task.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/experimental/promise.hpp>
#include <boost/asio/experimental/impl/promise.hpp>
#include <boost/asio/experimental/use_promise.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace araya {

enum class dispatch_mode : std::uint8_t {
    emit,
    parallel,
    serial,
    waterfall,
    bail,
};

// Options applied when a typed listener is registered through
// add_listener / plugin_context::on. Raw (ABI) listeners take no options.
struct listener_options {
    // Insert the listener ahead of the existing ones.
    bool prepend = false;
    // Remove the listener just before its first invocation, so it runs
    // at most once.
    bool once = false;
    // Deliver regardless of the dispatching scope (see scope below).
    bool global = false;
    // When set, the listener delivers only to dispatches carrying a scope
    // with the same realm label for the event key (the Section 5.2.1
    // realm machinery). Dispatches without a scope reach every listener.
    context const* scope = nullptr;
};

template <class Message, dispatch_mode Mode>
struct event_key {
    service_id id;

    constexpr event_key(std::string_view name,
                        std::uint32_t version = 1) noexcept
        : id{name, version} {}

    constexpr event_key(service_id i) noexcept : id(i) {}
};

template <class Message>
class waterfall_continuation;

namespace detail {

template <class Message>
struct waterfall_slot {
    std::uint64_t owner = 0;
    std::uint64_t token = 0;
    std::function<boost::asio::awaitable<Message>(
        Message const&, waterfall_continuation<Message>)>
        fn;
    listener_options opts;
};

template <class Message>
struct notify_slot {
    std::uint64_t owner = 0;
    std::uint64_t token = 0;
    std::function<boost::asio::awaitable<void>(Message const&)> fn;
    listener_options opts;
};

// A bail listener returns true to stop the chain (no further listeners
// run); dispatch reports whether anyone bailed.
template <class Message>
struct bail_slot {
    std::uint64_t owner = 0;
    std::uint64_t token = 0;
    std::function<boost::asio::awaitable<bool>(Message const&)> fn;
    listener_options opts;
};

template <class Message, class Fn>
auto make_notify_listener(Fn fn) {
    return [fn = std::move(fn)](
               Message const& m) -> boost::asio::awaitable<void> {
        using result_t = std::invoke_result_t<Fn const&, Message const&>;
        if constexpr (std::is_void_v<result_t>) {
            std::invoke(fn, m);
            co_return;
        } else {
            co_await std::invoke(fn, m);
        }
    };
}

template <class Message, class Fn>
auto make_waterfall_listener(Fn fn) {
    return [fn = std::move(fn)](
               Message const& m,
               waterfall_continuation<Message> next)
        -> boost::asio::awaitable<Message> {
        using result_t = std::invoke_result_t<
            Fn const&, Message const&, waterfall_continuation<Message>>;
        if constexpr (std::same_as<result_t, Message>) {
            co_return std::invoke(fn, m, std::move(next));
        } else {
            co_return co_await std::invoke(fn, m, std::move(next));
        }
    };
}

template <class Message, class Fn>
auto make_bail_listener(Fn fn) {
    return [fn = std::move(fn)](
               Message const& m) -> boost::asio::awaitable<bool> {
        using result_t = std::invoke_result_t<Fn const&, Message const&>;
        if constexpr (std::is_same_v<result_t, bool>) {
            co_return std::invoke(fn, m);
        } else {
            co_return co_await std::invoke(fn, m);
        }
    };
}

template <class Message, dispatch_mode Mode, class Fn>
auto make_listener(Fn&& fn) {
    if constexpr (Mode == dispatch_mode::waterfall)
        return make_waterfall_listener<Message>(std::forward<Fn>(fn));
    else if constexpr (Mode == dispatch_mode::bail)
        return make_bail_listener<Message>(std::forward<Fn>(fn));
    else
        return make_notify_listener<Message>(std::forward<Fn>(fn));
}

template <class Message, class Fn>
boost::asio::awaitable<void> invoke_owned(Fn const& fn, Message msg) {
    co_await fn(msg);
}

template <class Message, class Fn>
boost::asio::awaitable<void> invoke_owned_raw(Fn const& fn, Message msg) {
    co_await fn(&msg);
}

}  // namespace detail

template <class Message>
class waterfall_continuation {
public:
    waterfall_continuation(
        std::shared_ptr<std::vector<detail::waterfall_slot<Message>>> listeners,
        std::size_t index)
        : listeners_(std::move(listeners)), index_(index) {}

    boost::asio::awaitable<Message> operator()(Message msg) {
        if (index_ >= listeners_->size())
            co_return msg;
        co_return co_await (*listeners_)[index_].fn(
            msg, waterfall_continuation{listeners_, index_ + 1});
    }

private:
    std::shared_ptr<std::vector<detail::waterfall_slot<Message>>> listeners_;
    std::size_t index_ = 0;
};

namespace detail {

template <class Message>
struct raw_slot {
    std::uint64_t owner = 0;
    std::uint64_t token = 0;
    std::function<boost::asio::awaitable<void>(void const*)> fn;
};

struct event_entry_base {
    virtual ~event_entry_base() = default;
    virtual dispatch_mode mode() const noexcept = 0;
    virtual std::size_t count() const noexcept = 0;
    virtual void remove(std::uint64_t token) = 0;
    virtual void add_raw(std::uint64_t token, std::uint64_t owner,
                         std::function<boost::asio::awaitable<void>(void const*)> fn) = 0;
};

// The listener-slot type for each dispatch mode.
template <class Message, dispatch_mode Mode>
struct mode_slot;

template <class Message>
struct mode_slot<Message, dispatch_mode::waterfall> {
    using type = waterfall_slot<Message>;
};

template <class Message>
struct mode_slot<Message, dispatch_mode::bail> {
    using type = bail_slot<Message>;
};

template <class Message, dispatch_mode Mode>
struct mode_slot {
    using type = notify_slot<Message>;
};

struct parallel_state {
    explicit parallel_state(std::size_t total,
                            boost::asio::any_io_executor strand)
        : remaining(total), gate(make_gate(*this, std::move(strand))) {}

    std::size_t remaining;
    std::exception_ptr first;
    std::move_only_function<void()> complete_handler;
    boost::asio::experimental::promise<void()> gate;

    void complete(std::exception_ptr ep) {
        if (ep && !first)
            first = ep;
        if (--remaining == 0) {
            auto h = std::move(complete_handler);
            h();
        }
    }

private:
    static boost::asio::experimental::promise<void()> make_gate(
        parallel_state& self, boost::asio::any_io_executor strand) {
        boost::asio::experimental::use_promise_t<> token;
        return boost::asio::async_initiate<
            boost::asio::experimental::use_promise_t<>, void()>(
            boost::asio::bind_executor(strand,
                                       [&self](auto&& handler) {
                                           self.complete_handler = handler;
                                       }),
            token);
    }
};

template <class Message, dispatch_mode Mode>
class event_entry_impl : public event_entry_base {
public:
    using slot_t = typename mode_slot<Message, Mode>::type;
    using snapshot_t = std::shared_ptr<std::vector<slot_t>>;

    dispatch_mode mode() const noexcept override { return Mode; }
    std::size_t count() const noexcept override {
        return listeners_->size() + raw_->size();
    }

    void add(slot_t slot, bool prepend) {
        auto updated = std::make_shared<std::vector<slot_t>>(*listeners_);
        if (prepend)
            updated->insert(updated->begin(), std::move(slot));
        else
            updated->push_back(std::move(slot));
        listeners_ = std::move(updated);
    }

    void add_raw(std::uint64_t token, std::uint64_t owner,
                 std::function<boost::asio::awaitable<void>(void const*)> fn) override {
        auto updated = std::make_shared<std::vector<raw_slot<Message>>>(*raw_);
        updated->push_back(raw_slot<Message>{owner, token, std::move(fn)});
        raw_ = std::move(updated);
    }

    void remove(std::uint64_t token) override {
        auto updated = std::make_shared<std::vector<slot_t>>();
        updated->reserve(listeners_->size());
        for (auto& slot : *listeners_) {
            if (slot.token != token)
                updated->push_back(std::move(slot));
        }
        listeners_ = std::move(updated);

        auto raw_updated = std::make_shared<std::vector<raw_slot<Message>>>();
        raw_updated->reserve(raw_->size());
        for (auto& slot : *raw_) {
            if (slot.token != token)
                raw_updated->push_back(std::move(slot));
        }
        raw_ = std::move(raw_updated);
    }

    // Whether a listener delivers a dispatch: without a dispatching scope
    // every listener runs; with one, scoped listeners run only when the
    // realm labels for the event key match (Section 5.2.1), unless the
    // listener is marked global.
    bool deliver(slot_t const& slot, service_id id,
                 context const* scope) const noexcept {
        if (!scope || slot.opts.global || !slot.opts.scope)
            return true;
        return scope->realm_for(id) == slot.opts.scope->realm_for(id);
    }

    void dispatch_emit(
        Message msg, boost::asio::any_io_executor strand, service_id id,
        context const* scope,
        std::shared_ptr<std::move_only_function<void(std::exception_ptr)>> sink) {
        auto snapshot = listeners_;
        for (auto const& slot : *snapshot) {
            if (!deliver(slot, id, scope))
                continue;
            Message m = msg;
            auto t = invoke_owned(slot.fn, std::move(m));
            boost::asio::co_spawn(
                strand, std::move(t),
                [sink](std::exception_ptr ep) {
                    if (ep && *sink)
                        (*sink)(ep);
                });
        }
        auto raw_snapshot = raw_;
        for (auto const& slot : *raw_snapshot) {
            Message m = msg;
            auto t = invoke_owned_raw(slot.fn, std::move(m));
            boost::asio::co_spawn(
                strand, std::move(t),
                [sink](std::exception_ptr ep) {
                    if (ep && *sink)
                        (*sink)(ep);
                });
        }
    }

    boost::asio::awaitable<void> dispatch_parallel(
        Message const& msg, boost::asio::any_io_executor strand,
        service_id id, context const* scope) {
        auto snapshot = listeners_;
        auto raw_snapshot = raw_;
        if (snapshot->empty() && raw_snapshot->empty())
            co_return;
        std::size_t total = 0;
        for (auto const& slot : *snapshot)
            if (deliver(slot, id, scope))
                ++total;
        total += raw_snapshot->size();
        if (total == 0)
            co_return;
        auto state = std::make_shared<parallel_state>(total, strand);
        for (auto const& slot : *snapshot) {
            if (!deliver(slot, id, scope))
                continue;
            auto t = slot.fn(msg);
            boost::asio::co_spawn(strand, std::move(t),
                                  [state](std::exception_ptr ep) {
                                      state->complete(ep);
                                  });
        }
        for (auto const& slot : *raw_snapshot) {
            auto t = slot.fn(&msg);
            boost::asio::co_spawn(strand, std::move(t),
                                  [state](std::exception_ptr ep) {
                                      state->complete(ep);
                                  });
        }
        co_await state->gate(boost::asio::use_awaitable);
        if (state->first)
            std::rethrow_exception(state->first);
    }

    boost::asio::awaitable<void> dispatch_serial(Message const& msg,
                                                 service_id id,
                                                 context const* scope) {
        auto snapshot = listeners_;
        for (auto const& slot : *snapshot) {
            if (!deliver(slot, id, scope))
                continue;
            co_await slot.fn(msg);
        }
        auto raw_snapshot = raw_;
        for (auto const& slot : *raw_snapshot)
            co_await slot.fn(&msg);
    }

    boost::asio::awaitable<Message> dispatch_waterfall(Message const& msg,
                                                       service_id id,
                                                       context const* scope) {
        if (!scope) {
            auto snapshot = listeners_;
            co_return co_await waterfall_continuation<Message>{snapshot,
                                                               0}(msg);
        }
        // Scope filtering: compose the chain from the delivering slots.
        auto filtered = std::make_shared<std::vector<slot_t>>();
        for (auto const& slot : *listeners_)
            if (deliver(slot, id, scope))
                filtered->push_back(slot);
        co_return co_await waterfall_continuation<Message>{filtered, 0}(msg);
    }

    boost::asio::awaitable<bool> dispatch_bail(Message const& msg,
                                               service_id id,
                                               context const* scope) {
        auto snapshot = listeners_;
        for (auto const& slot : *snapshot) {
            if (!deliver(slot, id, scope))
                continue;
            if (co_await slot.fn(msg))
                co_return true;
        }
        co_return false;
    }

private:
    snapshot_t listeners_ = std::make_shared<std::vector<slot_t>>();
    std::shared_ptr<std::vector<raw_slot<Message>>> raw_ =
        std::make_shared<std::vector<raw_slot<Message>>>();
};

}  // namespace detail

class event_bus {
public:
    using strand_type = boost::asio::strand<boost::asio::any_io_executor>;

    explicit event_bus(strand_type control_strand);

    std::exception_ptr error;

    boost::asio::any_io_executor executor() const noexcept { return strand_; }

    // Whether the calling thread is executing within this bus's control
    // strand. Dispatch and listener registration must run there; teardown
    // paths that may run elsewhere use this to skip notification.
    bool on_control_strand() const noexcept {
        return strand_.running_in_this_thread();
    }

    void set_diagnostic_sink(
        std::move_only_function<void(std::exception_ptr)> sink);

    void report(std::exception_ptr ep);

    template <class Message, dispatch_mode Mode, class Fn>
    std::uint64_t add_listener(event_key<Message, Mode> const& key, Fn&& fn,
                               std::uint64_t owner,
                               listener_options opts = {}) {
        ensure_on_strand();
        auto* base = entry_base_for(key.id);
        if (!base) {
            entries_.insert_or_assign(
                owned_service_id(key.id),
                std::make_unique<detail::event_entry_impl<Message, Mode>>());
            base = entry_base_for(key.id);
        } else if (base->mode() != Mode) {
            throw std::logic_error("listener registered with conflicting "
                                   "dispatch mode for event '" +
                                   std::string(key.id.name) + "'");
        }
        auto* impl = static_cast<detail::event_entry_impl<Message, Mode>*>(
            base);
        using slot_t =
            typename detail::event_entry_impl<Message, Mode>::slot_t;
        auto token = next_token_++;
        std::decay_t<decltype(slot_t::fn)> listener =
            detail::make_listener<Message, Mode>(std::forward<Fn>(fn));
        if (opts.once) {
            // Remove-then-run: the slot leaves the list before its first
            // invocation. The in-flight dispatch is unaffected (it holds
            // a COW snapshot), and repeat removals are token no-ops.
            listener = [listener = std::move(listener),
                        remover = [this, id = owned_service_id(key.id),
                                   token]() {
                            remove_listener(id, token);
                        }]<class... Args>(
                           Args&&... args) mutable -> decltype(auto) {
                remover();
                return listener(std::forward<Args>(args)...);
            };
        }
        impl->add(slot_t{owner, token, std::move(listener), opts},
                  opts.prepend);
        return token;
    }

    void remove_listener(service_id id, std::uint64_t token);

    std::uint64_t add_raw_listener(
        service_id id, dispatch_mode mode,
        std::function<boost::asio::awaitable<void>(void const*)> fn,
        std::uint64_t owner);

    std::size_t listener_count(service_id id);

    template <class Message>
    void dispatch(event_key<Message, dispatch_mode::emit> const& key,
                  Message msg, context const* scope = nullptr) {
        ensure_on_strand();
        auto* impl = typed_entry<Message, dispatch_mode::emit>(key.id);
        if (!impl)
            return;
        impl->dispatch_emit(std::move(msg), strand_, key.id, scope, sink_);
    }

    template <class Message>
    boost::asio::awaitable<void> dispatch(
        event_key<Message, dispatch_mode::parallel> const& key,
        Message const& msg, context const* scope = nullptr) {
        ensure_on_strand();
        auto* impl = typed_entry<Message, dispatch_mode::parallel>(key.id);
        if (!impl)
            co_return;
        co_await impl->dispatch_parallel(msg, strand_, key.id, scope);
    }

    template <class Message>
    boost::asio::awaitable<void> dispatch(
        event_key<Message, dispatch_mode::serial> const& key,
        Message const& msg, context const* scope = nullptr) {
        ensure_on_strand();
        auto* impl = typed_entry<Message, dispatch_mode::serial>(key.id);
        if (!impl)
            co_return;
        co_await impl->dispatch_serial(msg, key.id, scope);
    }

    template <class Message>
    boost::asio::awaitable<Message> dispatch(
        event_key<Message, dispatch_mode::waterfall> const& key,
        Message const& msg, context const* scope = nullptr) {
        ensure_on_strand();
        auto* impl = typed_entry<Message, dispatch_mode::waterfall>(key.id);
        if (!impl)
            co_return msg;
        co_return co_await impl->dispatch_waterfall(msg, key.id, scope);
    }

    template <class Message>
    boost::asio::awaitable<bool> dispatch(
        event_key<Message, dispatch_mode::bail> const& key,
        Message const& msg, context const* scope = nullptr) {
        ensure_on_strand();
        auto* impl = typed_entry<Message, dispatch_mode::bail>(key.id);
        if (!impl)
            co_return false;
        co_return co_await impl->dispatch_bail(msg, key.id, scope);
    }

private:
    void ensure_on_strand() const;

    detail::event_entry_base* entry_base_for(service_id id) noexcept;

    template <class Message, dispatch_mode Mode>
    detail::event_entry_impl<Message, Mode>* typed_entry(service_id id) {
        auto* base = entry_base_for(id);
        if (!base)
            return nullptr;
        if (base->mode() != Mode)
            throw std::logic_error("dispatch mode mismatch for event '" +
                                   std::string(id.name) + "'");
        return static_cast<detail::event_entry_impl<Message, Mode>*>(base);
    }

    strand_type strand_;
    std::shared_ptr<std::move_only_function<void(std::exception_ptr)>> sink_ =
        std::make_shared<std::move_only_function<void(std::exception_ptr)>>();
    std::map<owned_service_id, std::unique_ptr<detail::event_entry_base>,
             transparent_id_less>
        entries_;
    std::uint64_t next_token_ = 1;
};

}  // namespace araya
