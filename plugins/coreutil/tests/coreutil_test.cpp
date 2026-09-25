#include <catch2/catch_test_macros.hpp>

#include "araya/coreutil/coreutil.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using namespace araya::tools;

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec coreutil_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::coreutil::plugin_descriptor(), std::move(cfg));
	}
};

// A scratch directory that removes itself.
struct scratch {
	fs::path dir;

	scratch() {
		dir = fs::temp_directory_path() / ("araya-coreutil-" + std::to_string(::getpid()) + "-" +
										   std::to_string(reinterpret_cast<std::uintptr_t>(this)));
		fs::remove_all(dir);
		fs::create_directories(dir);
	}
	~scratch() {
		std::error_code ec;
		fs::remove_all(dir, ec);
	}
};

struct rig {
	std::shared_ptr<araya::session::session_store> store;
	std::shared_ptr<tools_service> tools;
	std::string session = "s1";

	araya::task<void> mount(araya::runtime& rt, harness& h, fs::path const& cwd) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.coreutil_spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		tools = root.require<tools_service>(tools_key).shared();
		araya::session::create_session_options options;
		options.cwd = cwd.string();
		store->create(root, araya::session::session_id{session}, options);
	}

	araya::task<std::optional<tool_result>> call(std::string name, boost::json::object args) {
		std::string const tool = name;
		co_return co_await tools->invoke(
			tool,
			tool_context{
				.call_id = "c",
				.name = std::move(name),
				.session = session,
				.arguments = std::move(args),
			});
	}
};

std::string text_of(tool_result const& result) {
	auto const* arr = result.content.if_array();
	if (!arr || arr->empty())
		return {};
	auto const* object = arr->front().if_object();
	if (!object)
		return {};
	auto it = object->find("text");
	return it != object->end() && it->value().is_string() ? std::string(it->value().as_string()) : std::string{};
}

} // namespace

TEST_CASE("write then read round-trips with line numbers") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		rig r;
		co_await r.mount(rt, h, s.dir);

		auto wrote = co_await r.call("write", {{"file_path", "note.txt"}, {"content", "alpha\nbeta\ngamma\n"}});
		REQUIRE(wrote.has_value());
		CHECK_FALSE(wrote->is_error);
		CHECK(text_of(*wrote).find("Created file") != std::string::npos);

		auto read = co_await r.call("read", {{"file_path", "note.txt"}});
		REQUIRE(read.has_value());
		CHECK_FALSE(read->is_error);
		CHECK(text_of(*read).find("1: alpha") != std::string::npos);
		CHECK(text_of(*read).find("3: gamma") != std::string::npos);
		CHECK(text_of(*read).find("(End of file - total 3 lines)") != std::string::npos);

		// Rewriting an existing file reports Updated.
		auto rewrote = co_await r.call("write", {{"file_path", "note.txt"}, {"content", "x\n"}});
		REQUIRE(rewrote.has_value());
		CHECK(text_of(*rewrote).find("Updated file") != std::string::npos);
	});
}

TEST_CASE("read honours offset and limit") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		rig r;
		co_await r.mount(rt, h, s.dir);
		co_await r.call("write", {{"file_path", "n.txt"}, {"content", "1\n2\n3\n4\n"}});

		auto read = co_await r.call("read", {{"file_path", "n.txt"}, {"offset", 2}, {"limit", 2}});
		REQUIRE(read.has_value());
		auto const text = text_of(*read);
		CHECK(text.find("2: 2") != std::string::npos);
		CHECK(text.find("3: 3") != std::string::npos);
		CHECK(text.find("1: 1") == std::string::npos);
		CHECK(text.find("(Showing lines 2-3 of 4. Use offset=4 to continue.)") != std::string::npos);
	});
}

TEST_CASE("read reports not-found as a tool error") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		rig r;
		co_await r.mount(rt, h, s.dir);
		auto read = co_await r.call("read", {{"file_path", "missing.txt"}});
		REQUIRE(read.has_value());
		CHECK(read->is_error);
		CHECK(text_of(*read).find("file not found") != std::string::npos);
	});
}

TEST_CASE("edit enforces unique match and supports replace_all") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		rig r;
		co_await r.mount(rt, h, s.dir);
		co_await r.call("write", {{"file_path", "e.txt"}, {"content", "cat cat cat\n"}});

		auto ambiguous =
			co_await r.call("edit", {{"file_path", "e.txt"}, {"old_string", "cat"}, {"new_string", "dog"}});
		REQUIRE(ambiguous.has_value());
		CHECK(ambiguous->is_error);
		CHECK(text_of(*ambiguous).find("appears 3 times") != std::string::npos);

		auto all = co_await r.call(
			"edit", {{"file_path", "e.txt"}, {"old_string", "cat"}, {"new_string", "dog"}, {"replace_all", true}});
		REQUIRE(all.has_value());
		CHECK_FALSE(all->is_error);
		CHECK(text_of(*all).find("All occurrences") != std::string::npos);

		auto read = co_await r.call("read", {{"file_path", "e.txt"}});
		REQUIRE(read.has_value());
		CHECK(text_of(*read).find("1: dog dog dog") != std::string::npos);
	});
}

TEST_CASE("glob matches by path and grep finds lines") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		rig r;
		co_await r.mount(rt, h, s.dir);
		co_await r.call("write", {{"file_path", "src/a.cpp"}, {"content", "int main() {}\n"}});
		co_await r.call("write", {{"file_path", "src/b.txt"}, {"content", "hello world\n"}});
		co_await r.call("write", {{"file_path", "top.txt"}, {"content", "hello there\n"}});

		auto glob = co_await r.call("glob", {{"pattern", "**/*.txt"}});
		REQUIRE(glob.has_value());
		CHECK_FALSE(glob->is_error);
		CHECK(text_of(*glob).find("src/b.txt") != std::string::npos);
		CHECK(text_of(*glob).find("top.txt") != std::string::npos);
		CHECK(text_of(*glob).find("a.cpp") == std::string::npos);

		auto grep = co_await r.call("grep", {{"pattern", "hello"}, {"include", "*.txt"}});
		REQUIRE(grep.has_value());
		CHECK_FALSE(grep->is_error);
		CHECK(text_of(*grep).find("Found 2 matches") != std::string::npos);
		CHECK(text_of(*grep).find("Line 1: hello") != std::string::npos);

		auto bad = co_await r.call("grep", {{"pattern", "("}});
		REQUIRE(bad.has_value());
		CHECK(bad->is_error);
		CHECK(text_of(*bad).find("invalid regular expression") != std::string::npos);
	});
}

TEST_CASE("disabled config leaves tools unregistered") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		rig r;
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.coreutil_spec({{"disabled", "glob,grep"}}));
		co_await rt.wait_idle();
		auto root = rt.root_context();
		auto tools = root.require<tools_service>(tools_key).shared();
		CHECK_FALSE(tools->find("glob").has_value());
		CHECK_FALSE(tools->find("grep").has_value());
		CHECK(tools->find("read").has_value());
	});
}
