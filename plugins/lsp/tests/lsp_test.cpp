#include <catch2/catch_test_macros.hpp>

#include "araya/lsp/lsp.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include "support/plugin_harness.hpp"

#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace araya::lsp;

// A provider that answers with one fixed location and records the query it saw.
struct fake_provider : lsp_provider {
	std::string name = "fake";
	std::map<std::string, std::string> mapping{{".ts", "typescript"}};
	std::string last_language;
	lsp_query_request last_request;
	bool queried = false;

	std::string_view id() const override { return name; }
	std::map<std::string, std::string> const& extension_to_language() const override { return mapping; }

	araya::task<lsp_query_result> query(lsp_provider_query const& request, std::stop_token) override {
		queried = true;
		last_language = request.language_id;
		last_request = request.request;
		lsp_query_result result;
		result.type = lsp_query_result::kind::locations;
		result.resolved_workspace_uri = "file:///ws";
		result.locations.push_back(lsp_location{"file:///ws/a.ts", lsp_range{{1, 2}, {1, 5}}});
		co_return result;
	}
};

// Test wiring shared with the registrar plugin through a module-global pointer.
struct outcome {
	std::shared_ptr<lsp_service> service;
	std::string provider_id = "fake";
	std::map<std::string, std::string> mapping{{".ts", "typescript"}};
	std::shared_ptr<fake_provider> provider;
	araya::registration registration;
	std::string error;
	bool registered = false;
};

outcome* g_outcome = nullptr;

struct registrar : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto service = ctx.require<lsp_service>(lsp_key).shared();
		g_outcome->service = service;
		g_outcome->provider = std::make_shared<fake_provider>();
		g_outcome->provider->name = g_outcome->provider_id;
		g_outcome->provider->mapping = g_outcome->mapping;
		try {
			g_outcome->registration = service->register_provider(ctx, g_outcome->provider);
			g_outcome->registered = true;
		} catch (lsp_error const& error) {
			g_outcome->error = error.code();
		}
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_registrar(araya::plugin_config const&) { return std::make_unique<registrar>(); }

static const araya::dependency_spec reg_deps[]{{araya::service_id{"lsp", 1}, true, {}}};
static constexpr std::span<araya::provision_spec const> reg_provs{};
static const araya::plugin_descriptor reg_descriptor{"test-registrar", reg_deps, reg_provs, &make_registrar, {}};

struct harness : araya_test::plugin_harness {
	araya::component_spec lsp_spec() { return spec(&araya::lsp::plugin_descriptor()); }
	araya::component_spec registrar_spec() { return spec(&reg_descriptor); }
};

} // namespace

TEST_CASE("lsp final_extension normalizes paths and dotfiles") {
	CHECK(final_extension("Foo.TS") == ".ts");
	CHECK(final_extension("dir/foo.d.ts") == ".ts");
	CHECK(final_extension("dir\\foo.JS") == ".js");
	CHECK(final_extension("Makefile") == "");
	CHECK(final_extension(".bashrc") == "");
}

TEST_CASE("lsp selects a provider by extension and dispatches the query") {
	harness h;
	outcome o;
	g_outcome = &o;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.lsp_spec());
		co_await rt.mount(h.registrar_spec());
		co_await rt.wait_idle();
		REQUIRE(o.registered);

		lsp_query_request request;
		request.operation = lsp_operation::go_to_definition;
		request.file_path = "src/a.ts";
		request.position = {1, 2};
		request.workspace_root = "/ws";
		auto const result = co_await o.service->query(request, {});
		CHECK(result.type == lsp_query_result::kind::locations);
		CHECK(result.resolved_workspace_uri == "file:///ws");
		CHECK(o.provider->queried);
		CHECK(o.provider->last_language == "typescript");
		CHECK(o.provider->last_request.file_path == "src/a.ts");
	});
	g_outcome = nullptr;
}

TEST_CASE("lsp reports an unavailable provider for an unmatched extension") {
	harness h;
	outcome o;
	g_outcome = &o;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.lsp_spec());
		co_await rt.mount(h.registrar_spec());
		co_await rt.wait_idle();

		lsp_query_request request;
		request.file_path = "notes.md";
		bool threw = false;
		try {
			(void)co_await o.service->query(request, {});
		} catch (lsp_error const& error) {
			threw = true;
			CHECK(error.code() == error_code::unavailable);
		}
		CHECK(threw);
	});
	g_outcome = nullptr;
}

TEST_CASE("lsp rejects conflicting and invalid provider registrations") {
	harness h;
	outcome first;
	outcome conflict;
	outcome invalid;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.lsp_spec());

		g_outcome = &first;
		co_await rt.mount(h.registrar_spec());
		co_await rt.wait_idle();
		REQUIRE(first.registered);

		g_outcome = &conflict;
		co_await rt.mount(h.registrar_spec());
		co_await rt.wait_idle();
		CHECK(conflict.error == error_code::conflict);

		g_outcome = &invalid;
		invalid.provider_id = "  ";
		co_await rt.mount(h.registrar_spec());
		co_await rt.wait_idle();
		CHECK(invalid.error == error_code::invalid_provider);
	});
	g_outcome = nullptr;
}

TEST_CASE("lsp releases a route when the registration is disposed") {
	harness h;
	outcome o;
	g_outcome = &o;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.lsp_spec());
		co_await rt.mount(h.registrar_spec());
		co_await rt.wait_idle();
		REQUIRE(o.registered);

		o.registration.release();
		lsp_query_request request;
		request.file_path = "a.ts";
		bool threw = false;
		try {
			(void)co_await o.service->query(request, {});
		} catch (lsp_error const& error) {
			threw = true;
			CHECK(error.code() == error_code::unavailable);
		}
		CHECK(threw);
	});
	g_outcome = nullptr;
}
