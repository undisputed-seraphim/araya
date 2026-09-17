#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/events.hpp"
#include "araya/session/store.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace araya::session;

static std::vector<std::string> g_log;

boost::json::value message(std::string id, std::string role,
                           boost::json::value content) {
    return boost::json::object{{"id", std::move(id)},
                               {"role", std::move(role)},
                               {"content", std::move(content)}};
}

boost::json::value assistant(std::string id, boost::json::value content) {
    return boost::json::object{
        {"message", message(std::move(id), "assistant", std::move(content))}};
}

boost::json::value system(std::string id, std::string plugin,
                          boost::json::value content) {
    boost::json::object m = message(std::move(id), "system",
                                    std::move(content)).as_object();
    m["source"] = boost::json::object{{"kind", "plugin"},
                                      {"plugin", std::move(plugin)}};
    return boost::json::object{{"message", std::move(m)}};
}

boost::json::value tool_result(std::string id, std::string call_id) {
    boost::json::array content;
    content.emplace_back(boost::json::object{
        {"tool_call_id", call_id}, {"type", "tool_result"}});
    return boost::json::object{
        {"id", std::move(id)},
        {"role", "user"},
        {"content", std::move(content)},
        {"source", boost::json::object{{"kind", "tool"},
                                       {"call_id", std::move(call_id)}}}};
}

struct logger_plugin : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        ctx.on(created_key, [](session_created_msg const& m) {
            g_log.push_back("created:" + m.s->id().value);
        });
        ctx.on(appended_key, [](session_appended_msg const& m) {
            g_log.push_back("ev:" + m.event.type);
        });
        ctx.on(disposed_key, [](session_disposed_msg const& m) {
            g_log.push_back("disposed:" + m.id.value);
        });
        ctx.on(flush_key, [](session_flush_msg const& m) -> araya::task<void> {
            g_log.push_back("flush:" + m.id.value);
            co_return;
        });
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_logger(araya::plugin_config const&) {
    return std::make_unique<logger_plugin>();
}

// A consumer fiber that owns one session: unloading it must dispose the
// session it created.
struct owner_plugin : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        auto store = ctx.require<session_store>(sessions_key);
        auto s = store->create(ctx, session_id{"owned"});
        s->append("user/message",
                  message("m1", "user", boost::json::array{}));
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_owner(araya::plugin_config const&) {
    return std::make_unique<owner_plugin>();
}

struct projection_plugin : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        auto store = ctx.require<session_store>(sessions_key);
        g_log.push_back("projection-registered");
        (void)store->register_message_projection(
            ctx, {"custom/counter",
                  [](session_event const& ev)
                      -> std::optional<session_message> {
                      return session_message{
                          message_role::user,
                          "custom-" +
                              std::string(ev.data.at("count").as_string()),
                          ev.data, std::nullopt, std::nullopt};
                  }});
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_projection(
    araya::plugin_config const&) {
    return std::make_unique<projection_plugin>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_sessions_dep[]{
    {araya::service_id{"sessions", 1}, true}};
static const araya::plugin_descriptor g_logger_desc{
    "logger", g_sessions_dep, g_no_provs, &make_logger};
static const araya::plugin_descriptor g_owner_desc{
    "owner", g_sessions_dep, g_no_provs, &make_owner};
static const araya::plugin_descriptor g_projection_desc{
    "projection", g_sessions_dep, g_no_provs, &make_projection};

struct harness {
    boost::asio::io_context io;
    std::shared_ptr<araya::runtime> rt =
        std::make_shared<araya::runtime>(io.get_executor());

    template <typename Fn>
    void run(Fn&& fn) {
        g_log.clear();
        struct driver {
            std::decay_t<Fn> fn;
            harness* self;
            araya::task<void> operator()() { co_await fn(*self->rt); }
        };
        boost::asio::co_spawn(io.get_executor(),
                              driver{std::forward<Fn>(fn), this},
                              boost::asio::detached);
        io.run();
        io.restart();
    }

    araya::component_spec spec(araya::plugin_descriptor const* d,
                                 araya::plugin_config cfg = {}) {
        return araya::component_spec{
            std::shared_ptr<araya::plugin_descriptor>(
                const_cast<araya::plugin_descriptor*>(d),
                [](auto*) {}),
            std::move(cfg), nullptr, ""};
    }

    araya::component_spec session_spec() {
        return spec(&araya::session::plugin_descriptor());
    }

    std::shared_ptr<session_store> store(araya::plugin_context& root_ctx) {
        return root_ctx.require<session_store>(sessions_key).shared();
    }
};

}  // namespace

TEST_CASE("create announces, dispose removes, and the firehose sees every "
          "append") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        co_await rt.mount(h.session_spec());
        co_await rt.mount(h.spec(&g_logger_desc));
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto store = h.store(root_ctx);
        auto s = store->create(root_ctx, session_id{"s1"});
        CHECK(store->size() == 1);
        CHECK(store->get(session_id{"s1"}) == s);

        s->append("user/message",
                  message("m1", "user", boost::json::array{}));
        s->append("assistant/message",
                  assistant("m2", boost::json::array{}));
        co_await s->flush();

        CHECK(store->dispose(session_id{"s1"}));
        CHECK(!store->dispose(session_id{"s1"}));
        CHECK(store->size() == 0);

        co_await rt.wait_idle();

        CHECK(std::find(g_log.begin(), g_log.end(), "created:s1") !=
              g_log.end());
        CHECK(std::find(g_log.begin(), g_log.end(), "ev:user/message") !=
              g_log.end());
        CHECK(std::find(g_log.begin(), g_log.end(), "ev:assistant/message") !=
              g_log.end());
        CHECK(std::find(g_log.begin(), g_log.end(), "flush:s1") !=
              g_log.end());
        CHECK(std::find(g_log.begin(), g_log.end(), "disposed:s1") !=
              g_log.end());
    });
}

TEST_CASE("the surface folds the built-in message types in order") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        co_await rt.mount(h.session_spec());
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto store = h.store(root_ctx);
        auto s = store->create(root_ctx, session_id{"s1"});
        s->append("system/message", system("sys1", "prompts",
                                           boost::json::array{}));
        s->append("user/message",
                  message("u1", "user", boost::json::array{}));
        s->append("assistant/message",
                  assistant("a1", boost::json::array{}));
        s->append("tool/result", tool_result("t1", "tc1"));

        auto const& msgs = s->surface().messages();
        REQUIRE(msgs.size() == 4);
        CHECK(msgs[0].role == message_role::system);
        CHECK(msgs[0].source_plugin == "prompts");
        CHECK(msgs[1].role == message_role::user);
        CHECK(msgs[2].role == message_role::assistant);
        CHECK(msgs[3].role == message_role::tool_result);
        CHECK(msgs[3].tool_call_id == "tc1");
    });
}

TEST_CASE("projections fold their type while their fiber lives") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        co_await rt.mount(h.session_spec());
        co_await rt.mount(h.spec(&g_logger_desc));
        auto proj = co_await rt.mount(h.spec(&g_projection_desc));
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto store = h.store(root_ctx);
        auto s = store->create(root_ctx, session_id{"s1"});
        s->append("custom/counter", {{"count", "1"}});
        REQUIRE(s->surface().messages().size() == 1);
        CHECK(s->surface().messages()[0].id == "custom-1");

        // The projection's fiber unloads: the type becomes ignorable
        // vocabulary again and no longer folds.
        co_await rt.retire(proj);
        co_await rt.wait_idle();
        s->append("custom/counter", {{"count", "2"}});
        CHECK(s->surface().messages().size() == 1);
        CHECK(s->log().back().ignorable);
        CHECK(std::find(g_log.begin(), g_log.end(),
                        "ev:custom/counter") != g_log.end());
    });
}

TEST_CASE("seeded forks carry lineage, an end-seed marker, and a live "
          "boundary") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        co_await rt.mount(h.session_spec());
        co_await rt.mount(h.spec(&g_logger_desc));
        co_await rt.wait_idle();

        std::vector<session_event> seed;
        seed.push_back(session_event{
            0, 1000, "user/message",
            message("m1", "user", boost::json::array{}), false});
        seed.push_back(session_event{
            1, 1001, "assistant/message",
            assistant("m2", boost::json::array{}), false});

        create_session_options options;
        options.seed = std::move(seed);
        options.is_seeded = true;
        options.inherited_event_count = 2;
        options.parent_session = session_id{"parent-1"};

        auto root_ctx = rt.root_context();
        auto store = h.store(root_ctx);
        auto s = store->create(root_ctx, session_id{"s1"},
                               std::move(options));

        CHECK(s->header().is_seeded);
        CHECK(s->header().parent_session == session_id{"parent-1"});
        CHECK(s->inherited_event_count() == 2);
        CHECK(s->first_live_seq() == 2);
        REQUIRE(s->log().size() == 3);
        CHECK(s->log().back().type == "session/end-seed");
        CHECK(s->log().back().data.at("inherited").as_uint64() == 2);
        CHECK(s->surface().messages().size() == 2);

        // Appends after construction publish on the firehose.
        s->append("user/message",
                  message("m3", "user", boost::json::array{}));
        co_await rt.wait_idle();
        CHECK(std::find(g_log.begin(), g_log.end(), "ev:user/message") !=
              g_log.end());
    });
}

TEST_CASE("reconstruction closes interrupted tool calls") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        co_await rt.mount(h.session_spec());
        co_await rt.wait_idle();

        boost::json::array content;
        content.emplace_back(boost::json::object{
            {"type", "tool_call"},
            {"tool_call", boost::json::object{{"id", "tc1"}}}});
        std::vector<session_event> seed;
        seed.push_back(session_event{
            0, 1000, "assistant/message",
            assistant("m1", std::move(content)), false});

        create_session_options options;
        options.seed = std::move(seed);

        auto root_ctx = rt.root_context();
        auto store = h.store(root_ctx);
        auto s = store->create(root_ctx, session_id{"s1"},
                               std::move(options));

        // The interrupted call closed with an unknown-outcome tool result.
        auto const& msgs = s->surface().messages();
        REQUIRE(msgs.size() == 2);
        CHECK(msgs[1].role == message_role::tool_result);
        CHECK(msgs[1].tool_call_id == "tc1");
        CHECK(msgs[1].content.at(0).at("outcome") == "unknown");
    });
}

TEST_CASE("unknown vocabulary is preserved and ignorable") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        co_await rt.mount(h.session_spec());
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto store = h.store(root_ctx);
        auto s = store->create(root_ctx, session_id{"s1"});
        s->append("future/type", {{"opaque", true}});
        CHECK(s->surface().messages().empty());
        REQUIRE(s->log().size() == 1);
        CHECK(s->log().back().ignorable);
        CHECK(s->log().back().data.at("opaque").as_bool());
    });
}

TEST_CASE("unloading the owning fiber disposes its session") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        co_await rt.mount(h.session_spec());
        co_await rt.mount(h.spec(&g_logger_desc));
        auto owner = co_await rt.mount(h.spec(&g_owner_desc));
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto store = h.store(root_ctx);
        CHECK(store->size() == 1);
        CHECK(std::find(g_log.begin(), g_log.end(), "created:owned") !=
              g_log.end());

        co_await rt.retire(owner);
        co_await rt.wait_idle();

        CHECK(store->size() == 0);
        CHECK(std::find(g_log.begin(), g_log.end(), "disposed:owned") !=
              g_log.end());
    });
}

TEST_CASE("built-in message shapes are validated on append") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        co_await rt.mount(h.session_spec());
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto store = h.store(root_ctx);
        auto s = store->create(root_ctx, session_id{"s1"});

        // assistant/message must wrap the message under "message".
        CHECK_THROWS(s->append("assistant/message",
                               message("m1", "assistant",
                                       boost::json::array{})));
        // tool/result content must carry the matching tool_call_id.
        boost::json::array content;
        content.emplace_back(boost::json::object{
            {"tool_call_id", "tc1"}, {"type", "tool_result"}});
        CHECK_THROWS(
            s->append("tool/result",
                      boost::json::object{{"id", "t1"},
                                          {"role", "user"},
                                          {"content", std::move(content)},
                                          {"source",
                                           boost::json::object{
                                               {"kind", "tool"},
                                               {"call_id", "tc2"}}}}));

        // Nothing invalid entered the log.
        CHECK(s->log().empty());
    });
}
