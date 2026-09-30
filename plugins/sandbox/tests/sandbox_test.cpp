#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/sandbox/sandbox.hpp"
#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <memory>
#include <string>

namespace {

using namespace araya::sandbox;

struct harness : araya_test::plugin_harness {
	araya::component_spec sandbox_spec() { return spec(&araya::sandbox::plugin_descriptor()); }
};

bool contains(std::vector<std::string> const& values, std::string const& value) {
	return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

TEST_CASE("sandbox mode names round-trip") {
	CHECK(mode_name(sandbox_mode::read_only) == "read-only");
	CHECK(mode_name(sandbox_mode::workspace_write) == "workspace-write");
	CHECK(mode_name(sandbox_mode::danger_full_access) == "danger-full-access");
	CHECK(parse_mode("read-only") == sandbox_mode::read_only);
	CHECK(parse_mode("workspace-write") == sandbox_mode::workspace_write);
	CHECK(parse_mode("danger-full-access") == sandbox_mode::danger_full_access);
	CHECK_FALSE(parse_mode("nope").has_value());
}

TEST_CASE("denials are matched case-insensitively on a failed run") {
	CHECK(matches_denial(1, "bash: /home/x: Permission denied", "permission denied"));
	CHECK_FALSE(matches_denial(0, "Permission denied", "permission denied"));
	CHECK_FALSE(matches_denial(1, "command not found", "permission denied"));
	CHECK(denial_marker("workspace-write") == "[sandbox: file access denied under workspace-write mode]");
}

TEST_CASE("the provider builds the expected confinement program per mode") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.sandbox_spec());
		co_await rt.wait_idle();
		auto provider = rt.root_context().require<sandbox_provider>(sandbox_key).shared();
		REQUIRE(provider != nullptr);

		auto const full = provider->confine(policy{.mode = sandbox_mode::danger_full_access, .workspace_root = "/w"});
		CHECK_FALSE(full.active);

		if (!provider->available()) {
			SUCCEED("landlock is unavailable");
			co_return;
		}

		auto const read_only = provider->confine(policy{.mode = sandbox_mode::read_only, .workspace_root = "/w"});
		CHECK(read_only.active);
		CHECK(read_only.mode == "read-only");
		CHECK(contains(read_only.init.read_only, "/"));
		CHECK(contains(read_only.init.read_write, "/dev/null"));
		CHECK_FALSE(contains(read_only.init.read_write, "/w"));

		auto const workspace = provider->confine(policy{.mode = sandbox_mode::workspace_write, .workspace_root = "/w"});
		CHECK(workspace.active);
		CHECK(workspace.mode == "workspace-write");
		CHECK(contains(workspace.init.read_write, "/tmp"));
		CHECK(contains(workspace.init.read_write, "/w"));
	});
}
