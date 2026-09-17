#pragma once

#include <cstddef>
#include <cstdint>

// The araya native module boundary, ABI version 1.
//
// Everything here is plain C: no C++ exceptions, standard-library types,
// RTTI identities, or virtual interfaces cross this boundary. All functions
// return an error code (0 = success, negative = failure).

#define ARAYA_ABI_OK 0
#define ARAYA_ABI_ERROR -1
#define ARAYA_ABI_UNRESOLVED -2
#define ARAYA_ABI_UNSUPPORTED -3

#define ARAYA_MODE_EMIT 0u
#define ARAYA_MODE_PARALLEL 1u
#define ARAYA_MODE_SERIAL 2u
#define ARAYA_MODE_WATERFALL 3u

#define ARAYA_ABI_VERSION 1u

extern "C" {

// Opaque handles. A fiber handle identifies one live activation; a
// registration handle identifies an owned effect (provide/on) for early
// release.
using araya_fiber = void*;
using araya_handle = void*;

struct araya_config_pair {
    const char* key;
    const char* value;
};

struct araya_dependency_v1 {
    const char* name;
    std::uint32_t version;
    int required;  // 1 = required, 0 = optional
};

struct araya_provision_v1 {
    const char* name;
    std::uint32_t version;
};

struct araya_cleanup_v1 {
    void (*run)(void* data);  // may be null
    void* data;
};

struct araya_instance_v1 {
    // Initiates activation on the control strand. complete() must be called
    // exactly once, from strand context (use host->post to return to the
    // strand after asynchronous work). code = 0 for success; a nonzero code
    // fails the activation with `error` as the diagnostic.
    void (*apply)(araya_instance_v1* self, araya_fiber fiber,
                  void (*complete)(araya_fiber fiber, int code,
                                   const char* error));

    // Optional in-place reconfiguration. Returns nonzero on success.
    // May be null.
    int (*reconfigure)(araya_instance_v1* self,
                       const araya_config_pair* pairs, std::size_t count);

    void (*destroy)(araya_instance_v1* self);
};

struct araya_host_api_v1 {
    std::uint32_t version;  // ARAYA_ABI_VERSION

    // Resolve a declared service. On success *out receives a borrowed pointer
    // that remains valid until the fiber's teardown completes.
    // ARAYA_ABI_UNRESOLVED if the service is not available.
    int (*require)(araya_fiber fiber, const char* name,
                   std::uint32_t version, void** out);

    // Like require, but missing services are not an error.
    int (*find)(araya_fiber fiber, const char* name, std::uint32_t version,
                void** out);

    // Publish a service. The host takes ownership of `value` and releases it
    // through `destroy_value` (which may be null for immortal objects).
    // On success *out receives a registration handle.
    int (*provide)(araya_fiber fiber, const char* name,
                   std::uint32_t version, void* value,
                   void (*destroy_value)(void*), araya_handle* out);

    // Run `setup` on the strand. If it installs a cleanup through `cleanup`,
    // the host records it for reverse-order teardown and returns a
    // registration handle in *out (may be null).
    int (*effect)(araya_fiber fiber,
                  void (*setup)(void* data, araya_cleanup_v1* cleanup),
                  void* data, araya_handle* out);

    // Register an event listener (emit/parallel/serial modes). invoke() runs
    // on the strand and must consume the message synchronously. On success
    // *out receives a registration handle.
    int (*on)(araya_fiber fiber, const char* name, std::uint32_t version,
              std::uint32_t mode, void (*invoke)(void* data, const void* msg),
              void* data, araya_handle* out);

    // Early-release a registration handle. Idempotent.
    void (*release)(araya_handle handle);

    // Poll the fiber's cooperative stop request.
    int (*stop_requested)(araya_fiber fiber);

    // Schedule fn on the control strand. Host functions and complete() may
    // only be called from strand context; use this to return to it.
    void (*post)(araya_fiber fiber, void (*fn)(void* data), void* data);
};

struct araya_plugin_descriptor_v1 {
    std::uint32_t abi_version;  // ARAYA_ABI_VERSION
    const char* name;
    const araya_dependency_v1* inject;
    std::size_t inject_count;
    const araya_provision_v1* provide;
    std::size_t provide_count;
    // Creates one instance; null on failure. The returned pointer is passed
    // as `self` to every instance function and released via destroy.
    araya_instance_v1* (*create)(const araya_config_pair* pairs,
                                   std::size_t count);
};

using araya_plugin_entry_fn =
    const araya_plugin_descriptor_v1* (*)(const araya_host_api_v1*);

// Every native module exports this symbol.
const araya_plugin_descriptor_v1* araya_plugin_entry_v1(
    const araya_host_api_v1* host);

}  // extern "C"
