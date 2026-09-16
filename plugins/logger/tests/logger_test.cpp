#include <catch2/catch_test_macros.hpp>

#include "medulla/logger/logger.hpp"
#include "medulla/runtime.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <unistd.h>

namespace {

using namespace medulla::logger;

struct file_logger_guard {
    quill::Logger* logger;
    std::filesystem::path path;

    ~file_logger_guard() {
        logger->flush_log();
        quill::Frontend::remove_logger_blocking(logger);
        std::filesystem::remove(path);
    }
};

// Pre-creates a quill logger with a file sink: the logger service adopts
// it by name, so records land in the file (quill's own backend thread does
// the write).
file_logger_guard make_file_logger(std::string name) {
    auto path = std::filesystem::temp_directory_path() /
                ("medulla-logger-test-" + name + "-" +
                 std::to_string(::getpid()) + ".log");
    std::filesystem::remove(path);
    auto* logger = quill::Frontend::create_or_get_logger(
        name, std::make_shared<quill::FileSink>(path));
    return {logger, std::move(path)};
}

std::string read_file(std::filesystem::path const& path) {
    std::ifstream in{path};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct harness {
    boost::asio::io_context io;
    std::shared_ptr<medulla::runtime> rt =
        std::make_shared<medulla::runtime>(io.get_executor());

    template <typename Fn>
    void run(Fn&& fn) {
        struct driver {
            std::decay_t<Fn> fn;
            harness* self;
            medulla::task<void> operator()() { co_await fn(*self->rt); }
        };
        boost::asio::co_spawn(io.get_executor(),
                              driver{std::forward<Fn>(fn), this},
                              boost::asio::detached);
        io.run();
        io.restart();
    }

    medulla::component_spec spec(medulla::plugin_descriptor const* d,
                                 medulla::plugin_config cfg = {}) {
        return medulla::component_spec{
            std::shared_ptr<medulla::plugin_descriptor>(
                const_cast<medulla::plugin_descriptor*>(d),
                [](auto*) {}),
            std::move(cfg), nullptr, ""};
    }

    medulla::component_spec logger_spec(medulla::plugin_config cfg = {}) {
        return spec(&medulla::logger::plugin_descriptor(), std::move(cfg));
    }
};

}  // namespace

TEST_CASE("records reach the quill backend through named loggers") {
    auto file = make_file_logger("reach-backend");
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.mount(h.logger_spec());
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto service =
            root_ctx.require<logger_service>(logger_key).shared();
        service->named("reach-backend").info("hello {}", 42);
        service->named("reach-backend").warn("careful {}", "now");
    });

    file.logger->flush_log();
    file.logger->flush_log();
    auto content = read_file(file.path);
    CHECK(content.find("hello 42") != std::string::npos);
    CHECK(content.find("careful now") != std::string::npos);
}

TEST_CASE("level gating filters below-threshold records") {
    auto file = make_file_logger("gated");
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.mount(h.logger_spec());
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto service =
            root_ctx.require<logger_service>(logger_key).shared();
        service->named("gated").info("before the change");
        service->set_level(log_level::error);
        service->named("gated").info("still dropped");
        service->named("gated").error("kept {}", 1);
    });

    file.logger->flush_log();
    auto content = read_file(file.path);
    CHECK(content.find("before the change") != std::string::npos);
    CHECK(content.find("still dropped") == std::string::npos);
    CHECK(content.find("kept 1") != std::string::npos);
}

TEST_CASE("bad format strings degrade instead of throwing") {
    auto file = make_file_logger("bad-format");
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.mount(h.logger_spec());
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto service =
            root_ctx.require<logger_service>(logger_key).shared();
        CHECK_NOTHROW(
            service->named("bad-format").info("no args {}"));
    });

    file.logger->flush_log();
    auto content = read_file(file.path);
    CHECK(content.find("no args {} <format error>") !=
          std::string::npos);
}

TEST_CASE("the default logger name comes from config") {
    auto file = make_file_logger("custom-root");
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.mount(h.logger_spec({{"name", "custom-root"}}));
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto service =
            root_ctx.require<logger_service>(logger_key).shared();
        service->root().info("rooted");
    });

    file.logger->flush_log();
    auto content = read_file(file.path);
    CHECK(content.find("rooted") != std::string::npos);
}
