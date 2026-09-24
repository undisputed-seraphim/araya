#include "araya/util/json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <limits>
#include <string>

// The null-safe field accessors, with an emphasis on integer kind
// tolerance: Boost.JSON parses a non-negative literal as int64 when it
// fits, so an unsigned getter that only accepts is_uint64() silently
// drops parsed config values.
namespace {

boost::json::object parse_object(std::string_view text) { return boost::json::parse(text).as_object(); }

} // namespace

TEST_CASE("get_uint reads a parsed non-negative integer (int64 kind)") {
	// 524288 comes back from boost::json's parser as int64, not uint64.
	auto object = parse_object(R"({"context_window":524288,"max_tokens":1024})");
	CHECK(araya::util::json::get_uint(object, "context_window") == 524288);
	CHECK(araya::util::json::get_uint(object, "max_tokens") == 1024);
	CHECK(araya::util::json::opt_uint(object, "context_window") == 524288);
}

TEST_CASE("get_uint reads a programmatically built uint64") {
	boost::json::object object;
	object["n"] = std::uint64_t{7};
	CHECK(araya::util::json::get_uint(object, "n") == 7);
}

TEST_CASE("get_uint rejects negatives, absent, and non-numbers") {
	auto object = parse_object(R"({"neg":-1,"text":"5","huge":18446744073709551615})");
	CHECK(araya::util::json::get_uint(object, "neg") == 0);
	CHECK_FALSE(araya::util::json::opt_uint(object, "neg").has_value());
	CHECK(araya::util::json::get_uint(object, "missing") == 0);
	CHECK_FALSE(araya::util::json::opt_uint(object, "missing").has_value());
	CHECK(araya::util::json::get_uint(object, "text") == 0);
	// Values above INT64_MAX really are stored as uint64.
	CHECK(araya::util::json::get_uint(object, "huge") == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("get_int reads both integer kinds within range") {
	auto object = parse_object(R"({"neg":-1,"pos":42,"huge":18446744073709551615})");
	CHECK(araya::util::json::get_int(object, "neg") == -1);
	CHECK(araya::util::json::get_int(object, "pos") == 42);
	CHECK(araya::util::json::get_int(object, "huge") == 0); // out of int64 range
	CHECK(araya::util::json::opt_int(object, "pos") == 42);
	CHECK_FALSE(araya::util::json::opt_int(object, "huge").has_value());
}
