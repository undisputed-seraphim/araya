#pragma once

#include <cstddef>
#include <cstdint>

// The medulla native module boundary, ABI version 1.
//
// Everything here is plain C: no C++ exceptions, standard-library types,
// RTTI identities, or virtual interfaces cross this boundary. All functions
// return an error code (0 = success, negative = failure).

#define MEDULLA_ABI_OK 0
#define MEDULLA_ABI_ERROR -1
#define MEDULLA_ABI_UNRESOLVED -2
#define MEDULLA_ABI_UNSUPPORTED -3

#define MEDULLA_MODE_EMIT 0u
#define MEDULLA_MODE_PARALLEL 1u
#define MEDULLA_MODE_SERIAL 2u
#define MEDULLA_MODE_WATERFALL 3u

#define MEDULLA_ABI_VERSION 1u

extern "C" {

// Opaque handles. A fiber handle identifies one live activation; a
// registration handle identifies an owned effect (provide/on) for early
// release.
using medulla_fiber = void*;
using medulla_handle = void*;

struct medulla_config_pair {
    const char* key;
    const char* value;
};

struct medulla_dependency_v1 {
    const char* name;
    std::uint32_t version;
    int required;  // 1 = required, 0 = optional
};

struct medulla_provision_v1 {
    const char* name;
    std::uint32_t version;
};

struct medulla_cleanup_v1 {
    void (*run)(void* data);  // may be null
    void* data;
};

struct medulla_instance_v1 {
    // Initiates activation on the control strand. complete() must be called
    // exactly once, from strand context (use host->post to return to the
    // strand after asynchronous work). code = 0 for success; a nonzero code
    // fails the activation with `error` as the diagnostic.
    void (*apply)(medulla_instance_v1* self, medulla_fiber fiber,
                  void (*complete)(medulla_fiber fiber, int code,
                                   const char* error));

    // Optional in-place reconfiguration. Returns nonzero on success.
    // May be null.
    int (*reconfigure)(medulla_instance_v1* self,
                       const medulla_config_pair* pairs, std::size_t count);

    void (*destroy)(medulla_instance_v1* self);
};

struct medulla_host_api_v1 {
    std::uint32_t version;  // MEDULLA_ABI_VERSION

    // Resolve a declared service. On success *out receives a borrowed pointer
    // that remains valid until the fiber's teardown completes.
    // MEDULLA_ABI_UNRESOLVED if the service is not available.
    int (*require)(medulla_fiber fiber, const char* name,
                   std::uint32_t version, void** out);

    // Like require, but missing services are not an error.
    int (*find)(medulla_fiber fiber, const char* name, std::uint32_t version,
                void** out);

    // Publish a service. The host takes ownership of `value` and releases it
    // through `destroy_value` (which may be null for immortal objects).
    // On success *out receives a registration handle.
    int (*provide)(medulla_fiber fiber, const char* name,
                   std::uint32_t version, void* value,
                   void (*destroy_value)(void*), medulla_handle* out);

    // Run `setup` on the strand. If it installs a cleanup through `cleanup`,
    // the host records it for reverse-order teardown and returns a
    // registration handle in *out (may be null).
    int (*effect)(medulla_fiber fiber,
                  void (*setup)(void* data, medulla_cleanup_v1* cleanup),
                  void* data, medulla_handle* out);

    // Register an event listener (emit/parallel/serial modes). invoke() runs
    // on the strand and must consume the message synchronously. On success
    // *out receives a registration handle.
    int (*on)(medulla_fiber fiber, const char* name, std::uint32_t version,
              std::uint32_t mode, void (*invoke)(void* data, const void* msg),
              void* data, medulla_handle* out);

    // Early-release a registration handle. Idempotent.
    void (*release)(medulla_handle handle);

    // Poll the fiber's cooperative stop request.
    int (*stop_requested)(medulla_fiber fiber);

    // Schedule fn on the control strand. Host functions and complete() may
    // only be called from strand context; use this to return to it.
    void (*post)(medulla_fiber fiber, void (*fn)(void* data), void* data);
};

struct medulla_plugin_descriptor_v1 {
    std::uint32_t abi_version;  // MEDULLA_ABI_VERSION
    const char* name;
    const medulla_dependency_v1* inject;
    std::size_t inject_count;
    const medulla_provision_v1* provide;
    std::size_t provide_count;
    // Creates one instance; null on failure. The returned pointer is passed
    // as `self` to every instance function and released via destroy.
    medulla_instance_v1* (*create)(const medulla_config_pair* pairs,
                                   std::size_t count);
};

using medulla_plugin_entry_fn =
    const medulla_plugin_descriptor_v1* (*)(const medulla_host_api_v1*);

// Every native module exports this symbol.
const medulla_plugin_descriptor_v1* medulla_plugin_entry_v1(
    const medulla_host_api_v1* host);

}  // extern "C"
