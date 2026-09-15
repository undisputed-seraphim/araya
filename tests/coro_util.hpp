#pragma once

#include "medulla/task.hpp"

#include <utility>

namespace medulla_test {

// Wraps a coroutine lambda so its captures live in the heap-backed
// awaitable frame rather than in the stack temporary that initiates the
// co_spawn. GCC may otherwise alias the temporary's storage, which is
// use-after-scope once the initiating expression ends.
template <class Fn>
struct heap_coroutine_fn {
    Fn fn;

    decltype(fn()) operator()() {
        co_return co_await fn();
    }
};

template <class Fn>
heap_coroutine_fn<std::decay_t<Fn>> heap_coroutine(Fn&& fn) {
    return heap_coroutine_fn<std::decay_t<Fn>>{std::forward<Fn>(fn)};
}

}  // namespace medulla_test
