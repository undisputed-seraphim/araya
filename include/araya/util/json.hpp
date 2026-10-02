#pragma once

#include <boost/json/value.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

// Null-safe Boost.JSON field accessors: absent or wrong-typed fields
// read as the neutral value ("" / 0 / false / nullptr). Call sites stop
// repeating the if_contains()->is_*() dance.
//
// Integer kind tolerance: Boost.JSON parses a non-negative integer
// literal as int64 when it fits (only values above INT64_MAX become
// uint64), so the unsigned getters accept either kind and the signed
// getters accept an in-range uint64. Without this, a config value like
// 524288 read back from parsed text reports is_uint64()==false and is
// silently treated as absent.
namespace araya::util::json {

inline boost::json::object const* as_object(boost::json::value const& value) { return value.if_object(); }

inline boost::json::array const* as_array(boost::json::value const& value) { return value.if_array(); }

// Value-level accessors: absent (null) or wrong-typed reads as the neutral
// value. These are the primitive forms; the object+key overloads below
// delegate to them.
inline std::string get_string(boost::json::value const& value) {
	return value.is_string() ? std::string(value.as_string()) : std::string{};
}

inline std::optional<std::string> opt_string(boost::json::value const& value) {
	return value.is_string() ? std::optional<std::string>(std::string(value.as_string())) : std::nullopt;
}

inline std::uint64_t get_uint(boost::json::value const& value) {
	if (value.is_uint64())
		return value.as_uint64();
	if (value.is_int64() && value.as_int64() >= 0)
		return static_cast<std::uint64_t>(value.as_int64());
	return 0;
}

inline std::optional<std::uint64_t> opt_uint(boost::json::value const& value) {
	if (value.is_uint64())
		return value.as_uint64();
	if (value.is_int64() && value.as_int64() >= 0)
		return static_cast<std::uint64_t>(value.as_int64());
	return std::nullopt;
}

inline std::int64_t get_int(boost::json::value const& value) {
	if (value.is_int64())
		return value.as_int64();
	if (value.is_uint64() && value.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
		return static_cast<std::int64_t>(value.as_uint64());
	return 0;
}

inline std::optional<std::int64_t> opt_int(boost::json::value const& value) {
	if (value.is_int64())
		return value.as_int64();
	if (value.is_uint64() && value.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
		return static_cast<std::int64_t>(value.as_uint64());
	return std::nullopt;
}

inline bool get_bool(boost::json::value const& value) { return value.is_bool() && value.as_bool(); }

inline std::optional<bool> opt_bool(boost::json::value const& value) {
	return value.is_bool() ? std::optional<bool>(value.as_bool()) : std::nullopt;
}

// Object+key accessors.
inline boost::json::object const* get_object(boost::json::object const& object, std::string_view key) {
	auto const* node = object.if_contains(key);
	return node ? node->if_object() : nullptr;
}

inline boost::json::array const* get_array(boost::json::object const& object, std::string_view key) {
	auto const* node = object.if_contains(key);
	return node ? node->if_array() : nullptr;
}

inline std::string get_string(boost::json::object const& object, std::string_view key) {
	auto const* node = object.if_contains(key);
	return node ? get_string(*node) : std::string{};
}

inline std::optional<std::string> opt_string(boost::json::object const& object, std::string_view key) {
	auto const* node = object.if_contains(key);
	return node ? opt_string(*node) : std::nullopt;
}

inline std::uint64_t get_uint(boost::json::object const& object, std::string_view key) {
	auto const* node = object.if_contains(key);
	return node ? get_uint(*node) : 0;
}

inline std::optional<std::uint64_t> opt_uint(boost::json::object const& object, std::string_view key) {
	auto const* node = object.if_contains(key);
	return node ? opt_uint(*node) : std::nullopt;
}

inline std::int64_t get_int(boost::json::object const& object, std::string_view key) {
	auto const* node = object.if_contains(key);
	return node ? get_int(*node) : 0;
}

inline std::optional<std::int64_t> opt_int(boost::json::object const& object, std::string_view key) {
	auto const* node = object.if_contains(key);
	return node ? opt_int(*node) : std::nullopt;
}

inline bool get_bool(boost::json::object const& object, std::string_view key) {
	auto const* node = object.if_contains(key);
	return node ? get_bool(*node) : false;
}

inline std::optional<bool> opt_bool(boost::json::object const& object, std::string_view key) {
	auto const* node = object.if_contains(key);
	return node ? opt_bool(*node) : std::nullopt;
}

} // namespace araya::util::json
