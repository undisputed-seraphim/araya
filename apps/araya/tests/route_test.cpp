#include "input_route.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

// The pure input routing: how the TUI tells commands from conversation
// lines, and the feed's role markers. No plugins, no app library.
namespace {

bool known(std::string_view name) { return name == "ls" || name == "help" || name == "say"; }

araya::app::input_kind classify(std::string_view line) { return araya::app::classify_input(line, known); }

} // namespace

TEST_CASE("classify_input: empty and whitespace only") {
	CHECK(classify("") == araya::app::input_kind::empty);
	CHECK(classify("   ") == araya::app::input_kind::empty);
	CHECK(classify("\t \t") == araya::app::input_kind::empty);
}

TEST_CASE("classify_input: known command names") {
	CHECK(classify("ls") == araya::app::input_kind::command);
	CHECK(classify("help") == araya::app::input_kind::command);
	CHECK(classify("say hello") == araya::app::input_kind::command);
	CHECK(classify("  ls  ") == araya::app::input_kind::command);
	CHECK(classify("\tls -l") == araya::app::input_kind::command);
}

TEST_CASE("classify_input: free text is a message") {
	CHECK(classify("hello there") == araya::app::input_kind::message);
	CHECK(classify("what is the tech stack?") == araya::app::input_kind::message);
	// A command name must be the first token, not merely a prefix.
	CHECK(classify("lst") == araya::app::input_kind::message);
	CHECK(classify("lsfoo bar") == araya::app::input_kind::message);
}

TEST_CASE("role_marker: unicode and ascii tiers") {
	CHECK(araya::app::role_marker("user", false) == "\u276f");
	CHECK(araya::app::role_marker("assistant", false) == "\u25cf");
	CHECK(araya::app::role_marker("system", false) == "\u25cb");
	CHECK(araya::app::role_marker("tool", false) == "\u2699");

	CHECK(araya::app::role_marker("user", true) == ">");
	CHECK(araya::app::role_marker("assistant", true) == "*");
	CHECK(araya::app::role_marker("system", true) == "~");
	CHECK(araya::app::role_marker("tool", true) == "#");
	// The default (unknown role) renders as a user.
	CHECK(araya::app::role_marker("", true) == ">");
}
