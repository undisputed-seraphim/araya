#pragma once

#include "araya/llm/llm.hpp"
#include "araya/logger/log.hpp"
#include "araya/plugin.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The compatibility LLM adapter: one instance speaks both wire protocols
// through per-route profiles. `protocol` selects OpenAI Chat Completions
// (OpenAI, DeepSeek-chat, Ollama, vLLM, llama.cpp, OpenRouter) or Anthropic
// Messages (Anthropic, DeepSeek-anthropic, llama.cpp). Configured through the
// plugin config: either `config_file` (path to JSON) or `config` (inline
// JSON), with sensible defaults when absent.
//
//   {
//     "provider": "openai",
//     "protocol": "chat-completions",
//     "base_url": "https://api.openai.com/v1",
//     "api_key_env": "OPENAI_API_KEY",
//     "default_model": "gpt-4o-mini",
//     "idle_timeout_ms": 60000,
//     "connect_timeout_ms": 30000,
//     "verify_peer": true,
//     "models": { "gpt-4o-mini": {"context_window": 128000, "max_tokens": 16384} }
//   }
namespace araya::llm_compat {

// The wire protocol a provider route speaks.
enum class compat_protocol : std::uint8_t {
	chat_completions, // POST {base}/chat/completions (OpenAI-compatible)
	messages,		  // POST {base}/v1/messages (Anthropic-compatible)
};

struct compat_model {
	std::string id;
	std::string name; // display name; defaults to the id
	std::uint64_t context_window = 0;
	std::uint64_t default_max_tokens = 0;
	std::optional<double> default_temperature;
	std::optional<std::string> default_reasoning_effort;
	std::optional<std::string> system_prompt_update;
	std::vector<std::string> reasoning_efforts;
	bool supports_image = false;
	// USD per million tokens; absent for a free/local endpoint.
	std::optional<double> input_price;
	std::optional<double> output_price;
	std::optional<double> cache_read_price;
	std::optional<double> cache_write_price;
};

struct compat_config {
	std::string provider = "openai";
	compat_protocol protocol = compat_protocol::chat_completions;
	std::string base_url = "https://api.openai.com/v1";
	std::string api_key_env = "OPENAI_API_KEY";
	std::string default_model = "gpt-4o-mini";
	std::chrono::milliseconds idle_timeout{60000};
	std::chrono::milliseconds connect_timeout{30000};
	bool verify_peer = true;
	// Messages requires max_tokens; used when neither the request nor the
	// model supplies one.
	std::uint64_t fallback_max_tokens = 4096;
	// Messages reasoning: when positive, send the Anthropic-native
	// `thinking: {type:"enabled", budget_tokens:N}` instead of the
	// DeepSeek-style `output_config.effort`.
	std::uint64_t thinking_budget_tokens = 0;
	// The route's retry policy (`retryPolicy` in config); nullopt uses the
	// normal defaults.
	std::optional<araya::llm::retry_policy> retry;
	std::vector<compat_model> models;
};

// Loads the config from the plugin config map (the `config_file` /
// `config` convention). Throws std::invalid_argument on unreadable
// files or malformed JSON - the mounting fiber reports it. With a
// `providers` map this returns the first profile (use `load_routes` for
// every route).
compat_config load_config(araya::plugin_config const& config);

// One or more provider routes: the provider id (route key) and its profile.
// A flat config yields a single route under the profile's `provider`; a
// `providers` map yields one route per key.
using compat_routes = std::vector<std::pair<std::string, compat_config>>;
compat_routes load_routes(araya::plugin_config const& config);

class compat_adapter : public araya::llm::llm_adapter {
public:
	// A single-route adapter: the profile registers under its `provider`.
	explicit compat_adapter(compat_config config, araya::logger::log_handle log = {});
	// A dispatcher over multiple provider routes.
	explicit compat_adapter(compat_routes routes, araya::logger::log_handle log = {});

	araya::task<void> stream(araya::llm::generate_options const& options, araya::llm::chunk_sink const& sink) override;

	std::vector<araya::llm::model_info> list_models(std::string_view provider) override;

	araya::llm::model_info resolve_model(std::string_view provider, std::string_view model) override;

	araya::llm::provider_info describe_provider(std::string_view provider) override;

	std::optional<araya::llm::retry_policy> provider_retry_policy(std::string_view provider) override;

private:
	compat_config const& profile_for(std::string_view provider) const;

	std::map<std::string, compat_config, std::less<>> profiles_;
	araya::logger::log_handle log_;
};

// The plugin descriptor: requires `llm`, registers the adapter under the
// configured provider route.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::llm_compat
