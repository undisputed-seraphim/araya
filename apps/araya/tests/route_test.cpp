#include "input_route.hpp"
#include "metrics.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

// The pure input routing and palette helpers: slash classification,
// command parsing/filtering, the palette open condition, the feed's role
// markers, and title derivation. No plugins, no app library.
namespace {

using araya::app::command_info;

std::vector<command_info> const k_commands{
	{"ls", "ls", "list"},
	{"load", "load <component>", "load a component"},
	{"session", "session ...", "sessions"},
	{"say", "say <text>", "say something"},
};

std::span<command_info const> commands() { return k_commands; }

} // namespace

TEST_CASE("classify_input: empty and whitespace only") {
	CHECK(araya::app::classify_input("") == araya::app::input_kind::empty);
	CHECK(araya::app::classify_input("   ") == araya::app::input_kind::empty);
	CHECK(araya::app::classify_input("\t \t") == araya::app::input_kind::empty);
}

TEST_CASE("classify_input: slash lines are commands") {
	CHECK(araya::app::classify_input("/ls") == araya::app::input_kind::command);
	CHECK(araya::app::classify_input("/") == araya::app::input_kind::command);
	CHECK(araya::app::classify_input("  /help me") == araya::app::input_kind::command);
}

TEST_CASE("classify_input: anything else is a message") {
	CHECK(araya::app::classify_input("hello there") == araya::app::input_kind::message);
	CHECK(araya::app::classify_input("ls") == araya::app::input_kind::message); // no slash
	CHECK(araya::app::classify_input("what about /tmp?") == araya::app::input_kind::message);
}

TEST_CASE("parse_command: name and args") {
	auto load = araya::app::parse_command("/load logger");
	CHECK(load.name == "load");
	CHECK(load.args == "logger");

	auto say = araya::app::parse_command("/say  hello   world");
	CHECK(say.name == "say");
	CHECK(say.args == "hello   world");

	auto bare = araya::app::parse_command("/session");
	CHECK(bare.name == "session");
	CHECK(bare.args.empty());

	auto none = araya::app::parse_command("/");
	CHECK(none.name.empty());
	CHECK(none.args.empty());
}

TEST_CASE("filter_commands: prefix ranks above substring, case-insensitive") {
	auto all = araya::app::filter_commands(commands(), "");
	REQUIRE(all.size() == k_commands.size());
	CHECK(commands()[all[0]].name == "ls");

	auto matches = araya::app::filter_commands(commands(), "se");
	REQUIRE(matches.size() == 1);
	CHECK(commands()[matches[0]].name == "session");

	// "sa" is a prefix of say; "ls" contains no match.
	auto sa = araya::app::filter_commands(commands(), "SA");
	REQUIRE(sa.size() == 1);
	CHECK(commands()[sa[0]].name == "say");

	// Prefix match ("lo") beats an interior match would if present.
	auto lo = araya::app::filter_commands(commands(), "lo");
	REQUIRE(lo.size() == 1);
	CHECK(commands()[lo[0]].name == "load");

	CHECK(araya::app::filter_commands(commands(), "zzz").empty());
}

TEST_CASE("filter_commands: prefix before substring") {
	std::vector<command_info> const items{
		{"asend", "asend", ""},
		{"session", "session", ""},
	};
	auto matches = araya::app::filter_commands(items, "se");
	REQUIRE(matches.size() == 2);
	CHECK(items[matches[0]].name == "session"); // prefix
	CHECK(items[matches[1]].name == "asend");	// substring
}

TEST_CASE("palette_open: open while typing the command token") {
	CHECK(araya::app::palette_open("/"));
	CHECK(araya::app::palette_open("/se"));
	CHECK(araya::app::palette_open("/say"));
	CHECK_FALSE(araya::app::palette_open(""));
	CHECK_FALSE(araya::app::palette_open("hello"));
	CHECK_FALSE(araya::app::palette_open("/say hello"));
	CHECK_FALSE(araya::app::palette_open("/session "));
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

TEST_CASE("session_title: falls back when there is no user text") {
	CHECK(araya::app::session_title("", "ses_abc") == "ses_abc");
	CHECK(araya::app::session_title("   \n\t ", "ses_abc") == "ses_abc");
}

TEST_CASE("session_title: collapses whitespace runs") {
	CHECK(araya::app::session_title("  hello   world  ", "id") == "hello world");
	CHECK(araya::app::session_title("line one\nline two", "id") == "line one line two");
	CHECK(araya::app::session_title("a\t\tb\r\nc", "id") == "a b c");
}

TEST_CASE("session_title: truncates long text with an ellipsis") {
	std::string long_text(80, 'x');
	auto title = araya::app::session_title(long_text, "id");
	CHECK(title == std::string(48, 'x') + "\u2026");
}

TEST_CASE("session_title: truncates on a codepoint boundary") {
	// 47 ASCII bytes then a two-byte codepoint straddling the cut:
	// the 48th byte is a continuation byte, so the cut backs off to 47.
	std::string text = std::string(47, 'a') + "\u00e9" + "zzzz";
	CHECK(araya::app::session_title(text, "id") == std::string(47, 'a') + "\u2026");
}

TEST_CASE("token_count_text: in/out or a dash placeholder") {
	CHECK(araya::app::token_count_text(0, 0, false) == "--");
	CHECK(araya::app::token_count_text(4, 30, true) == "4/30");
	CHECK(araya::app::token_count_text(0, 0, true) == "0/0");
}

TEST_CASE("context_percent_text: whole percent or a dash placeholder") {
	CHECK(araya::app::context_percent_text(100, 1000, false) == "--%");
	CHECK(araya::app::context_percent_text(100, 0, true) == "--%");
	// A tiny prompt on a huge window rounds to 0%.
	CHECK(araya::app::context_percent_text(4, 524288, true) == "0%");
	CHECK(araya::app::context_percent_text(5000, 10000, true) == "50%");
	CHECK(araya::app::context_percent_text(10000, 10000, true) == "100%");
	// Nearest-percent rounding.
	CHECK(araya::app::context_percent_text(1, 3, true) == "33%");
	CHECK(araya::app::context_percent_text(2, 3, true) == "67%");
}

TEST_CASE("elide: short text is untouched, long text gets an ellipsis") {
	CHECK(araya::app::elide("gpt-4o", 10) == "gpt-4o");
	CHECK(araya::app::elide("abcdefghij", 5) == "abcde\u2026");
	// The cut backs off a split UTF-8 codepoint.
	CHECK(araya::app::elide(std::string(4, 'a') + "\u00e9" + "z", 5) == "aaaa\u2026");
}

TEST_CASE("wrap_lines: greedy word wrap") {
	using araya::app::wrap_lines;
	CHECK(wrap_lines("", 10) == std::vector<std::string>{""});
	CHECK(wrap_lines("a b c", 10) == std::vector<std::string>{"a b c"});
	CHECK(wrap_lines("a b c", 3) == std::vector<std::string>{"a b", "c"});
	// A word wider than the line is hard-split.
	CHECK(wrap_lines("abcdef", 4) == std::vector<std::string>{"abcd", "ef"});
	// Explicit newlines always break; spaces collapse.
	CHECK(wrap_lines("a\nb", 10) == std::vector<std::string>{"a", "b"});
	CHECK(wrap_lines("a\n", 10) == std::vector<std::string>{"a"});
	CHECK(wrap_lines("a   b", 10) == std::vector<std::string>{"a b"});
}

TEST_CASE("spinner_glyph: cycles the ascii and braille frames") {
	CHECK(araya::app::spinner_glyph(0, true) == "|");
	CHECK(araya::app::spinner_glyph(1, true) == "/");
	CHECK(araya::app::spinner_glyph(4, true) == "|");
	CHECK(araya::app::spinner_glyph(0, false) == "\u280b");
	CHECK(araya::app::spinner_glyph(10, false) == "\u280b");
}
