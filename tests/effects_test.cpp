#include <catch2/catch_test_macros.hpp>

#include "araya/effects.hpp"
#include "araya/plugin_context.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

araya::registration record(std::vector<std::string>& log, std::string tag,
                             araya::plugin_context& ctx) {
    return ctx.effect([&log, tag]() -> araya::cleanup_action {
        log.push_back("setup:" + tag);
        return [&log, tag] { log.push_back("cleanup:" + tag); };
    });
}

}  // namespace

TEST_CASE("cleanup runs in reverse registration order") {
    auto root = araya::context::root();
    auto act = std::make_shared<araya::activation>(root);
    araya::plugin_context ctx{act};

    std::vector<std::string> log;
    record(log, "1", ctx);
    record(log, "2", ctx);
    record(log, "3", ctx);

    act->teardown();
    CHECK(log == std::vector<std::string>{"setup:1", "setup:2", "setup:3",
                                          "cleanup:3", "cleanup:2",
                                          "cleanup:1"});
}

TEST_CASE("early release runs the cleanup immediately") {
    auto root = araya::context::root();
    auto act = std::make_shared<araya::activation>(root);
    araya::plugin_context ctx{act};

    std::vector<std::string> log;
    record(log, "1", ctx);
    auto reg = record(log, "2", ctx);
    record(log, "3", ctx);

    reg.release();

    act->teardown();
    CHECK(log == std::vector<std::string>{"setup:1", "setup:2", "setup:3",
                                          "cleanup:2", "cleanup:3",
                                          "cleanup:1"});
}

TEST_CASE("throwing setup records nothing and keeps earlier effects") {
    auto root = araya::context::root();
    auto act = std::make_shared<araya::activation>(root);
    araya::plugin_context ctx{act};

    std::vector<std::string> log;
    record(log, "1", ctx);
    CHECK_THROWS_AS(ctx.effect([]() -> araya::cleanup_action {
                        throw std::runtime_error("setup failed");
                    }),
                    std::runtime_error);

    act->teardown();
    CHECK(log == std::vector<std::string>{"setup:1", "cleanup:1"});
}

TEST_CASE("throwing cleanup does not stop remaining teardown") {
    auto root = araya::context::root();
    auto act = std::make_shared<araya::activation>(root);
    araya::plugin_context ctx{act};

    std::vector<std::string> log;
    record(log, "1", ctx);
    ctx.effect([&log]() -> araya::cleanup_action {
        return [&log] {
            log.push_back("cleanup:bad");
            throw std::runtime_error("cleanup failed");
        };
    });
    record(log, "3", ctx);

    act->teardown();
    CHECK(act->error != nullptr);
    CHECK(log == std::vector<std::string>{"setup:1", "setup:3", "cleanup:3",
                                          "cleanup:bad", "cleanup:1"});
}

TEST_CASE("registration token survives teardown and double release") {
    auto root = araya::context::root();
    auto act = std::make_shared<araya::activation>(root);
    araya::plugin_context ctx{act};

    std::vector<std::string> log;
    auto reg = record(log, "1", ctx);
    reg.release();
    reg.release();

    act->teardown();
    reg.release();

    CHECK(log == std::vector<std::string>{"setup:1", "cleanup:1"});
    CHECK(act->error == nullptr);
}

TEST_CASE("null cleanup from setup records nothing") {
    auto root = araya::context::root();
    auto act = std::make_shared<araya::activation>(root);
    araya::plugin_context ctx{act};

    auto reg = ctx.effect([]() -> araya::cleanup_action { return nullptr; });
    CHECK_FALSE(reg);
    CHECK(act->effects->size() == 0);
}

TEST_CASE("activation stop token observes request_stop") {
    auto root = araya::context::root();
    auto act = std::make_shared<araya::activation>(root);
    araya::plugin_context ctx{act};

    auto token = ctx.stop_token();
    CHECK_FALSE(token.stop_requested());
    act->stop_source->request_stop();
    CHECK(token.stop_requested());
}
