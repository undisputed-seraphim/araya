#pragma once

#include "araya/plugin.hpp"

#include <charconv>
#include <concepts>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace araya {

// A failed config lookup or parse, carrying the key, the offending value,
// and a short reason. Mirrors resolution_error: plugin authors see which
// key of which value failed instead of a bare std::invalid_argument.
class config_error : public std::runtime_error {
public:
    config_error(std::string_view key, std::string_view value,
                 std::string_view reason)
        : std::runtime_error("config key '" + std::string(key) +
                             "' value '" + std::string(value) +
                             "': " + std::string(reason)),
          key_(key),
          value_(value),
          reason_(reason) {}

    std::string_view key() const noexcept { return key_; }
    std::string_view value() const noexcept { return value_; }
    std::string_view reason() const noexcept { return reason_; }

private:
    std::string key_;
    std::string value_;
    std::string reason_;
};

// A compile-time typed config key, the config-side counterpart of
// service_key<T>: the value type is bound to the key at declaration, so
// accessing a key with the wrong type does not compile.
template <class T>
struct config_key {
    using value_type = T;

    std::string_view name;

    constexpr config_key(std::string_view n) noexcept : name(n) {}
};

// Parser customization point. The primary template is undefined; the
// built-ins cover the arithmetic types (std::from_chars, strict: no
// whitespace, no trailing garbage, overflow checked), bool ("true",
// "false", "1", "0"), std::string (a copy), std::string_view (a view
// into the config map's storage — valid while the entry lives), and enum
// types (via their underlying type). Plugin authors may specialize for
// their own types (e.g. a word-valued enum).
template <class T>
struct config_parser;

template <std::integral T>
struct config_parser<T> {
    static std::optional<T> parse(std::string_view text) {
        T value{};
        auto [ptr, ec] = std::from_chars(text.data(),
                                         text.data() + text.size(), value);
        if (ec != std::errc{} || ptr != text.data() + text.size())
            return std::nullopt;
        return value;
    }
};

template <std::floating_point T>
struct config_parser<T> {
    static std::optional<T> parse(std::string_view text) {
        T value{};
        auto [ptr, ec] =
            std::from_chars(text.data(), text.data() + text.size(), value,
                            std::chars_format::general);
        if (ec != std::errc{} || ptr != text.data() + text.size())
            return std::nullopt;
        return value;
    }
};

template <>
struct config_parser<bool> {
    static std::optional<bool> parse(std::string_view text) {
        if (text == "true" || text == "1")
            return true;
        if (text == "false" || text == "0")
            return false;
        return std::nullopt;
    }
};

template <>
struct config_parser<std::string> {
    static std::optional<std::string> parse(std::string_view text) {
        return std::string(text);
    }
};

template <>
struct config_parser<std::string_view> {
    static std::optional<std::string_view> parse(std::string_view text) {
        return text;
    }
};

template <class T>
    requires std::is_enum_v<T>
struct config_parser<T> {
    static std::optional<T> parse(std::string_view text) {
        auto parsed = config_parser<std::underlying_type_t<T>>::parse(text);
        if (!parsed)
            return std::nullopt;
        return static_cast<T>(*parsed);
    }
};

// The free-function form of config parsing, for argv and other
// non-plugin_config inputs.
template <class T>
std::optional<T> parse_value(std::string_view text) {
    return config_parser<T>::parse(text);
}

// Read-only typed access over a plugin_config. get()/operator[] throw
// config_error for absent and malformed values; try_get() returns
// nullopt only when the key is absent — a present but malformed value
// still throws ("optional presence, strict values").
class plugin_config_view {
public:
    explicit plugin_config_view(plugin_config const& cfg) noexcept
        : cfg_(&cfg) {}

    // The underlying map, for string access and edge cases.
    plugin_config const& raw() const noexcept { return *cfg_; }

    bool contains(std::string_view key) const noexcept {
        return cfg_->find(std::string(key)) != cfg_->end();
    }

    template <class T>
    T operator[](config_key<T> const& key) const {
        auto it = find(key.name);
        if (it == cfg_->end())
            throw config_error(key.name, "", "missing");
        auto parsed = config_parser<T>::parse(it->second);
        if (!parsed)
            throw config_error(key.name, it->second, "malformed");
        return std::move(*parsed);
    }

    template <class T>
    std::optional<T> try_get(config_key<T> const& key) const {
        auto it = find(key.name);
        if (it == cfg_->end())
            return std::nullopt;
        auto parsed = config_parser<T>::parse(it->second);
        if (!parsed)
            throw config_error(key.name, it->second, "malformed");
        return std::move(*parsed);
    }

private:
    plugin_config::const_iterator find(std::string_view key) const {
        return cfg_->find(std::string(key));
    }

    plugin_config const* cfg_;
};

}  // namespace araya
