#pragma once

#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// The OpenAI-compatible chat-completions adapter: one instance serves
// every OpenAI-compatible endpoint (OpenAI, DeepSeek-chat, Ollama,
// vLLM, llama.cpp, OpenRouter). Configured through the plugin config:
// either `config_file` (path to JSON) or `config` (inline JSON), with
// sensible defaults when absent.
//
//   {
//     "provider": "openai",
//     "base_url": "https://api.openai.com/v1",
//     "api_key_env": "OPENAI_API_KEY",
//     "default_model": "gpt-4o-mini",
//     "idle_timeout_ms": 60000,
//     "connect_timeout_ms": 30000,
//     "verify_peer": true,
//     "models": { "gpt-4o-mini": {"context_window": 128000, "max_tokens": 16384} }
//   }
namespace araya::llm_openai {

struct openai_model {
	std::string id;
	std::string name; // display name; defaults to the id
	std::uint64_t context_window = 0;
	std::uint64_t default_max_tokens = 0;
	std::vector<std::string> reasoning_efforts;
	bool supports_image = false;
};

struct openai_config {
	std::string provider = "openai";
	std::string base_url = "https://api.openai.com/v1";
	std::string api_key_env = "OPENAI_API_KEY";
	std::string default_model = "gpt-4o-mini";
	std::chrono::milliseconds idle_timeout{60000};
	std::chrono::milliseconds connect_timeout{30000};
	bool verify_peer = true;
	std::vector<openai_model> models;
};

// Loads the config from the plugin config map (the `config_file` /
// `config` convention). Throws std::invalid_argument on unreadable
// files or malformed JSON - the mounting fiber reports it.
openai_config load_config(araya::plugin_config const& config);

class openai_adapter : public araya::llm::llm_adapter {
public:
	explicit openai_adapter(openai_config config);

	araya::task<void> stream(araya::llm::generate_options const& options, araya::llm::chunk_sink const& sink) override;

	std::vector<araya::llm::model_info> list_models(std::string_view provider) override;

	araya::llm::model_info resolve_model(std::string_view provider, std::string_view model) override;

	araya::llm::provider_info describe_provider(std::string_view provider) override;

private:
	openai_config config_;
};

// The plugin descriptor: requires `llm`, registers the adapter under the
// configured provider route.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::llm_openai
