#include <catch2/catch_test_macros.hpp>

#include "medulla/context.hpp"
#include "medulla/plugin_context.hpp"
#include "medulla/service.hpp"

#include <memory>
#include <string>

namespace {

struct database {
    std::string name;
};

struct cache {
    int hits = 0;
};

inline constexpr medulla::service_key<database> db_key{"example.database", 1};
inline constexpr medulla::service_key<cache> cache_key{"example.cache", 1};

}  // namespace

TEST_CASE("root context resolves a bound service") {
    auto root = medulla::context::root();
    auto db = std::make_shared<database>("production");
    root->bind(db_key.id, medulla::binding{db, 7});

    auto const* b = root->lookup(db_key.id);
    REQUIRE(b != nullptr);
    CHECK(b->provider == 7);

    auto lease =
        medulla::service_lease<database>(std::static_pointer_cast<database>(
                                             b->value),
                                         b->provider);
    CHECK(lease->name == "production");
}

TEST_CASE("child scope overrides parent binding without mutating parent") {
    auto root = medulla::context::root();
    auto child = root->make_child();

    auto production = std::make_shared<database>("production");
    auto test = std::make_shared<database>("test");

    root->bind(db_key.id, medulla::binding{production, 1});
    child->bind(db_key.id, medulla::binding{test, 2});

    CHECK(root->lookup(db_key.id)->value == production);
    CHECK(child->lookup(db_key.id)->value == test);

    child->unbind(db_key.id);
    CHECK(root->lookup(db_key.id)->value == production);
    CHECK(child->lookup(db_key.id)->value == production);
}

TEST_CASE("child scope falls back to parent bindings") {
    auto root = medulla::context::root();
    auto child = root->make_child();
    auto grandchild = child->make_child();

    auto db = std::make_shared<database>("production");
    root->bind(db_key.id, medulla::binding{db, 1});

    CHECK(child->lookup(db_key.id)->value == db);
    CHECK(grandchild->lookup(db_key.id)->value == db);
}

TEST_CASE("service identity includes name and version") {
    auto root = medulla::context::root();
    auto v1 = std::make_shared<database>("v1");
    auto v2 = std::make_shared<database>("v2");

    root->bind(medulla::service_id{"example.database", 1},
               medulla::binding{v1, 1});
    root->bind(medulla::service_id{"example.database", 2},
               medulla::binding{v2, 2});

    CHECK(root->lookup(medulla::service_id{"example.database", 1})->value ==
          v1);
    CHECK(root->lookup(medulla::service_id{"example.database", 2})->value ==
          v2);
    CHECK(root->lookup(medulla::service_id{"example.database", 3}) == nullptr);
    CHECK(root->lookup(medulla::service_id{"other.database", 1}) == nullptr);
}

TEST_CASE("scope metadata merges from root with child overrides") {
    auto root = medulla::context::root();
    auto child = root->make_child();

    root->set_metadata(db_key.id, {{"region", "eu"}, {"quota", "100"}});
    child->set_metadata(db_key.id, {{"quota", "10"}, {"tenant", "a"}});

    auto md = child->metadata_for(db_key.id);
    CHECK(md.at("region") == "eu");
    CHECK(md.at("quota") == "10");
    CHECK(md.at("tenant") == "a");

    auto root_md = root->metadata_for(db_key.id);
    CHECK(root_md.at("quota") == "100");
    CHECK_FALSE(root_md.contains("tenant"));

    CHECK(child->metadata_for(cache_key.id).empty());
}

TEST_CASE("plugin_context provides and requires services") {
    auto root = medulla::context::root();
    auto act = std::make_shared<medulla::activation>(root);
    medulla::plugin_context ctx{act};

    auto db = std::make_shared<database>("production");
    auto reg = ctx.provide(db_key, db);

    auto lease = ctx.require<database>(db_key);
    CHECK(lease->name == "production");

    auto found = ctx.find<database>(db_key);
    REQUIRE(found);
    CHECK((*found)->name == "production");

    reg.release();
    CHECK_FALSE(ctx.find<database>(db_key));
    CHECK_THROWS_AS(ctx.require<database>(db_key),
                    medulla::resolution_error);
}

TEST_CASE("resolution_error carries service identity") {
    auto root = medulla::context::root();
    auto act = std::make_shared<medulla::activation>(root);
    medulla::plugin_context ctx{act};

    try {
        ctx.require<database>(db_key);
        FAIL("expected resolution_error");
    } catch (medulla::resolution_error const& e) {
        CHECK(e.service_name == "example.database");
        CHECK(e.version == 1);
    }
}

TEST_CASE("lease survives provider withdrawal") {
    auto root = medulla::context::root();
    auto act = std::make_shared<medulla::activation>(root);
    medulla::plugin_context ctx{act};

    auto reg = ctx.provide(db_key, std::make_shared<database>("production"));
    auto lease = ctx.require<database>(db_key);

    reg.release();
    CHECK_FALSE(ctx.find<database>(db_key));
    CHECK(lease->name == "production");
}

TEST_CASE("teardown withdraws provided services in reverse order") {
    auto root = medulla::context::root();
    auto act = std::make_shared<medulla::activation>(root);
    medulla::plugin_context ctx{act};

    ctx.provide(db_key, std::make_shared<database>("a"));
    ctx.provide(cache_key, std::make_shared<cache>());

    CHECK(ctx.find<database>(db_key));
    CHECK(ctx.find<cache>(cache_key));

    act->teardown();
    CHECK_FALSE(ctx.find<database>(db_key));
    CHECK_FALSE(ctx.find<cache>(cache_key));
    CHECK(root->lookup(db_key.id) == nullptr);
    CHECK(root->lookup(cache_key.id) == nullptr);
}

TEST_CASE("withdrawing one provider does not remove another's binding") {
    auto root = medulla::context::root();
    auto act_a = std::make_shared<medulla::activation>(root);
    auto act_b = std::make_shared<medulla::activation>(root);
    medulla::plugin_context ctx_a{act_a};
    medulla::plugin_context ctx_b{act_b};

    auto reg_a = ctx_a.provide(db_key, std::make_shared<database>("a"));
    ctx_b.provide(db_key, std::make_shared<database>("b"));

    auto lease = ctx_b.require<database>(db_key);
    CHECK(lease->name == "b");

    reg_a.release();
    REQUIRE(root->lookup(db_key.id) != nullptr);
    CHECK(root->lookup(db_key.id)->value != nullptr);

    auto after = ctx_b.require<database>(db_key);
    CHECK(after->name == "b");
}
