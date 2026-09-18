#include <catch2/catch_test_macros.hpp>

#include "araya/abi.hpp"
#include "araya/abi_host.hpp"
#include "araya/events.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// A C-style service interface crossing the boundary.
struct db_service {
	const char* (*name)(void* self);
};

struct db_value {
	db_service iface;
	std::string name;
};

static const char* db_value_name(void* self) { return static_cast<db_value*>(self)->name.c_str(); }

inline constexpr araya::service_id db_id{"example.db", 1};

inline constexpr araya::event_key<std::string, araya::dispatch_mode::serial> notify_key{"example.notify", 1};

static std::vector<std::string> g_log;
static const araya_host_api_v1* g_host = nullptr;
static const araya_host_api_v1* g_slow_host = nullptr;

// Module instances use the container-of pattern: araya_instance_v1 must be
// the first member; self pointers are reinterpreted back.
struct mod_instance {
	araya_instance_v1 base;
	std::string tag;
};

static void mod_apply(araya_instance_v1* self, araya_fiber fiber, void (*complete)(araya_fiber, int, const char*)) {
	auto* inst = reinterpret_cast<mod_instance*>(self);
	auto const* api = g_host;

	araya_handle reg;
	auto* db = new db_value{{&db_value_name}, inst->tag};
	if (api->provide(
			fiber, "example.db", 1, db, +[](void* p) { delete static_cast<db_value*>(p); }, &reg) != ARAYA_ABI_OK) {
		complete(fiber, ARAYA_ABI_ERROR, "provide failed");
		return;
	}
	g_log.push_back("provide:" + inst->tag);

	void* dep = nullptr;
	if (api->find(fiber, "example.aux", 1, &dep) == ARAYA_ABI_OK && dep)
		g_log.push_back("aux-found");

	if (api->effect(
			fiber,
			+[](void* data, araya_cleanup_v1* c) {
				c->run = +[](void* data) { g_log.push_back("cleanup:" + *static_cast<std::string*>(data)); };
				c->data = data;
			},
			&inst->tag,
			nullptr) != ARAYA_ABI_OK) {
		complete(fiber, ARAYA_ABI_ERROR, "effect failed");
		return;
	}

	if (api->on(
			fiber,
			"example.notify",
			1,
			ARAYA_MODE_SERIAL,
			+[](void* data, const void* msg) {
				g_log.push_back(
					"notify:" + *static_cast<std::string const*>(msg) + ":" + *static_cast<std::string*>(data));
			},
			&inst->tag,
			nullptr) != ARAYA_ABI_OK) {
		complete(fiber, ARAYA_ABI_ERROR, "on failed");
		return;
	}

	complete(fiber, ARAYA_ABI_OK, nullptr);
}

static araya_instance_v1* make_mod(const araya_config_pair* pairs, std::size_t count) {
	auto* inst = new mod_instance{};
	inst->base.apply = &mod_apply;
	inst->base.reconfigure = nullptr;
	inst->base.destroy = +[](araya_instance_v1* self) { delete reinterpret_cast<mod_instance*>(self); };
	for (std::size_t i = 0; i < count; ++i) {
		if (std::strcmp(pairs[i].key, "tag") == 0)
			inst->tag = pairs[i].value;
	}
	return &inst->base;
}

struct slow_completion {
	araya_fiber fiber;
	void (*complete)(araya_fiber, int, const char*);
};

static void slow_tick(void* data) {
	auto* c = static_cast<slow_completion*>(data);
	auto const* api = g_slow_host;
	if (api->stop_requested(c->fiber)) {
		c->complete(c->fiber, ARAYA_ABI_OK, nullptr);
		delete c;
		return;
	}
	g_log.push_back("slow-tick");
	api->post(c->fiber, &slow_tick, c);
}

static void slow_apply(araya_instance_v1*, araya_fiber fiber, void (*complete)(araya_fiber, int, const char*)) {
	auto* c = new slow_completion{fiber, complete};
	g_slow_host->post(fiber, &slow_tick, c);
}

struct slow_instance {
	araya_instance_v1 base;
};

static araya_instance_v1* make_slow(const araya_config_pair*, std::size_t) {
	auto* inst = new slow_instance{};
	inst->base.apply = &slow_apply;
	inst->base.reconfigure = nullptr;
	inst->base.destroy = +[](araya_instance_v1* self) { delete reinterpret_cast<slow_instance*>(self); };
	return &inst->base;
}

struct failing_instance {
	araya_instance_v1 base;
};

static void failing_apply(araya_instance_v1*, araya_fiber fiber, void (*complete)(araya_fiber, int, const char*)) {
	complete(fiber, ARAYA_ABI_ERROR, "module failed on purpose");
}

static araya_instance_v1* make_failing(const araya_config_pair*, std::size_t) {
	auto* inst = new failing_instance{};
	inst->base.apply = &failing_apply;
	inst->base.reconfigure = nullptr;
	inst->base.destroy = +[](araya_instance_v1* self) { delete reinterpret_cast<failing_instance*>(self); };
	return &inst->base;
}

static const araya_dependency_v1 g_inject[]{
	{"example.aux", 1, 0}, // optional
};
static const araya_provision_v1 g_provide[]{{"example.db", 1}};

static araya_plugin_descriptor_v1 g_mod_desc{ARAYA_ABI_VERSION, "module", g_inject, 1, g_provide, 1, &make_mod};
static araya_plugin_descriptor_v1 g_slow_desc{ARAYA_ABI_VERSION, "slow-module", nullptr, 0, nullptr, 0, &make_slow};
static araya_plugin_descriptor_v1
	g_failing_desc{ARAYA_ABI_VERSION, "failing-module", nullptr, 0, nullptr, 0, &make_failing};

const araya_plugin_descriptor_v1* make_failing_entry(const araya_host_api_v1*) { return &g_failing_desc; }

const araya_plugin_descriptor_v1* make_slow_entry(const araya_host_api_v1* host) {
	g_slow_host = host;
	return &g_slow_desc;
}

struct harness {
	boost::asio::io_context io;
	std::shared_ptr<araya::runtime> rt = std::make_shared<araya::runtime>(io.get_executor());

	template <typename Fn>
	void run(Fn&& fn) {
		g_log.clear();
		struct driver {
			std::decay_t<Fn> fn;
			harness* self;
			araya::task<void> operator()() { co_await fn(*self->rt); }
		};
		boost::asio::co_spawn(io.get_executor(), driver{std::forward<Fn>(fn), this}, boost::asio::detached);
		io.run();
		io.restart();
	}
};

} // namespace

extern "C" const araya_plugin_descriptor_v1* araya_plugin_entry_v1(const araya_host_api_v1* host) {
	g_host = host;
	return &g_mod_desc;
}

namespace {

TEST_CASE("module provides services, effects, and listeners over the ABI") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		auto desc = araya::abi::wrap(&araya_plugin_entry_v1);
		// declare the event host-side so the module's raw listener attaches
		co_await rt.run_on_strand([&] { rt.root_context().on(notify_key, [&](std::string const&) {}); });

		auto fh = co_await rt.mount(araya::component_spec{desc, {{"tag", "m1"}}, nullptr, ""});
		co_await rt.wait_idle();
		CHECK(rt.state_of(fh.id()) == araya::fiber_state::active);

		auto root = rt.root_context();
		auto b = root.find_binding(db_id);
		REQUIRE(b);
		auto* db = static_cast<db_service*>(b->value.get());
		CHECK(std::string(db->name(db)) == "m1");

		co_await rt.bus()->dispatch(notify_key, std::string("hello"));

		co_await rt.retire(fh);
		co_await rt.wait_idle();
	});
	CHECK(g_log == std::vector<std::string>{"provide:m1", "notify:hello:m1", "cleanup:m1"});
}

TEST_CASE("module failure fails the activation") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		auto desc = araya::abi::wrap(&make_failing_entry);
		auto fh = co_await rt.mount(araya::component_spec{desc, {}, nullptr, ""});
		co_await rt.wait_idle();
		CHECK(rt.state_of(fh.id()) == araya::fiber_state::inactive);
		CHECK(rt.error_of(fh.id()) != nullptr);
	});
}

TEST_CASE("module observes cooperative stop requests") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		auto desc = araya::abi::wrap(&make_slow_entry);
		auto fh = co_await rt.mount(araya::component_spec{desc, {}, nullptr, ""});

		co_await rt.retire(fh);
		co_await rt.wait_idle();
		CHECK(rt.state_of(fh.id()) == araya::fiber_state::inactive);
	});
	CHECK(std::find(g_log.begin(), g_log.end(), "slow-tick") != g_log.end());
}

TEST_CASE("module events require a host-declared typed event") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		auto desc = araya::abi::wrap(&araya_plugin_entry_v1);
		// no typed listener registered for notify_key: the module's raw
		// listener registration fails, so the module fails its activation.
		auto fh = co_await rt.mount(araya::component_spec{desc, {{"tag", "m2"}}, nullptr, ""});
		co_await rt.wait_idle();
		CHECK(rt.state_of(fh.id()) == araya::fiber_state::inactive);
		CHECK(rt.error_of(fh.id()) != nullptr);
	});
}

TEST_CASE("module without reconfigure is replaced on config change") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		auto desc = araya::abi::wrap(&araya_plugin_entry_v1);
		co_await rt.run_on_strand([&] { rt.root_context().on(notify_key, [&](std::string const&) {}); });

		std::vector<araya::desired_component> desired;
		desired.push_back(araya::desired_component{"mod", araya::component_spec{desc, {{"tag", "r1"}}, nullptr, ""}});
		co_await rt.reconcile(std::move(desired));
		co_await rt.wait_idle();

		std::vector<araya::desired_component> updated;
		updated.push_back(araya::desired_component{"mod", araya::component_spec{desc, {{"tag", "r2"}}, nullptr, ""}});
		co_await rt.reconcile(std::move(updated));
		co_await rt.wait_idle();

		auto root = rt.root_context();
		auto b = root.find_binding(db_id);
		REQUIRE(b);
		auto* db = static_cast<db_service*>(b->value.get());
		CHECK(std::string(db->name(db)) == "r2");
	});
}

} // namespace
