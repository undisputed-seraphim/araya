#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session-log-export/export.hpp"
#include "araya/session-query/query.hpp"
#include "araya/session/store.hpp"

#include "support/plugin_harness.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using araya::session_export::session_export_key;
using araya::session_export::session_export_service;

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec query_spec() { return spec(&araya::session_query::plugin_descriptor()); }
	araya::component_spec export_spec() { return spec(&araya::session_export::plugin_descriptor()); }
};

struct rig {
	std::shared_ptr<araya::session::session_store> store;
	std::shared_ptr<session_export_service> service;
	std::optional<araya::plugin_context> owner;

	araya::task<void> mount(araya::runtime& rt, harness& h) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.query_spec());
		co_await rt.mount(h.export_spec());
		co_await rt.wait_idle();
		owner.emplace(rt.root_context());
		store = owner->require<araya::session::session_store>(araya::session::sessions_key).shared();
		service = owner->require<session_export_service>(session_export_key).shared();
	}

	araya::session::session&
	create(std::string const& id, std::optional<araya::session::session_id> parent = std::nullopt) {
		araya::session::create_session_options options;
		options.parent_session = std::move(parent);
		return *store->create(*owner, araya::session::session_id{id}, std::move(options));
	}
};

void append_user(araya::session::session& session, std::string id, std::string text) {
	session.append(
		"user/message",
		boost::json::value{
			{"id", std::move(id)},
			{"role", "user"},
			{"content", boost::json::array{boost::json::object{{"type", "text"}, {"text", std::move(text)}}}}});
}

TEST_CASE("export builds a ZIP with the log and descendant subagents") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		append_user(r.create("ses_a"), "u1", "root message");
		append_user(r.create("ses_b", araya::session::session_id{"ses_a"}), "u2", "child message");

		auto const zip = r.service->build_zip(araya::session::session_id{"ses_a"}, true);
		REQUIRE(zip.size() > 4);
		CHECK(zip.compare(0, 4, "PK\x03\x04") == 0);
		CHECK(zip.find("session.v1.jsonl") != std::string::npos);
		CHECK(zip.find("subagents/ses_b/session.v1.jsonl") != std::string::npos);

		// Without descendants, only the root log is present.
		auto const root_only = r.service->build_zip(araya::session::session_id{"ses_a"}, false);
		CHECK(root_only.find("subagents/") == std::string::npos);
	});
}

TEST_CASE("export rejects an unknown session and names the download") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		CHECK_THROWS_AS(r.service->build_zip(araya::session::session_id{"missing"}, true), std::runtime_error);
		CHECK(
			araya::session_export::session_export_filename(araya::session::session_id{"ses a/b"}) ==
			"araya-session-ses_a_b.zip");
	});
}

} // namespace
