#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/events.hpp"
#include "araya/llm-retry/llm_retry.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include "support/plugin_harness.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <utility>

namespace {

using namespace araya::agent;
using namespace araya::llm;

// A route adapter that declares a configurable retry policy; its stream is
// never exercised here (the waterfall is dispatched directly).
struct flaky_adapter : llm_adapter {
	std::optional<retry_policy> policy;

	araya::task<void> stream(generate_options const&, chunk_sink const&) override { co_return; }

	std::optional<retry_policy> provider_retry_policy(std::string_view) override { return policy; }
};

std::shared_ptr<flaky_adapter> g_adapter;

struct registrar : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto llm = ctx.require<llm_service>(llm_key).shared();
		llm->register_adapter({"flaky"}, g_adapter, ctx);
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_registrar(araya::plugin_config const&) { return std::make_unique<registrar>(); }

static const araya::dependency_spec reg_deps[]{{araya::service_id{"llm", 1}, true, {}}};
static constexpr std::span<araya::provision_spec const> reg_provs{};
static const araya::plugin_descriptor reg_descriptor{"test-flaky", reg_deps, reg_provs, &make_registrar, {}};

struct harness : araya_test::plugin_harness {
	araya::component_spec llm_spec() { return spec(&araya::llm::plugin_descriptor()); }
	araya::component_spec registrar_spec() { return spec(&reg_descriptor); }
	araya::component_spec retry_spec() { return spec(&araya::llm_retry::plugin_descriptor()); }
};

request_error_msg make_message(llm_error_code code, std::uint32_t attempt, std::stop_token stop = {}) {
	request_error_msg message;
	message.provider = "flaky";
	message.failure = llm_failure{code, "boom"};
	message.attempt = attempt;
	message.stop = stop;
	return message;
}

retry_policy fast_policy(retry_mode mode, std::uint32_t max_retries) {
	retry_policy policy;
	policy.mode = mode;
	policy.max_retries = max_retries;
	policy.initial_delay = std::chrono::milliseconds(1);
	policy.max_delay = std::chrono::milliseconds(4);
	policy.jitter_ratio = 0.0;
	return policy;
}

} // namespace

TEST_CASE("llm-retry retries eligible transient failures within budget") {
	harness h;
	g_adapter = std::make_shared<flaky_adapter>();
	g_adapter->policy = fast_policy(retry_mode::normal, 2);
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.registrar_spec());
		co_await rt.mount(h.retry_spec());
		co_await rt.wait_idle();
		auto const bus = rt.bus();

		auto const first = co_await bus->dispatch(request_error_key, make_message(llm_error_code::server, 0), nullptr);
		CHECK(first.retry);
		CHECK(first.delay.count() > 0);

		auto const exhausted =
			co_await bus->dispatch(request_error_key, make_message(llm_error_code::server, 2), nullptr);
		CHECK_FALSE(exhausted.retry);

		auto const ineligible =
			co_await bus->dispatch(request_error_key, make_message(llm_error_code::auth, 0), nullptr);
		CHECK_FALSE(ineligible.retry);
	});
	g_adapter.reset();
}

TEST_CASE("llm-retry always mode retries every failure") {
	harness h;
	g_adapter = std::make_shared<flaky_adapter>();
	g_adapter->policy = fast_policy(retry_mode::always, 0);
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.registrar_spec());
		co_await rt.mount(h.retry_spec());
		co_await rt.wait_idle();
		auto const bus = rt.bus();

		auto const far = co_await bus->dispatch(request_error_key, make_message(llm_error_code::server, 99), nullptr);
		CHECK(far.retry);
		auto const auth = co_await bus->dispatch(request_error_key, make_message(llm_error_code::auth, 5), nullptr);
		CHECK(auth.retry);
	});
	g_adapter.reset();
}

TEST_CASE("llm-retry delegates when the stop token is requested") {
	harness h;
	g_adapter = std::make_shared<flaky_adapter>();
	g_adapter->policy = fast_policy(retry_mode::always, 0);
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.registrar_spec());
		co_await rt.mount(h.retry_spec());
		co_await rt.wait_idle();
		auto const bus = rt.bus();

		std::stop_source source;
		source.request_stop();
		auto const stopped = co_await bus->dispatch(
			request_error_key, make_message(llm_error_code::server, 0, source.get_token()), nullptr);
		CHECK_FALSE(stopped.retry);
	});
	g_adapter.reset();
}
