// A minimal native module compiled as a shared library for the dlopen test.
// It marks its unload by writing a marker file from a destructor.

#include "medulla/abi.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct db_service {
    const char* (*name)(void* self);
};

struct db_value {
    db_service iface;
    std::string name;
};

static const char* db_value_name(void* self) {
    return static_cast<db_value*>(self)->name.c_str();
}

struct mod_instance {
    medulla_instance_v1 base;
    std::string tag;
};

static const medulla_host_api_v1* g_host = nullptr;

static void mod_apply(medulla_instance_v1* self, medulla_fiber fiber,
                      void (*complete)(medulla_fiber, int, const char*)) {
    auto* inst = reinterpret_cast<mod_instance*>(self);
    auto* db = new db_value{{&db_value_name}, inst->tag};
    if (g_host->provide(fiber, "example.db", 1, db,
                        +[](void* p) { delete static_cast<db_value*>(p); },
                        nullptr) != MEDULLA_ABI_OK) {
        complete(fiber, MEDULLA_ABI_ERROR, "provide failed");
        return;
    }
    complete(fiber, MEDULLA_ABI_OK, nullptr);
}

static medulla_instance_v1* make_mod(
    const medulla_config_pair* pairs, std::size_t count) {
    auto* inst = new mod_instance{};
    inst->base.apply = &mod_apply;
    inst->base.reconfigure = nullptr;
    inst->base.destroy = +[](medulla_instance_v1* self) {
        delete reinterpret_cast<mod_instance*>(self);
    };
    for (std::size_t i = 0; i < count; ++i) {
        if (std::strcmp(pairs[i].key, "tag") == 0)
            inst->tag = pairs[i].value;
    }
    return &inst->base;
}

static const medulla_provision_v1 g_provide[]{{"example.db", 1}};

static const medulla_plugin_descriptor_v1 g_desc{
    MEDULLA_ABI_VERSION, "dlopen-module", nullptr, 0, g_provide, 1, &make_mod};

}  // namespace

extern "C" const medulla_plugin_descriptor_v1* medulla_plugin_entry_v1(
    const medulla_host_api_v1* host) {
    g_host = host;
    return &g_desc;
}

extern "C" __attribute__((destructor)) void medulla_test_module_unload() {
    std::FILE* marker = std::fopen("/tmp/medulla_module_unloaded.marker", "w");
    if (marker)
        std::fclose(marker);
}
