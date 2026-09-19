#pragma once

#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

// The mock adapter: deterministic canned chunks for tests and demos,
// zero network. Configured through the plugin config map (flat keys):
//
//   provider        route name (default "mock")
//   model           advertised model id (default "mock-model")
//   response        canned reply; "{user}" expands to the last user
//                   message's text (default "echo: {user}")
//   fail            "1"/"true" to fail every request
//   fail_code       the failure code name (default "server")
//   fail_message    the failure message (default "mock failure")
//   input_tokens    usage to report (default 1)
//   output_tokens   usage to report (default 1)
//   delay_ms        pacing between chunks (default 0)
namespace araya::llm_mock {

struct mock_config {
	std::string provider = "mock";
	std::string model = "mock-model";
	std::string response = "echo: {user}";
	bool fail = false;
	araya::llm::llm_error_code fail_code = araya::llm::llm_error_code::server;
	std::string fail_message = "mock failure";
	std::uint64_t input_tokens = 1;
	std::uint64_t output_tokens = 1;
	std::chrono::milliseconds delay{0};
};

// Parses the flat config map. Throws std::invalid_argument on unknown
// fail_code names - the mounting fiber reports it.
mock_config load_config(araya::plugin_config const& config);

class mock_adapter : public araya::llm::llm_adapter {
public:
	explicit mock_adapter(mock_config config);

	araya::task<void> stream(araya::llm::generate_options const& options, araya::llm::chunk_sink const& sink) override;

	araya::llm::model_info resolve_model(std::string_view provider, std::string_view model) override;

	araya::llm::provider_info describe_provider(std::string_view provider) override;

private:
	mock_config config_;
};

// The plugin descriptor: requires `llm`, registers the adapter under
// the configured provider route.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::llm_mock
